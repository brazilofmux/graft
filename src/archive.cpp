// archive.cpp: the .gfa versioned archive.
//
// Append-only: each `add` writes the new contents (full zstd copies or .gft
// deltas against earlier contents), then a segment listing them and the new
// version, then a footer pointing at the previous footer. Opening walks the
// footer chain back from the end; a torn tail from an interrupted add is
// ignored and trimmed by the next add.

#include "archive_format.h"

#include <zstd.h>

#include <chrono>
#include <ctime>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace graft::arc {

struct State {
    fs::path path;
    File f;
    Log log{nullptr, nullptr};
    bool writable = false;
    std::vector<Object> objs;
    std::vector<Version> versions;
    uint64_t end = kHeaderSize;       // end of the valid data (just past the last footer)
    uint64_t last_footer = kNoPrev;
    uint64_t torn = 0;                 // bytes past `end` (an interrupted add)

    // Recently materialized contents, bounded by total size.
    std::list<std::pair<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>> cache;
    size_t cache_bytes = 0;
    static constexpr size_t kCacheBudget = 256u << 20;
};

namespace {

graft_status read_at(std::FILE *f, uint64_t off, void *buf, size_t n) {
    if (n == 0) return GRAFT_OK;
    if (!file_seek(f, off) || std::fread(buf, 1, n, f) != n) return GRAFT_E_IO;
    return GRAFT_OK;
}

// A footer that ends at `at + kFooterSize`, with a matching segment before it.
bool verify_footer(State &s, uint64_t at, Footer &ft) {
    uint8_t raw[kFooterSize];
    if (at < kHeaderSize || read_at(s.f.get(), at, raw, sizeof raw) != GRAFT_OK || !decode_footer(raw, ft))
        return false;
    if (ft.offset < kHeaderSize || ft.comp == 0 || ft.offset > at || ft.comp != at - ft.offset) return false;
    if (ft.raw > kMaxSegment || ft.comp > ZSTD_compressBound(static_cast<size_t>(ft.raw))) return false;
    if (ft.prev != kNoPrev && (ft.prev < kHeaderSize || ft.prev > ft.offset || ft.offset - ft.prev < kFooterSize))
        return false;
    std::vector<uint8_t> comp(static_cast<size_t>(ft.comp));
    if (read_at(s.f.get(), ft.offset, comp.data(), comp.size()) != GRAFT_OK) return false;
    Hash h;
    hash_bytes(comp.data(), comp.size(), {}, h);
    return !kCheckHashes || h == ft.hash;
}

// The last valid footer: at the very end, or (after an interrupted add)
// further back. A footer at the very end that fails its checks is damage,
// not an interruption.
graft_status find_last_footer(State &s, uint64_t size, uint64_t &at) {
    at = kNoPrev;
    if (size == kHeaderSize) return GRAFT_OK;
    Footer ft;
    if (size >= kHeaderSize + kFooterSize) {
        uint8_t raw[kFooterSize];
        if (read_at(s.f.get(), size - kFooterSize, raw, sizeof raw) != GRAFT_OK) return GRAFT_E_IO;
        if (decode_footer(raw, ft)) {
            if (!verify_footer(s, size - kFooterSize, ft)) return GRAFT_E_CORRUPT;
            at = size - kFooterSize;
            return GRAFT_OK;
        }
    }
    // Scan back for the end magic of an earlier footer.
    const size_t chunk = 1 << 20;
    std::vector<uint8_t> buf;
    uint64_t hi = size;  // search for magics ending at or before hi
    while (hi > kHeaderSize + kFooterSize - 1) {
        const uint64_t lo = std::max<uint64_t>(kHeaderSize, hi > chunk ? hi - chunk : 0);
        buf.resize(static_cast<size_t>(hi - lo));
        if (read_at(s.f.get(), lo, buf.data(), buf.size()) != GRAFT_OK) return GRAFT_E_IO;
        for (size_t i = buf.size(); i >= sizeof kEndMagic; --i) {
            if (std::memcmp(buf.data() + i - sizeof kEndMagic, kEndMagic, sizeof kEndMagic) != 0) continue;
            const uint64_t end = lo + i;
            if (end < kHeaderSize + kFooterSize) break;
            if (verify_footer(s, end - kFooterSize, ft)) {
                at = end - kFooterSize;
                return GRAFT_OK;
            }
        }
        if (lo == kHeaderSize) break;
        hi = lo + sizeof kEndMagic - 1;  // overlap so a magic across the boundary is seen
    }
    return GRAFT_OK;  // no footer: an archive whose first add was interrupted
}

graft_status load(State &s) {
    std::error_code ec;
    const uint64_t size = fs::file_size(s.path, ec);
    if (ec) return GRAFT_E_IO;
    uint8_t hdr[kHeaderSize];
    if (size < kHeaderSize || read_at(s.f.get(), 0, hdr, sizeof hdr) != GRAFT_OK) return GRAFT_E_CORRUPT;
    if (std::memcmp(hdr, kMagic, 8) != 0) return GRAFT_E_CORRUPT;
    if (get_le<uint16_t>(hdr + 8) != kVersion || get_le<uint16_t>(hdr + 10) != 0) return GRAFT_E_UNSUPPORTED;

    uint64_t at;
    if (graft_status st = find_last_footer(s, size, at); st != GRAFT_OK) return st;
    s.last_footer = at;
    s.end = at == kNoPrev ? kHeaderSize : at + kFooterSize;
    s.torn = size - s.end;
    if (s.torn)
        s.log(GRAFT_LOG_WARN, "ignoring " + std::to_string(s.torn) + " trailing bytes (an interrupted add?)");

    std::vector<Footer> chain;
    for (uint64_t p = at; p != kNoPrev;) {
        Footer ft;
        if (!verify_footer(s, p, ft)) return GRAFT_E_CORRUPT;
        if (!chain.empty() && p >= chain.back().offset) return GRAFT_E_CORRUPT;
        chain.push_back(ft);
        p = ft.prev;
    }
    std::set<std::string> labels;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        std::vector<uint8_t> comp(static_cast<size_t>(it->comp)), raw(static_cast<size_t>(it->raw));
        if (graft_status st = read_at(s.f.get(), it->offset, comp.data(), comp.size()); st != GRAFT_OK) return st;
        const size_t n = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
        if (ZSTD_isError(n) || n != raw.size()) return GRAFT_E_CORRUPT;
        Version v;
        if (!decode_segment(raw.data(), raw.size(), s.objs, v, it->offset)) return GRAFT_E_CORRUPT;
        if (!labels.insert(v.label).second) return GRAFT_E_CORRUPT;
        s.versions.push_back(std::move(v));
    }
    return GRAFT_OK;
}

