// package_build.cpp: graft_pack. Compares the new tree against each old
// tree and writes a .gfp package.

#include "package_format.h"
#include "tree.h"

#include <blake3.h>
#include <zstd.h>

#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>

namespace graft::pkg {
namespace {

// Package entry kinds and tree node kinds share values.
static_assert(int{kFile} == int{kNodeFile} && int{kDir} == int{kNodeDir} && int{kSymlink} == int{kNodeSymlink});

graft_status scan(const fs::path &root, Tree &tree, const Log &log) { return scan_tree(root, tree, log); }

class HashingFile {
public:
    explicit HashingFile(std::FILE *f) : f_(f) { blake3_hasher_init(&h_); }
    graft_status write(const void *p, size_t n) {
        if (n == 0) return GRAFT_OK;
        blake3_hasher_update(&h_, p, n);
        if (std::fwrite(p, 1, n, f_) != n) return GRAFT_E_IO;
        pos_ += n;
        return GRAFT_OK;
    }
    uint64_t pos() const { return pos_; }
    void digest(uint8_t out[GRAFT_HASH_SIZE]) const { blake3_hasher_finalize(&h_, out, GRAFT_HASH_SIZE); }
    graft_sink sink() {
        return {this, [](void *ctx, const void *p, size_t n) { return static_cast<HashingFile *>(ctx)->write(p, n); }};
    }

private:
    std::FILE *f_;
    blake3_hasher h_;
    uint64_t pos_ = 0;
};

struct Builder {
    const graft_pack_options &opts;
    Log log;
    HashingFile &out;
    Manifest m;
    std::vector<Tree> olds;
    std::vector<fs::path> old_roots;
    Tree nw;
    fs::path new_root;
    std::map<Hash, uint64_t> full_blobs;  // new content -> blob

    // Index of each old tree's files by content, for rename sources.
    std::vector<std::map<Hash, std::string>> by_hash;
    std::vector<std::map<std::string, std::string>> by_name;

    graft_status write_blob(uint8_t kind, uint64_t raw_size, const std::vector<uint8_t> &bytes, uint64_t &idx) {
        Blob b{kind, out.pos(), bytes.size(), raw_size};
        if (graft_status s = out.write(bytes.data(), bytes.size()); s != GRAFT_OK) return s;
        idx = m.blobs.size();
        m.blobs.push_back(b);
        return GRAFT_OK;
    }

    graft_status full_blob(const std::string &path, const Node &n, uint64_t &idx) {
        if (auto it = full_blobs.find(n.hash); it != full_blobs.end()) {
            idx = it->second;
            return GRAFT_OK;
        }
        std::vector<uint8_t> data, comp;
        if (graft_status s = read_whole(new_root / path, data); s != GRAFT_OK) return s;
        comp.resize(ZSTD_compressBound(data.size()));
        const size_t c = ZSTD_compress(comp.data(), comp.size(), data.data(), data.size(), opts.level);
        if (ZSTD_isError(c)) return GRAFT_E_NOMEM;
        comp.resize(c);
        if (graft_status s = write_blob(kBlobZstd, data.size(), comp, idx); s != GRAFT_OK) return s;
        full_blobs.emplace(n.hash, idx);
        log(GRAFT_LOG_INFO, "full   " + path + " (" + std::to_string(c) + " bytes)");
        return GRAFT_OK;
    }

    // Delta from old version `v`'s file at `src` to the new file at `path`.
    // Returns the target's masked regions through `regions`.
    graft_status delta_blob(size_t v, const std::string &src, const std::string &path, uint64_t &idx,
                            Regions &regions) {
        std::vector<uint8_t> oldb, newb;
        if (graft_status s = read_whole(old_roots[v] / src, oldb); s != GRAFT_OK) return s;
        if (graft_status s = read_whole(new_root / path, newb); s != GRAFT_OK) return s;

        std::vector<graft_mask> masks(64);
        size_t nmasks = 0;
        if (opts.masks_for) {
            graft_status s = opts.masks_for(opts.masks_ctx, path.c_str(), oldb.data(), oldb.size(), newb.data(),
                                            newb.size(), masks.data(), masks.size(), &nmasks);
            if (s != GRAFT_OK) return s;
            if (nmasks > masks.size()) return GRAFT_E_INVALID_ARG;
        }
        masks.resize(nmasks);
        regions.clear();
        for (const auto &k : masks) regions.emplace_back(k.new_offset, k.new_length);
        std::sort(regions.begin(), regions.end());

        graft_diff_options d;
        graft_diff_options_init(&d);
        d.level = opts.level;
        d.masks = masks.data();
        d.mask_count = masks.size();
        if (opts.filter_for) d.filter = opts.filter_for(opts.filter_ctx, path.c_str());
        std::vector<uint8_t> patch;
        graft_sink sink{&patch, [](void *ctx, const void *p, size_t n) {
                            auto *b = static_cast<const uint8_t *>(p);
                            static_cast<std::vector<uint8_t> *>(ctx)->insert(
                                static_cast<std::vector<uint8_t> *>(ctx)->end(), b, b + n);
                            return GRAFT_OK;
                        }};
        if (graft_status s = graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &d, &sink);
            s != GRAFT_OK) {
            if (s == GRAFT_E_INVALID_ARG) log(GRAFT_LOG_ERROR, path + ": masks rejected (overlapping or out of range)");
            return s;
        }
        if (graft_status s = write_blob(kBlobGft, 0, patch, idx); s != GRAFT_OK) return s;
        log(GRAFT_LOG_INFO, "delta  " + path + (src == path ? "" : " (from " + src + ")") + " for " + m.labels[v] +
                                " (" + std::to_string(patch.size()) + " bytes)");
        return GRAFT_OK;
    }