using Content = std::shared_ptr<const std::vector<uint8_t>>;

graft_status materialize(State &s, uint64_t id, Content &out) {
    for (auto it = s.cache.begin(); it != s.cache.end(); ++it) {
        if (it->first == id) {
            s.cache.splice(s.cache.begin(), s.cache, it);
            out = it->second;
            return GRAFT_OK;
        }
    }
    const Object &o = s.objs[static_cast<size_t>(id)];
    std::vector<uint8_t> stored(static_cast<size_t>(o.length));
    if (graft_status st = read_at(s.f.get(), o.offset, stored.data(), stored.size()); st != GRAFT_OK) return st;
    auto data = std::make_shared<std::vector<uint8_t>>();
    if (o.kind == kObjFull) {
        const unsigned long long declared = ZSTD_getFrameContentSize(stored.data(), stored.size());
        if (declared != o.size) return GRAFT_E_CORRUPT;
        data->resize(static_cast<size_t>(o.size));
        const size_t n = ZSTD_decompress(data->data(), data->size(), stored.data(), stored.size());
        if (ZSTD_isError(n) || n != o.size) return GRAFT_E_CORRUPT;
    } else {
        Content base;
        if (graft_status st = materialize(s, o.base, base); st != GRAFT_OK) return st;
        graft_source src = graft_source_from_memory(base->data(), base->size());
        struct Mem {
            const std::vector<uint8_t> *v;
            size_t pos = 0;
        } mem{&stored};
        graft_reader rd{&mem, [](void *ctx, void *buf, size_t cap, size_t *got) {
                            auto *m = static_cast<Mem *>(ctx);
                            *got = std::min(cap, m->v->size() - m->pos);
                            std::memcpy(buf, m->v->data() + m->pos, *got);
                            m->pos += *got;
                            return GRAFT_OK;
                        }};
        graft_sink sk{data.get(), [](void *ctx, const void *p, size_t n) {
                          auto *v = static_cast<std::vector<uint8_t> *>(ctx);
                          v->insert(v->end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
                          return GRAFT_OK;
                      }};
        graft_apply_limits lim;
        graft_apply_limits_init(&lim);
        lim.max_target_size = o.size;
        lim.max_stream = std::max<uint64_t>(lim.max_stream, std::min<uint64_t>(o.size, 1u << 30));
        if (graft_status st = graft_apply(&src, &rd, &sk, &lim); st != GRAFT_OK)
            return st == GRAFT_E_WRONG_SOURCE ? GRAFT_E_CORRUPT : st;
    }
    Hash h;
    hash_bytes(data->data(), data->size(), {}, h);
    if (data->size() != o.size || (kCheckHashes && h != o.hash)) return GRAFT_E_CORRUPT;

    out = data;
    s.cache.emplace_front(id, data);
    s.cache_bytes += data->size();
    while (s.cache_bytes > State::kCacheBudget && s.cache.size() > 1) {
        s.cache_bytes -= s.cache.back().second->size();
        s.cache.pop_back();
    }
    return GRAFT_OK;
}

const Version *find_version(const State &s, const std::string &label) {
    for (const auto &v : s.versions)
        if (v.label == label) return &v;
    return nullptr;
}

// --- adding ---------------------------------------------------------------------------

struct Item {
    VEntry entry;  // object filled in by add
    Hash hash{};
    uint64_t size = 0;
    std::function<graft_status(std::vector<uint8_t> &)> fetch;  // files only
};

graft_status append(State &s, uint64_t &pos, const void *p, size_t n) {
    if (!file_seek(s.f.get(), pos) || std::fwrite(p, 1, n, s.f.get()) != n) return GRAFT_E_IO;
    pos += n;
    return GRAFT_OK;
}

graft_status add_version(State &s, const std::string &label, uint64_t time, std::vector<Item> &items, int level) {
    if (!valid_label(label)) {
        s.log(GRAFT_LOG_ERROR, "bad label '" + label + "'");
        return GRAFT_E_INVALID_ARG;
    }
    if (find_version(s, label)) {
        s.log(GRAFT_LOG_ERROR, "the archive already has a version '" + label + "'");
        return GRAFT_E_INVALID_ARG;
    }
    if (s.torn) {
        std::fflush(s.f.get());
        std::error_code ec;
        fs::resize_file(s.path, s.end, ec);
        if (ec) return GRAFT_E_IO;
        s.log(GRAFT_LOG_INFO, "trimmed " + std::to_string(s.torn) + " bytes left by an interrupted add");
        s.torn = 0;
    }

    std::map<Hash, uint64_t> by_hash;
    for (size_t i = 0; i < s.objs.size(); ++i) by_hash.emplace(s.objs[i].hash, i);
    std::map<std::string, uint64_t> prev_path, prev_name;
    if (!s.versions.empty())
        for (const auto &e : s.versions.back().entries)
            if (e.kind == kNodeFile) {
                prev_path.emplace(e.path, e.object);
                prev_name.emplace(fs::path(e.path).filename().string(), e.object);
            }

    const uint64_t first = s.objs.size();
    std::vector<Object> fresh;
    uint64_t pos = s.end;
    uint64_t stored_bytes = 0, reused = 0;
    std::unique_ptr<ZSTD_CCtx, size_t (*)(ZSTD_CCtx *)> cctx(ZSTD_createCCtx(), ZSTD_freeCCtx);
    if (!cctx) return GRAFT_E_NOMEM;
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level);

    for (auto &it : items) {
        if (it.entry.kind != kNodeFile) continue;
        if (auto h = by_hash.find(it.hash); h != by_hash.end()) {
            it.entry.object = h->second;
            ++reused;
            continue;
        }
        std::vector<uint8_t> data;
        if (graft_status st = it.fetch(data); st != GRAFT_OK) return st;
        Hash check;
        hash_bytes(data.data(), data.size(), {}, check);
        if (check != it.hash || data.size() != it.size) {
            s.log(GRAFT_LOG_ERROR, it.entry.path + ": changed while being added");
            return GRAFT_E_IO;
        }

        std::vector<uint8_t> full(ZSTD_compressBound(data.size()));
        const size_t fn = ZSTD_compress2(cctx.get(), full.data(), full.size(), data.data(), data.size());
        if (ZSTD_isError(fn)) return GRAFT_E_NOMEM;
        full.resize(fn);

        // A delta against the same path (or name) in the previous version,
        // if its chain is short enough; kept only if smaller than a full copy.
        Object o;
        o.hash = it.hash;
        o.size = data.size();
        const std::vector<uint8_t> *chosen = &full;
        std::vector<uint8_t> patch;
        uint64_t base = UINT64_MAX;
        if (auto p = prev_path.find(it.entry.path); p != prev_path.end()) base = p->second;
        else if (auto q = prev_name.find(fs::path(it.entry.path).filename().string()); q != prev_name.end())
            base = q->second;
        if (base != UINT64_MAX && s.objs[static_cast<size_t>(base)].depth < kDefaultDepth) {
            Content bc;
            if (graft_status st = materialize(s, base, bc); st != GRAFT_OK) return st;
            graft_diff_options d;
            graft_diff_options_init(&d);
            d.level = level;
            graft_sink sk{&patch, [](void *ctx, const void *p, size_t n) {
                              auto *v = static_cast<std::vector<uint8_t> *>(ctx);
                              v->insert(v->end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
                              return GRAFT_OK;
                          }};
            if (graft_status st = graft_diff(bc->data(), bc->size(), data.data(), data.size(), &d, &sk);
                st != GRAFT_OK)
                return st;
            if (patch.size() < full.size()) {
                chosen = &patch;
                o.kind = kObjDelta;
                o.base = base;
                o.depth = s.objs[static_cast<size_t>(base)].depth + 1;
            }
        }
        o.offset = pos;
        o.length = chosen->size();
        if (graft_status st = append(s, pos, chosen->data(), chosen->size()); st != GRAFT_OK) return st;
        stored_bytes += chosen->size();
        it.entry.object = first + fresh.size();
        by_hash.emplace(o.hash, it.entry.object);
        fresh.push_back(o);
        s.objs.push_back(o);  // so later items in this add can use it as a base
    }

    Version v;
    v.label = label;
    v.time = time;
    for (auto &it : items) v.entries.push_back(it.entry);

    auto rollback_mem = [&] { s.objs.resize(static_cast<size_t>(first)); };
    std::vector<uint8_t> raw, comp;
    encode_segment(fresh, v, raw);
    if (raw.size() > kMaxSegment) {
        rollback_mem();
        return GRAFT_E_LIMIT;
    }
    comp.resize(ZSTD_compressBound(raw.size()));
    const size_t cn = ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(cn)) {
        rollback_mem();
        return GRAFT_E_NOMEM;
    }
    comp.resize(cn);
    // Contents first, synced; then the segment and footer that make them reachable.
    if (sync_file(s.f.get()) != GRAFT_OK) {
        rollback_mem();
        return GRAFT_E_IO;
    }
    Footer ft;
    ft.offset = pos;
    ft.comp = comp.size();
    ft.raw = raw.size();
    ft.prev = s.last_footer;
    hash_bytes(comp.data(), comp.size(), {}, ft.hash);
    uint8_t fraw[kFooterSize];
    encode_footer(ft, fraw);
    if (append(s, pos, comp.data(), comp.size()) != GRAFT_OK || append(s, pos, fraw, sizeof fraw) != GRAFT_OK ||
        sync_file(s.f.get()) != GRAFT_OK) {
        rollback_mem();
        s.torn = pos - s.end;  // whatever got written is a torn tail now
        return GRAFT_E_IO;
    }
    s.last_footer = pos - kFooterSize;
    s.end = pos;
    s.versions.push_back(std::move(v));
    s.log(GRAFT_LOG_INFO, "added " + label + ": " + std::to_string(fresh.size()) + " new contents (" +
                              std::to_string(stored_bytes) + " bytes stored), " + std::to_string(reused) +
                              " unchanged");
    return GRAFT_OK;
}

graft_status items_from_dir(const fs::path &dir, const Log &log, std::vector<Item> &items) {
    Tree tree;
    if (graft_status st = scan_tree(dir, tree, log); st != GRAFT_OK) return st;
    std::set<std::string> folded;
    for (const auto &[p, n] : tree) {
        if (!folded.insert(fold_case(p)).second) {
            log(GRAFT_LOG_ERROR, p + ": differs only by case from another path");
            return GRAFT_E_INVALID_ARG;
        }
        Item it;
        it.entry.path = p;
        it.entry.kind = n.kind;
        it.entry.mode = n.mode;
        it.entry.link = n.link;
        it.hash = n.hash;
        it.size = n.size;
        const fs::path full = dir / p;
        it.fetch = [full](std::vector<uint8_t> &out) { return read_whole(full, out); };
        items.push_back(std::move(it));
    }
    return GRAFT_OK;
}

uint64_t now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// --- extracting -----------------------------------------------------------------------

void set_mode(const fs::path &p, uint32_t mode, std::error_code &ec) {
#if defined(_WIN32)
    (void)p;
    (void)mode;
    ec.clear();
#else
    fs::permissions(p, static_cast<fs::perms>(mode), ec);
#endif
}

graft_status extract_into(State &s, const Version &v, const fs::path &dir) {
    std::error_code ec;
    for (const auto &e : v.entries) {
        const fs::path p = dir / e.path;
        if (e.kind == kNodeDir) {
            if (!fs::create_directory(p, ec) || ec) return GRAFT_E_IO;
        } else if (e.kind == kNodeSymlink) {
            fs::create_symlink(e.link, p, ec);
            if (ec) return GRAFT_E_IO;
        } else {
            Content c;
            if (graft_status st = materialize(s, e.object, c); st != GRAFT_OK) return st;
            File out;
            if (!out.open(p, "wbx")) return GRAFT_E_IO;
            if (!c->empty() && std::fwrite(c->data(), 1, c->size(), out.get()) != c->size()) return GRAFT_E_IO;
            if (!out.close()) return GRAFT_E_IO;
            set_mode(p, e.mode, ec);
            if (ec) return GRAFT_E_IO;
        }
    }
    // Directory modes last, deepest first, so a read-only directory can still be filled.
    for (auto it = v.entries.rbegin(); it != v.entries.rend(); ++it) {
        if (it->kind != kNodeDir) continue;
        set_mode(dir / it->path, it->mode, ec);
        if (ec) return GRAFT_E_IO;
    }
    return GRAFT_OK;
}