    bool owned(const std::string &path) const {
        return opts.user_owned && opts.user_owned(opts.user_ctx, path.c_str()) != 0;
    }

    // A user-owned file: one full copy, installed only where it's missing.
    graft_status seed_entry(const std::string &path, const Node &n) {
        Entry e;
        e.path = path;
        e.kind = kSeed;
        e.mode = n.mode;
        e.size = n.size;
        e.current = n.hash;
        Recipe r;
        r.kind = kFull;
        if (graft_status s = full_blob(path, n, r.blob); s != GRAFT_OK) return s;
        for (size_t v = 0; v < olds.size(); ++v)
            if (!olds[v].count(path)) r.labels.push_back(v);
        e.recipes.push_back(std::move(r));
        log(GRAFT_LOG_INFO, "seed   " + path + " (user-owned: installed only where missing)");
        m.entries.push_back(std::move(e));
        return GRAFT_OK;
    }

    graft_status file_entry(const std::string &path, const Node &n) {
        Entry e;
        e.path = path;
        e.kind = kFile;
        e.mode = n.mode;
        e.size = n.size;
        bool needed = false, have_regions = false;
        // Recipes keyed by (source path, source content): versions sharing both share a blob.
        std::map<std::pair<std::string, Hash>, size_t> recipe_for;
        for (size_t v = 0; v < olds.size(); ++v) {
            const auto it = olds[v].find(path);
            const bool same_path_file = it != olds[v].end() && it->second.kind == kFile;
            if (same_path_file && it->second.hash == n.hash) {
                if (it->second.mode != n.mode) needed = true;  // a chmod; no recipe
                continue;
            }
            needed = true;
            // Pick a source: the same path, else identical content elsewhere, else the same name elsewhere.
            std::string src;
            Hash src_hash{};
            if (same_path_file) {
                src = path;
                src_hash = it->second.hash;
            } else if (auto h = by_hash[v].find(n.hash); h != by_hash[v].end()) {
                src = h->second;
                src_hash = n.hash;
            } else if (auto nm = by_name[v].find(fs::path(path).filename().string()); nm != by_name[v].end()) {
                src = nm->second;
                src_hash = olds[v].at(src).hash;
            }
            if (src.empty()) {
                Recipe r;
                r.kind = kFull;
                if (graft_status s = full_blob(path, n, r.blob); s != GRAFT_OK) return s;
                auto key = std::make_pair(std::string(), n.hash);
                if (auto rf = recipe_for.find(key); rf != recipe_for.end()) {
                    e.recipes[rf->second].labels.push_back(v);
                    continue;
                }
                r.labels.push_back(v);
                recipe_for.emplace(key, e.recipes.size());
                e.recipes.push_back(std::move(r));
                continue;
            }
            auto key = std::make_pair(src, src_hash);
            if (auto rf = recipe_for.find(key); rf != recipe_for.end()) {
                e.recipes[rf->second].labels.push_back(v);
                continue;
            }
            Recipe r;
            r.kind = kDelta;
            r.source_path = src;
            Regions regions;
            if (graft_status s = delta_blob(v, src, path, r.blob, regions); s != GRAFT_OK) return s;
            if (have_regions && regions != e.regions) {
                log(GRAFT_LOG_ERROR, path + ": masked regions in the new file differ between old versions");
                return GRAFT_E_INVALID_ARG;
            }
            e.regions = regions;
            have_regions = true;
            r.labels.push_back(v);
            recipe_for.emplace(key, e.recipes.size());
            e.recipes.push_back(std::move(r));
        }
        if (!needed) return GRAFT_OK;
        std::vector<uint8_t> data;
        if (graft_status s = read_whole(new_root / path, data); s != GRAFT_OK) return s;
        hash_bytes(data.data(), data.size(), e.regions, e.current);
        m.entries.push_back(std::move(e));
        return GRAFT_OK;
    }