// Extracts to a sibling temporary directory and renames it into place.
graft_status extract(State &s, const std::string &label, const fs::path &dir) {
    const Version *v = find_version(s, label);
    if (!v) {
        s.log(GRAFT_LOG_ERROR, "no version '" + label + "'");
        return GRAFT_E_INVALID_ARG;
    }
    std::error_code ec;
    if (fs::symlink_status(dir, ec).type() != fs::file_type::not_found) {
        s.log(GRAFT_LOG_ERROR, dir.string() + ": already exists");
        return GRAFT_E_INVALID_ARG;
    }
    const fs::path tmp = dir.string() + ".graft-tmp";
    if (!fs::create_directory(tmp, ec) || ec) {
        s.log(GRAFT_LOG_ERROR, tmp.string() + ": can't create (left over from an earlier run?)");
        return GRAFT_E_IO;
    }
    graft_status st = extract_into(s, *v, tmp);
    if (st == GRAFT_OK) {
        fs::rename(tmp, dir, ec);
        if (ec) st = GRAFT_E_IO;
    }
    if (st != GRAFT_OK) {
        // Read-only directories would block removal; open them up first.
        for (auto it = fs::recursive_directory_iterator(tmp, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_directory() && !it->is_symlink()) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::permissions(tmp, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(tmp, ec);
    }
    return st;
}

std::string random_suffix() {
    std::random_device rd;
    return std::to_string(rd()) + std::to_string(rd());
}

}  // namespace
}  // namespace graft::arc

struct graft_archive {
    graft::arc::State s;
};

namespace {

template <class F>
graft_status guarded(F &&f) {
    try {
        return f();
    } catch (const std::bad_alloc &) {
        return GRAFT_E_NOMEM;
    } catch (const std::length_error &) {
        return GRAFT_E_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return GRAFT_E_IO;
    }
}

graft_status open_impl(const char *path, uint32_t flags, graft_log_fn log, void *log_ctx, graft_archive **out) {
    namespace fs = std::filesystem;
    auto a = std::make_unique<graft_archive>();
    auto &s = a->s;
    s.path = path;
    s.log = graft::Log(log, log_ctx);
    s.writable = (flags & (GRAFT_ARCHIVE_WRITE | GRAFT_ARCHIVE_CREATE)) != 0;
    std::error_code ec;
    if (fs::symlink_status(s.path, ec).type() == fs::file_type::not_found) {
        if (!(flags & GRAFT_ARCHIVE_CREATE)) {
            s.log(GRAFT_LOG_ERROR, std::string(path) + ": no such archive");
            return GRAFT_E_IO;
        }
        graft::File f;
        if (!f.open(s.path, "wbx")) return GRAFT_E_IO;
        uint8_t hdr[graft::arc::kHeaderSize];
        std::memcpy(hdr, graft::arc::kMagic, 8);
        graft::put_le<uint16_t>(hdr + 8, graft::arc::kVersion);
        graft::put_le<uint16_t>(hdr + 10, 0);
        if (std::fwrite(hdr, 1, sizeof hdr, f.get()) != sizeof hdr || graft::sync_file(f.get()) != GRAFT_OK ||
            !f.close())
            return GRAFT_E_IO;
    }
    if (!s.f.open(s.path, s.writable ? "r+b" : "rb")) {
        s.log(GRAFT_LOG_ERROR, std::string(path) + ": can't open");
        return GRAFT_E_IO;
    }
    if (graft_status st = graft::arc::load(s); st != GRAFT_OK) {
        if (st == GRAFT_E_CORRUPT) s.log(GRAFT_LOG_ERROR, std::string(path) + ": damaged or not an archive");
        return st;
    }
    *out = a.release();
    return GRAFT_OK;
}

}  // namespace