    graft_status build() {
        for (size_t v = 0; v < olds.size(); ++v) {
            by_hash.emplace_back();
            by_name.emplace_back();
            for (const auto &[p, n] : olds[v]) {
                if (n.kind != kFile || owned(p)) continue;  // never patch from a user's file
                by_hash[v].emplace(n.hash, p);  // first path wins (sorted)
                by_name[v].emplace(fs::path(p).filename().string(), p);
            }
        }

        // Paths that differ only by case can't coexist on every filesystem.
        std::map<std::string, std::string> folded;
        for (const auto &[p, n] : nw) {
            auto [it, fresh] = folded.emplace(fold_case(p), p);
            if (!fresh) {
                log(GRAFT_LOG_ERROR, p + " and " + it->second + " differ only by case");
                return GRAFT_E_INVALID_ARG;
            }
        }

        // Removals: anything in an old tree that's gone (or changed kind) in the new one.
        std::map<std::string, Entry> removals;
        for (const auto &old : olds) {
            for (const auto &[p, n] : old) {
                if (owned(p)) continue;  // the user's to keep
                const auto it = nw.find(p);
                if (it != nw.end() && it->second.kind == n.kind) continue;
                Entry &e = removals[p];
                e.path = p;
                e.kind = kRemove;
                e.what = n.kind;  // if versions disagree on kind, the last wins; plan rechecks
                if (n.kind == kFile &&
                    std::find(e.known_hashes.begin(), e.known_hashes.end(), n.hash) == e.known_hashes.end())
                    e.known_hashes.push_back(n.hash);
                if (n.kind == kSymlink &&
                    std::find(e.known_links.begin(), e.known_links.end(), n.link) == e.known_links.end())
                    e.known_links.push_back(n.link);
            }
        }
        for (auto &[p, e] : removals) {
            log(GRAFT_LOG_INFO, "remove " + p);
            m.entries.push_back(std::move(e));
        }

        for (const auto &[p, n] : nw) {
            if (n.kind == kFile && owned(p)) {
                if (graft_status s = seed_entry(p, n); s != GRAFT_OK) return s;
            } else if (n.kind == kFile) {
                if (graft_status s = file_entry(p, n); s != GRAFT_OK) return s;
            } else if (n.kind == kDir) {
                bool needed = false;
                for (const auto &old : olds) {
                    const auto it = old.find(p);
                    needed |= it == old.end() || it->second.kind != kDir || it->second.mode != n.mode;
                }
                if (needed) {
                    Entry e;
                    e.path = p;
                    e.kind = kDir;
                    e.mode = n.mode;
                    m.entries.push_back(std::move(e));
                }
            } else {
                Entry e;
                e.path = p;
                e.kind = kSymlink;
                e.link = n.link;
                bool needed = false;
                for (const auto &old : olds) {
                    const auto it = old.find(p);
                    if (it != old.end() && it->second.kind == kSymlink && it->second.link == n.link) continue;
                    needed = true;
                    if (it != old.end() && it->second.kind == kSymlink &&
                        std::find(e.known_links.begin(), e.known_links.end(), it->second.link) == e.known_links.end())
                        e.known_links.push_back(it->second.link);
                }
                if (needed) m.entries.push_back(std::move(e));
            }
        }
        std::stable_sort(m.entries.begin(), m.entries.end(), [](const Entry &a, const Entry &b) {
            if (a.path != b.path) return a.path < b.path;
            return kind_order(a.kind) < kind_order(b.kind);
        });
        return GRAFT_OK;
    }
};

graft_status pack_impl(const char *new_dir, const char *new_label, const char *const *old_dirs,
                       const char *const *old_labels, size_t old_count, const char *out_path,
                       const graft_pack_options &opts) {
    Log log(opts.log, opts.log_ctx);
    const fs::path out_final(out_path);
    const fs::path out_tmp = out_final.string() + ".graft-tmp";
    File f;
    if (!f.open(out_tmp, "wbx")) {
        log(GRAFT_LOG_ERROR, out_tmp.string() + ": can't create (left over from an earlier run?)");
        return GRAFT_E_IO;
    }
    struct Cleanup {
        const fs::path &p;
        bool keep = false;
        ~Cleanup() {
            std::error_code ec;
            if (!keep) fs::remove(p, ec);
        }
    } cleanup{out_tmp};

    HashingFile out(f.get());
    Builder b{opts, log, out, {}, {}, {}, {}, new_dir, {}, {}, {}};
    b.m.target_label = new_label ? new_label : "";
    if (graft_status s = scan(new_dir, b.nw, log); s != GRAFT_OK) return s;
    for (size_t i = 0; i < old_count; ++i) {
        b.old_roots.emplace_back(old_dirs[i]);
        b.m.labels.emplace_back(old_labels && old_labels[i] ? old_labels[i] : old_dirs[i]);
        b.olds.emplace_back();
        if (graft_status s = scan(old_dirs[i], b.olds.back(), log); s != GRAFT_OK) return s;
    }

    uint8_t hdr[kHeaderSize];
    std::memcpy(hdr, kMagic, 8);
    put_le<uint16_t>(hdr + 8, kVersion);
    put_le<uint16_t>(hdr + 10, 0);
    if (graft_status s = out.write(hdr, sizeof hdr); s != GRAFT_OK) return s;

    if (graft_status s = b.build(); s != GRAFT_OK) return s;

    std::vector<uint8_t> raw, comp;
    encode_manifest(b.m, raw);
    if (raw.size() > kMaxManifest) return GRAFT_E_LIMIT;
    comp.resize(ZSTD_compressBound(raw.size()));
    const size_t c = ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), opts.level);
    if (ZSTD_isError(c)) return GRAFT_E_NOMEM;
    comp.resize(c);
    const uint64_t moff = out.pos();
    if (graft_status s = out.write(comp.data(), comp.size()); s != GRAFT_OK) return s;
    uint8_t foot[kFooterSize];
    put_le<uint64_t>(foot, moff);
    put_le<uint64_t>(foot + 8, comp.size());
    put_le<uint64_t>(foot + 16, raw.size());
    if (graft_status s = out.write(foot, sizeof foot); s != GRAFT_OK) return s;
    uint8_t h[GRAFT_HASH_SIZE];
    out.digest(h);
    if (std::fwrite(h, 1, sizeof h, f.get()) != sizeof h) return GRAFT_E_IO;
    if (sync_file(f.get()) != GRAFT_OK || !f.close()) return GRAFT_E_IO;

    std::error_code ec;
    fs::rename(out_tmp, out_final, ec);
    if (ec) return GRAFT_E_IO;
    cleanup.keep = true;
    log(GRAFT_LOG_INFO, std::to_string(b.m.entries.size()) + " entries, " + std::to_string(b.m.blobs.size()) +
                            " blobs, " + std::to_string(out.pos() + sizeof h) + " bytes");
    return GRAFT_OK;
}

}  // namespace
}  // namespace graft::pkg