extern "C" graft_status graft_archive_open(const char *path, uint32_t flags, graft_log_fn log, void *log_ctx,
                                           graft_archive **out) {
    if (!path || !out) return GRAFT_E_INVALID_ARG;
    *out = nullptr;
    return guarded([&] { return open_impl(path, flags, log, log_ctx, out); });
}

extern "C" void graft_archive_close(graft_archive *a) { delete a; }

extern "C" size_t graft_archive_count(const graft_archive *a) { return a ? a->s.versions.size() : 0; }

extern "C" graft_status graft_archive_get(const graft_archive *a, size_t i, graft_archive_version *out) {
    if (!a || !out || i >= a->s.versions.size()) return GRAFT_E_INVALID_ARG;
    const auto &v = a->s.versions[i];
    out->label = v.label.c_str();
    out->time = v.time;
    out->file_count = 0;
    out->total_size = 0;
    for (const auto &e : v.entries)
        if (e.kind == graft::kNodeFile) {
            ++out->file_count;
            out->total_size += a->s.objs[static_cast<size_t>(e.object)].size;
        }
    return GRAFT_OK;
}

extern "C" graft_status graft_archive_add(graft_archive *a, const char *dir, const char *label, int level) {
    if (!a || !dir || !label || level < 1 || level > ZSTD_maxCLevel()) return GRAFT_E_INVALID_ARG;
    if (!a->s.writable) return GRAFT_E_INVALID_ARG;
    return guarded([&] {
        std::vector<graft::arc::Item> items;
        if (graft_status st = graft::arc::items_from_dir(dir, a->s.log, items); st != GRAFT_OK) return st;
        return graft::arc::add_version(a->s, label, graft::arc::now(), items, level);
    });
}

extern "C" graft_status graft_archive_extract(graft_archive *a, const char *label, const char *dir) {
    if (!a || !label || !dir) return GRAFT_E_INVALID_ARG;
    return guarded([&] { return graft::arc::extract(a->s, label, dir); });
}

extern "C" graft_status graft_archive_diff(graft_archive *a, const char *from, const char *to) {
    if (!a || !from || !to) return GRAFT_E_INVALID_ARG;
    return guarded([&]() -> graft_status {
        auto &s = a->s;
        const auto *va = graft::arc::find_version(s, from), *vb = graft::arc::find_version(s, to);
        if (!va || !vb) {
            s.log(GRAFT_LOG_ERROR, std::string("no version '") + (!va ? from : to) + "'");
            return GRAFT_E_INVALID_ARG;
        }
        std::map<std::string, const graft::arc::VEntry *> ma, mb;
        for (const auto &e : va->entries) ma[e.path] = &e;
        for (const auto &e : vb->entries) mb[e.path] = &e;
        for (const auto &[p, e] : ma)
            if (!mb.count(p)) s.log(GRAFT_LOG_INFO, "- " + p);
        for (const auto &[p, e] : mb) {
            const auto it = ma.find(p);
            if (it == ma.end()) {
                s.log(GRAFT_LOG_INFO, "+ " + p);
                continue;
            }
            const auto &x = *it->second;
            if (x.kind != e->kind) s.log(GRAFT_LOG_INFO, "T " + p);
            else if (e->kind == graft::kNodeFile && s.objs[static_cast<size_t>(x.object)].hash !=
                                                         s.objs[static_cast<size_t>(e->object)].hash)
                s.log(GRAFT_LOG_INFO, "M " + p);
            else if (e->kind == graft::kNodeSymlink && x.link != e->link) s.log(GRAFT_LOG_INFO, "M " + p);
            else if (e->kind != graft::kNodeSymlink && x.mode != e->mode) s.log(GRAFT_LOG_INFO, "m " + p);
        }
        return GRAFT_OK;
    });
}

extern "C" graft_status graft_archive_mkpatch(graft_archive *a, const char *to, const char *const *from,
                                              size_t from_count, const char *out_path,
                                              const graft_pack_options *opts) {
    if (!a || !to || !out_path || (from_count && !from)) return GRAFT_E_INVALID_ARG;
    return guarded([&]() -> graft_status {
        namespace fs = std::filesystem;
        auto &s = a->s;
        std::vector<std::string> froms;
        if (from_count) {
            for (size_t i = 0; i < from_count; ++i) froms.emplace_back(from[i]);
        } else {
            for (const auto &v : s.versions)
                if (v.label != to) froms.push_back(v.label);
        }
        const fs::path work = fs::temp_directory_path() / ("graft-mkpatch-" + graft::arc::random_suffix());
        std::error_code ec;
        if (!fs::create_directory(work, ec) || ec) return GRAFT_E_IO;
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                std::error_code e;
                fs::remove_all(p, e);
            }
        } cleanup{work};

        if (graft_status st = graft::arc::extract(s, to, work / "to"); st != GRAFT_OK) return st;
        std::vector<std::string> dirs;
        for (size_t i = 0; i < froms.size(); ++i) {
            dirs.push_back((work / ("from" + std::to_string(i))).string());
            if (graft_status st = graft::arc::extract(s, froms[i], dirs.back()); st != GRAFT_OK) return st;
        }
        std::vector<const char *> dp, lp;
        for (size_t i = 0; i < froms.size(); ++i) {
            dp.push_back(dirs[i].c_str());
            lp.push_back(froms[i].c_str());
        }
        graft_pack_options o;
        graft_pack_options_init(&o);
        if (opts) {
            if (opts->struct_size < sizeof o) return GRAFT_E_INVALID_ARG;
            o = *opts;
        }
        if (!o.log) {
            o.log = s.log.fn();
            o.log_ctx = s.log.ctx();
        }
        return graft_pack((work / "to").string().c_str(), to, dp.data(), lp.data(), dp.size(), out_path, &o);
    });
}