extern "C" void graft_pack_options_init(graft_pack_options *o) {
    if (!o) return;
    o->struct_size = sizeof *o;
    o->level = GRAFT_DEFAULT_LEVEL;
    o->masks_for = nullptr;
    o->masks_ctx = nullptr;
    o->log = nullptr;
    o->log_ctx = nullptr;
    o->filter_for = nullptr;
    o->filter_ctx = nullptr;
    o->user_owned = nullptr;
    o->user_ctx = nullptr;
}

extern "C" graft_status graft_pack(const char *new_dir, const char *new_label, const char *const *old_dirs,
                                   const char *const *old_labels, size_t old_count, const char *out_path,
                                   const graft_pack_options *opts) {
    if (!new_dir || !out_path || (old_count && !old_dirs)) return GRAFT_E_INVALID_ARG;
    for (size_t i = 0; i < old_count; ++i)
        if (!old_dirs[i]) return GRAFT_E_INVALID_ARG;
    graft_pack_options o;
    graft_pack_options_init(&o);
    if (opts) {
        if (opts->struct_size < sizeof o) return GRAFT_E_INVALID_ARG;
        o = *opts;
    }
    if (o.level < 1 || o.level > ZSTD_maxCLevel()) return GRAFT_E_INVALID_ARG;
    try {
        return graft::pkg::pack_impl(new_dir, new_label, old_dirs, old_labels, old_count, out_path, o);
    } catch (const std::bad_alloc &) {
        return GRAFT_E_NOMEM;
    } catch (const std::length_error &) {
        return GRAFT_E_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return GRAFT_E_IO;
    }
}