extern "C" graft_status graft_archive_prune(const char *path, const char *const *keep, size_t keep_count, int level,
                                            graft_log_fn log, void *log_ctx) {
    if (!path || (keep_count && !keep) || level < 1 || level > ZSTD_maxCLevel()) return GRAFT_E_INVALID_ARG;
    return guarded([&]() -> graft_status {
        namespace fs = std::filesystem;
        graft_archive *src = nullptr;
        if (graft_status st = open_impl(path, 0, log, log_ctx, &src); st != GRAFT_OK) return st;
        std::unique_ptr<graft_archive, void (*)(graft_archive *)> sg(src, graft_archive_close);
        std::set<std::string> want(keep, keep + keep_count);
        for (const auto &l : want)
            if (!graft::arc::find_version(src->s, l)) {
                src->s.log(GRAFT_LOG_ERROR, "no version '" + l + "'");
                return GRAFT_E_INVALID_ARG;
            }

        const std::string tmp = std::string(path) + ".graft-tmp";
        graft_archive *dst = nullptr;
        std::error_code ec;
        if (fs::exists(tmp, ec)) {
            src->s.log(GRAFT_LOG_ERROR, tmp + ": already exists (left over from an earlier run?)");
            return GRAFT_E_IO;
        }
        if (graft_status st = open_impl(tmp.c_str(), GRAFT_ARCHIVE_CREATE, log, log_ctx, &dst); st != GRAFT_OK)
            return st;
        std::unique_ptr<graft_archive, void (*)(graft_archive *)> dg(dst, graft_archive_close);
        struct Cleanup {
            std::string p;
            bool keep = false;
            ~Cleanup() {
                std::error_code e;
                if (!keep) fs::remove(p, e);
            }
        } cleanup{tmp};

        // Re-add each kept version, oldest first, with contents from the old archive.
        for (const auto &v : src->s.versions) {
            if (!want.count(v.label)) continue;
            std::vector<graft::arc::Item> items;
            for (const auto &e : v.entries) {
                graft::arc::Item it;
                it.entry = e;
                if (e.kind == graft::kNodeFile) {
                    const auto &o = src->s.objs[static_cast<size_t>(e.object)];
                    it.hash = o.hash;
                    it.size = o.size;
                    const uint64_t id = e.object;
                    graft::arc::State *ss = &src->s;
                    it.fetch = [ss, id](std::vector<uint8_t> &out) {
                        graft::arc::Content c;
                        if (graft_status st = graft::arc::materialize(*ss, id, c); st != GRAFT_OK) return st;
                        out = *c;
                        return GRAFT_OK;
                    };
                }
                items.push_back(std::move(it));
            }
            if (graft_status st = graft::arc::add_version(dst->s, v.label, v.time, items, level); st != GRAFT_OK)
                return st;
        }
        dg.reset();
        sg.reset();
        fs::rename(tmp, path, ec);
        if (ec) return GRAFT_E_IO;
        cleanup.keep = true;
        return GRAFT_OK;
    });
}
