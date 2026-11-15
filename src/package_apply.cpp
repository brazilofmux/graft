// package_apply.cpp: graft_update and graft_package_describe.
//
// Update = open (verify the whole package) -> plan (inspect every file the
// package touches; any mismatch is a conflict, and nothing has changed yet)
// -> stage (build every new file under .graft-txn/stage and verify it) ->
// commit (journaled renames; failure rolls back) -> clean up.

#include "package_internal.h"
#include "tree.h"

#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>

namespace graft::pkg {

FaultHook g_fault_hook = nullptr;

namespace {

std::string join_labels(const Manifest &m, const std::vector<uint64_t> &idx) {
    std::string s;
    for (uint64_t i : idx) {
        if (!s.empty()) s += ", ";
        s += m.labels[static_cast<size_t>(i)];
    }
    return s;
}

// --- opening -----------------------------------------------------------------------

struct Package {
    File f;
    uint64_t size = 0;
    Manifest m;
};

graft_status open_package(const fs::path &path, Package &pk, const Log &log) {
    std::error_code ec;
    pk.size = fs::file_size(path, ec);
    if (ec || !pk.f.open(path, "rb")) {
        log(GRAFT_LOG_ERROR, u8str(path) + ": can't open");
        return GRAFT_E_IO;
    }
    if (pk.size < kHeaderSize + kFooterSize + GRAFT_HASH_SIZE) return GRAFT_E_CORRUPT;
    const uint64_t body = pk.size - GRAFT_HASH_SIZE;

    // Whole-file hash first: nothing is parsed from a damaged package.
    {
        graft_hasher *h = nullptr;
        if (graft_hasher_new(&h) != GRAFT_OK) return GRAFT_E_NOMEM;
        std::unique_ptr<graft_hasher, void (*)(graft_hasher *)> guard(h, graft_hasher_free);
        std::vector<uint8_t> buf(1 << 16);
        for (uint64_t off = 0; off < body;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), body - off));
            if (std::fread(buf.data(), 1, n, pk.f.get()) != n) return GRAFT_E_IO;
            graft_hasher_update(h, buf.data(), n);
            off += n;
        }
        uint8_t want[GRAFT_HASH_SIZE], got[GRAFT_HASH_SIZE];
        if (std::fread(want, 1, sizeof want, pk.f.get()) != sizeof want) return GRAFT_E_IO;
        graft_hasher_final(h, got);
        if (kCheckHashes && std::memcmp(want, got, sizeof want) != 0) return GRAFT_E_CORRUPT;
    }

    uint8_t hdr[kHeaderSize], foot[kFooterSize];
    if (!file_seek(pk.f.get(), 0) || std::fread(hdr, 1, sizeof hdr, pk.f.get()) != sizeof hdr) return GRAFT_E_IO;
    if (std::memcmp(hdr, kMagic, 8) != 0) return GRAFT_E_CORRUPT;
    if (get_le<uint16_t>(hdr + 8) != kVersion || get_le<uint16_t>(hdr + 10) != 0) return GRAFT_E_UNSUPPORTED;
    if (!file_seek(pk.f.get(), body - kFooterSize) || std::fread(foot, 1, sizeof foot, pk.f.get()) != sizeof foot)
        return GRAFT_E_IO;
    const uint64_t moff = get_le<uint64_t>(foot), mcomp = get_le<uint64_t>(foot + 8), mraw = get_le<uint64_t>(foot + 16);
    if (moff < kHeaderSize || moff > body - kFooterSize || mcomp != body - kFooterSize - moff) return GRAFT_E_CORRUPT;
    if (mraw > kMaxManifest || mcomp > ZSTD_compressBound(static_cast<size_t>(mraw)) || mcomp == 0)
        return GRAFT_E_CORRUPT;

    std::vector<uint8_t> comp(static_cast<size_t>(mcomp)), raw(static_cast<size_t>(mraw));
    if (!file_seek(pk.f.get(), moff) || std::fread(comp.data(), 1, comp.size(), pk.f.get()) != comp.size())
        return GRAFT_E_IO;
    const size_t n = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    if (ZSTD_isError(n) || n != raw.size()) return GRAFT_E_CORRUPT;
    if (!decode_manifest(raw.data(), raw.size(), pk.m) || !validate_blobs(pk.m, moff)) return GRAFT_E_CORRUPT;
    return GRAFT_OK;
}

// --- inspecting the installed tree ---------------------------------------------------

enum class Kind { Missing, File, Dir, Symlink, Other, Unsafe };

struct Stat {
    Kind kind = Kind::Missing;
    std::string link;
};

std::vector<std::string> split(const std::string &p) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string::npos) slash = p.size();
        parts.push_back(p.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

std::string parent_of(const std::string &p) {
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

// lstat of root/rel that refuses to pass through a symlink or non-directory.
Stat lstat_rel(const fs::path &root, const std::string &rel) {
    const auto parts = split(rel);
    fs::path cur = root;
    std::error_code ec;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        cur /= u8path(parts[i]);
        const auto st = fs::symlink_status(cur, ec);
        if (st.type() == fs::file_type::not_found) return {Kind::Missing, {}};
        if (ec || fs::is_symlink(st) || !fs::is_directory(st)) return {Kind::Unsafe, {}};
    }
    cur /= u8path(parts.back());
    const auto st = fs::symlink_status(cur, ec);
    if (st.type() == fs::file_type::not_found) return {Kind::Missing, {}};
    if (ec) return {Kind::Unsafe, {}};
    if (fs::is_symlink(st)) return {Kind::Symlink, u8str(fs::read_symlink(cur, ec))};
    if (fs::is_directory(st)) return {Kind::Dir, {}};
    if (fs::is_regular_file(st)) return {Kind::File, {}};
    return {Kind::Other, {}};
}

uint32_t mode_of(const fs::path &p) {
#if defined(_WIN32)
    (void)p;
    return 0;
#else
    std::error_code ec;
    return static_cast<uint32_t>(fs::symlink_status(p, ec).permissions() & fs::perms::mask) & 07777;
#endif
}

void set_mode(const fs::path &p, uint32_t mode, std::error_code &ec) {
#if defined(_WIN32)
    (void)p;
    (void)mode;
    ec.clear();
#else
    fs::permissions(p, static_cast<fs::perms>(mode), ec);
#endif
}

// --- planning --------------------------------------------------------------------------

struct Action {
    enum Type { Remove, RemoveDir, MakeDir, Install, Chmod } type;
    std::string path;
    uint32_t mode = 0;
    const Entry *entry = nullptr;
    int recipe = -1;       // Install of a file: which recipe
    bool replace = false;  // Install over an existing file/link (back it up first)
};

struct Planner {
    const fs::path &root;
    Package &pk;
    const graft_apply_limits &limits;
    const Log &log;
    std::vector<Action> actions;
    std::vector<std::string> conflicts;
    std::vector<std::string> notes;  // informational: user-owned files left alone
    std::map<std::string, Kind> future;  // state after planned changes, where it differs
    std::set<std::string> creates;       // paths with a create entry

    // The path's state once the planned changes are made. A planned change to
    // an ancestor decides it: below a removed path or a directory the plan
    // creates, nothing exists yet; below a planned file or link is unsafe.
    Stat state(const std::string &p) {
        if (auto it = future.find(p); it != future.end()) return {it->second, {}};
        for (std::string d = parent_of(p); !d.empty(); d = parent_of(d)) {
            if (auto it = future.find(d); it != future.end()) {
                if (it->second == Kind::Missing || it->second == Kind::Dir) return {Kind::Missing, {}};
                return {Kind::Unsafe, {}};
            }
        }
        return lstat_rel(root, p);
    }

    void conflict(const std::string &p, const std::string &why) { conflicts.push_back(p + ": " + why); }

    // A file the plan couldn't read: say why, when the reason is another program.
    void unreadable(const std::string &p) {
        conflict(p, file_in_use(join(root, p)) ? std::string(kInUse) : std::string("can't read"));
    }
    static constexpr const char *kInUse = "in use by another program; stop it and try again";

    bool ensure_parents(const std::string &p) {
        std::vector<std::string> missing;
        for (std::string d = parent_of(p); !d.empty(); d = parent_of(d)) {
            const Stat st = state(d);
            if (st.kind == Kind::Dir) break;
            if (st.kind != Kind::Missing) {
                conflict(p, "parent " + d + " isn't a directory");
                return false;
            }
            missing.push_back(d);
        }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            actions.push_back({Action::MakeDir, *it, 0755});
            future[*it] = Kind::Dir;
        }
        return true;
    }

    bool source_matches(const Recipe &r) {
        const Stat st = lstat_rel(root, r.source_path);
        if (st.kind != Kind::File) return false;
        File src;
        std::error_code ec;
        const fs::path sp = join(root, r.source_path);
        const uint64_t size = fs::file_size(sp, ec);
        if (ec || !src.open(sp, "rb")) return false;
        FileSource fsrc{src.get(), size};
        graft_source gs = fsrc.source();
        const Blob &b = pk.m.blobs[static_cast<size_t>(r.blob)];
        RangeReader rr{pk.f.get(), b.offset, b.offset + b.length};
        graft_reader gr = rr.reader();
        return graft_patch_check_source(&gr, &gs) == GRAFT_OK;
    }

    void plan_remove(const Entry &e) {
        const Stat st = lstat_rel(root, e.path);
        const bool recreated = creates.count(e.path) != 0;
        if (st.kind == Kind::Missing) return;
        if (st.kind == Kind::Unsafe) return conflict(e.path, "path goes through a symlink or non-directory");
        const Kind want = e.what == kFile ? Kind::File : e.what == kDir ? Kind::Dir : Kind::Symlink;
        if (st.kind != want) {
            if (!recreated) conflict(e.path, "expected to remove a different kind of entry");
            return;  // a create entry for this path will judge what's there
        }
        if (e.what == kFile) {
            Hash h;
            if (hash_file(join(root, e.path), {}, h) != GRAFT_OK) return unreadable(e.path);
            if (std::find(e.known_hashes.begin(), e.known_hashes.end(), h) == e.known_hashes.end()) {
                if (!recreated) conflict(e.path, "modified locally; not removing it");
                return;
            }
        } else if (e.what == kSymlink) {
            if (std::find(e.known_links.begin(), e.known_links.end(), st.link) == e.known_links.end()) {
                if (!recreated) conflict(e.path, "symlink points somewhere unexpected; not removing it");
                return;
            }
        }
        actions.push_back({e.what == kDir ? Action::RemoveDir : Action::Remove, e.path});
        future[e.path] = Kind::Missing;
    }

    void plan_dir(const Entry &e) {
        const Stat st = state(e.path);
        if (st.kind == Kind::Dir) {
            if (kModesSupported && mode_of(join(root, e.path)) != e.mode && future.count(e.path) == 0)
                actions.push_back({Action::Chmod, e.path, e.mode});
            return;
        }
        if (st.kind != Kind::Missing) return conflict(e.path, "exists and isn't a directory");
        if (!ensure_parents(e.path)) return;
        actions.push_back({Action::MakeDir, e.path, e.mode});
        future[e.path] = Kind::Dir;
    }

    void plan_symlink(const Entry &e) {
        const Stat st = state(e.path);
        if (st.kind == Kind::Symlink && st.link == e.link) return;
        bool replace = false;
        if (st.kind == Kind::Symlink) {
            if (std::find(e.known_links.begin(), e.known_links.end(), st.link) == e.known_links.end())
                return conflict(e.path, "symlink points somewhere unexpected");
            replace = true;
        } else if (st.kind != Kind::Missing) {
            return conflict(e.path, "exists and isn't a symlink");
        }
        if (!ensure_parents(e.path)) return;
        Action a{Action::Install, e.path};
        a.entry = &e;
        a.replace = replace;
        actions.push_back(a);
        future[e.path] = Kind::Symlink;
    }

    // User-owned: install only where nothing exists; never look further.
    void plan_seed(const Entry &e) {
        const Stat st = state(e.path);
        if (st.kind == Kind::Unsafe) {
            notes.push_back("not installing " + e.path + ": its path goes through a symlink or non-directory");
            return;
        }
        if (st.kind != Kind::Missing) {
            notes.push_back("keep   " + e.path + " (user-owned)");
            return;
        }
        if (!ensure_parents(e.path)) return;
        Action a{Action::Install, e.path, e.mode};
        a.entry = &e;
        a.recipe = 0;
        actions.push_back(a);
        future[e.path] = Kind::File;
    }

    void plan_file(const Entry &e) {
        const Stat st = state(e.path);
        if (st.kind == Kind::Unsafe) return conflict(e.path, "path goes through a symlink or non-directory");
        if (st.kind != Kind::File && st.kind != Kind::Missing) return conflict(e.path, "exists and isn't a file");
        if (st.kind == Kind::File) {
            Hash h;
            uint64_t size = 0;
            if (hash_file(join(root, e.path), e.regions, h, &size) != GRAFT_OK) return unreadable(e.path);
            if (size == e.size && h == e.current) {
                if (kModesSupported && mode_of(join(root, e.path)) != e.mode)
                    actions.push_back({Action::Chmod, e.path, e.mode});
                return;  // already the new version
            }
        }
        for (size_t i = 0; i < e.recipes.size(); ++i) {
            const Recipe &r = e.recipes[i];
            if (r.kind == kDelta ? source_matches(r) : st.kind == Kind::Missing) {
                if (!ensure_parents(e.path)) return;
                Action a{Action::Install, e.path, e.mode};
                a.entry = &e;
                a.recipe = static_cast<int>(i);
                a.replace = st.kind == Kind::File;
                actions.push_back(a);
                future[e.path] = Kind::File;
                return;
            }
        }
        conflict(e.path, st.kind == Kind::File ? "doesn't match any version this package updates"
                                               : "missing, and the package has no full copy of it");
    }

    void plan() {
        for (const auto &e : pk.m.entries) {
            if (e.kind == kRemove) continue;
            creates.insert(e.path);
            if (!platform_safe_path(e.path)) conflict(e.path, "this name can't be created on this platform");
        }
        for (const auto &e : pk.m.entries)
            if (e.kind == kRemove) plan_remove(e);
        for (const auto &e : pk.m.entries) {
            if (e.kind == kDir) plan_dir(e);
            else if (e.kind == kSymlink) plan_symlink(e);
            else if (e.kind == kFile) plan_file(e);
            else if (e.kind == kSeed) plan_seed(e);
        }
        // A file another program holds open can't be replaced or removed
        // (Windows): stop now, naming it, rather than failing mid-commit.
        for (const auto &a : actions)
            if ((a.type == Action::Remove || (a.type == Action::Install && a.replace)) &&
                file_in_use(join(root, a.path)))
                conflict(a.path, kInUse);
    }

    std::string describe(const Action &a) const {
        switch (a.type) {
        case Action::Remove: return "remove " + a.path;
        case Action::RemoveDir: return "rmdir  " + a.path;
        case Action::MakeDir: return "mkdir  " + a.path;
        case Action::Chmod: return "chmod  " + a.path;
        case Action::Install:
            if (a.entry->kind == kSymlink) return "link   " + a.path + " -> " + a.entry->link;
            if (a.entry->recipes[static_cast<size_t>(a.recipe)].kind == kFull) return "add    " + a.path;
            {
                const Recipe &r = a.entry->recipes[static_cast<size_t>(a.recipe)];
                return "update " + a.path + (r.source_path == a.path ? "" : " (from " + r.source_path + ")") +
                       " [" + join_labels(pk.m, r.labels) + "]";
            }
        }
        return {};
    }
};

// --- staging -----------------------------------------------------------------------------

graft_status stage_full(Package &pk, const Blob &b, std::FILE *out, uint64_t expect) {
    if (b.raw_size != expect) return GRAFT_E_CORRUPT;
    std::unique_ptr<ZSTD_DStream, size_t (*)(ZSTD_DStream *)> ds(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!ds) return GRAFT_E_NOMEM;
    ZSTD_DCtx_setParameter(ds.get(), ZSTD_d_windowLogMax, 27);  // bound decoder memory (128 MiB window)
    if (!file_seek(pk.f.get(), b.offset)) return GRAFT_E_IO;
    std::vector<uint8_t> in(ZSTD_DStreamInSize()), outbuf(ZSTD_DStreamOutSize());
    uint64_t left = b.length, produced = 0;
    size_t last = 1;
    while (left) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(in.size(), left));
        if (std::fread(in.data(), 1, n, pk.f.get()) != n) return GRAFT_E_IO;
        left -= n;
        ZSTD_inBuffer ib{in.data(), n, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob{outbuf.data(), outbuf.size(), 0};
            last = ZSTD_decompressStream(ds.get(), &ob, &ib);
            if (ZSTD_isError(last)) return GRAFT_E_CORRUPT;
            produced += ob.pos;
            if (produced > expect) return GRAFT_E_CORRUPT;
            if (std::fwrite(outbuf.data(), 1, ob.pos, out) != ob.pos) return GRAFT_E_IO;
        }
    }
    // Drain anything still buffered in the decoder. A return of 0 means the
    // frame is complete and flushed; calling again would ask for a new frame.
    while (last != 0) {
        ZSTD_inBuffer ib{nullptr, 0, 0};
        ZSTD_outBuffer ob{outbuf.data(), outbuf.size(), 0};
        last = ZSTD_decompressStream(ds.get(), &ob, &ib);
        if (ZSTD_isError(last)) return GRAFT_E_CORRUPT;
        produced += ob.pos;
        if (produced > expect) return GRAFT_E_CORRUPT;
        if (std::fwrite(outbuf.data(), 1, ob.pos, out) != ob.pos) return GRAFT_E_IO;
        if (ob.pos == 0) break;
    }
    return last == 0 && produced == expect ? GRAFT_OK : GRAFT_E_CORRUPT;
}

graft_status stage_one(const fs::path &root, Package &pk, const Action &a, const fs::path &dst,
                       const graft_apply_limits &limits) {
    const Entry &e = *a.entry;
    std::error_code ec;
    if (e.kind == kSymlink) {
        fs::create_symlink(u8path(e.link), dst, ec);
        return ec ? GRAFT_E_IO : GRAFT_OK;
    }
    const Recipe &r = e.recipes[static_cast<size_t>(a.recipe)];
    const Blob &b = pk.m.blobs[static_cast<size_t>(r.blob)];
    File out;
    if (!out.create(dst)) return GRAFT_E_IO;
    graft_status st;
    if (r.kind == kFull) {
        st = stage_full(pk, b, out.get(), e.size);
    } else {
        File src;
        const fs::path sp = join(root, r.source_path);
        const uint64_t size = fs::file_size(sp, ec);
        if (ec || !src.open(sp, "rb")) return GRAFT_E_IO;
        FileSource fsrc{src.get(), size};
        graft_source gs = fsrc.source();
        RangeReader rr{pk.f.get(), b.offset, b.offset + b.length};
        graft_reader gr = rr.reader();
        FileSink fsink{out.get()};
        graft_sink gk = fsink.sink();
        st = graft_apply(&gs, &gr, &gk, &limits);
    }
    if (st != GRAFT_OK) return st;
    if (sync_file(out.get()) != GRAFT_OK || !out.close()) return GRAFT_E_IO;
    // Check the staged file against the manifest too, independently of the blob.
    Hash h;
    uint64_t size = 0;
    if (graft_status s = hash_file(dst, e.regions, h, &size); s != GRAFT_OK) return s;
    if (size != e.size || (kCheckHashes && h != e.current)) return GRAFT_E_CORRUPT;
    set_mode(dst, e.mode, ec);
    return ec ? GRAFT_E_IO : GRAFT_OK;
}

// --- the journal ---------------------------------------------------------------------------
//
// One line per step, written and synced *before* the step:
//   backup P | install P N | mkdir P | rmdir P MODE | chmod P OLDMODE | commit
// Paths contain no control characters (valid_rel_path), so tabs separate fields.

struct Txn {
    fs::path root, dir, stage, backup;
    File journal;
    std::vector<std::vector<std::string>> done;  // steps logged, for rollback

    explicit Txn(const fs::path &r)
        : root(r), dir(r / kTxnDir), stage(dir / "stage"), backup(dir / "backup") {}

    graft_status log_step(std::vector<std::string> fields) {
        std::string line;
        for (size_t i = 0; i < fields.size(); ++i) line += (i ? "\t" : "") + fields[i];
        line += "\n";
        if (std::fwrite(line.data(), 1, line.size(), journal.get()) != line.size()) return GRAFT_E_IO;
        if (sync_file(journal.get()) != GRAFT_OK) return GRAFT_E_IO;
        done.push_back(std::move(fields));
        return GRAFT_OK;
    }
};

void undo_step(const fs::path &root, const fs::path &backup, const std::vector<std::string> &f, const Log &log) {
    if (f.size() < 2 || !valid_rel_path(f[1])) return;
    std::error_code ec;
    const fs::path p = join(root, f[1]);
    const std::string &op = f[0];
    if (op == "install") {
        if (fs::symlink_status(p, ec).type() != fs::file_type::not_found) fs::remove(p, ec);
    } else if (op == "backup") {
        const fs::path b = join(backup, f[1]);
        if (fs::symlink_status(b, ec).type() != fs::file_type::not_found) fs::rename(b, p, ec);
    } else if (op == "mkdir") {
        fs::remove(p, ec);  // only succeeds if empty, which it is after undoing its contents
    } else if (op == "rmdir" && f.size() >= 3) {
        fs::create_directory(p, ec);
        set_mode(p, static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 8)), ec);
    } else if (op == "chmod" && f.size() >= 3) {
        set_mode(p, static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 8)), ec);
    }
    if (ec) log(GRAFT_LOG_ERROR, "rollback: " + op + " " + f[1] + ": " + ec.message());
}

void rollback(Txn &t, const Log &log) {
    for (auto it = t.done.rbegin(); it != t.done.rend(); ++it) undo_step(t.root, t.backup, *it, log);
}

// An earlier update was interrupted: finish its cleanup if it committed,
// otherwise undo what it did.
graft_status recover(const fs::path &root, const Log &log) {
    const fs::path dir = root / kTxnDir;
    std::error_code ec;
    if (fs::symlink_status(dir, ec).type() == fs::file_type::not_found) return GRAFT_OK;
    if (fs::is_symlink(fs::symlink_status(dir, ec)) || !fs::is_directory(dir, ec)) {
        log(GRAFT_LOG_ERROR, u8str(root / kTxnDir) + " exists and isn't a directory");
        return GRAFT_E_CONFLICT;
    }
    std::vector<std::vector<std::string>> steps;
    {
        File j;
        if (j.open(dir / "journal", "rb")) {
            std::string all;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, j.get())) > 0) all.append(buf, n);
            std::istringstream in(all);
            std::string line;
            while (std::getline(in, line)) {
                if (in.eof() && !all.empty() && all.back() != '\n') break;  // torn last line: never acted on
                std::vector<std::string> f;
                size_t start = 0;
                while (start <= line.size()) {
                    size_t tab = line.find('\t', start);
                    if (tab == std::string::npos) tab = line.size();
                    f.push_back(line.substr(start, tab - start));
                    start = tab + 1;
                }
                steps.push_back(std::move(f));
            }
        }
    }
    if (!steps.empty() && steps.back().size() == 1 && steps.back()[0] == "commit") {
        log(GRAFT_LOG_WARN, "an earlier update finished but didn't clean up; cleaning up");
    } else if (!steps.empty()) {
        log(GRAFT_LOG_WARN, "an earlier update was interrupted; rolling it back");
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) undo_step(root, dir / "backup", *it, log);
    }
    make_tree_writable(dir);
    fs::remove_all(dir, ec);
    return ec ? GRAFT_E_IO : GRAFT_OK;
}

int fault(const char *phase, size_t step) { return g_fault_hook ? g_fault_hook(phase, step) : 0; }

graft_status commit(Txn &t, std::vector<Action> &actions, const std::vector<size_t> &stage_ids, const Log &log) {
    std::error_code ec;
    size_t step = 0;
    auto check_fault = [&]() -> graft_status {
        switch (fault("commit", step++)) {
        case 1: return GRAFT_E_IO;
        case 2: throw FaultCrash{};
        default: return GRAFT_OK;
        }
    };
    auto do_backup = [&](const std::string &p) -> graft_status {
        if (graft_status s = check_fault(); s != GRAFT_OK) return s;
        if (graft_status s = t.log_step({"backup", p}); s != GRAFT_OK) return s;
        const fs::path dst = join(t.backup, p);
        fs::create_directories(dst.parent_path(), ec);
        if (!ec) fs::rename(join(t.root, p), dst, ec);
        return ec ? GRAFT_E_IO : GRAFT_OK;
    };

    // 1. removals; 2. directory removals, deepest first; 3. new directories,
    // shallowest first; 4. installs; 5. mode changes.
    for (const auto &a : actions)
        if (a.type == Action::Remove)
            if (graft_status s = do_backup(a.path); s != GRAFT_OK) return s;

    std::vector<const Action *> rmdirs, mkdirs;
    for (const auto &a : actions) {
        if (a.type == Action::RemoveDir) rmdirs.push_back(&a);
        if (a.type == Action::MakeDir) mkdirs.push_back(&a);
    }
    auto depth = [](const std::string &p) { return std::count(p.begin(), p.end(), '/'); };
    std::stable_sort(rmdirs.begin(), rmdirs.end(),
                     [&](const Action *a, const Action *b) { return depth(a->path) > depth(b->path); });
    for (const Action *a : rmdirs) {
        const fs::path p = join(t.root, a->path);
        if (!fs::is_empty(p, ec) || ec) {
            log(GRAFT_LOG_WARN, a->path + ": directory not empty; leaving it");
            continue;
        }
        if (graft_status s = check_fault(); s != GRAFT_OK) return s;
        char mode[16];
        std::snprintf(mode, sizeof mode, "%o", mode_of(p));
        if (graft_status s = t.log_step({"rmdir", a->path, mode}); s != GRAFT_OK) return s;
        fs::remove(p, ec);
        if (ec) return GRAFT_E_IO;
    }
    std::stable_sort(mkdirs.begin(), mkdirs.end(),
                     [&](const Action *a, const Action *b) { return depth(a->path) < depth(b->path); });
    for (const Action *a : mkdirs) {
        if (graft_status s = check_fault(); s != GRAFT_OK) return s;
        if (graft_status s = t.log_step({"mkdir", a->path}); s != GRAFT_OK) return s;
        const fs::path p = join(t.root, a->path);
        if (!fs::create_directory(p, ec) || ec) return GRAFT_E_IO;
        set_mode(p, a->mode, ec);
        if (ec) return GRAFT_E_IO;
    }
    for (size_t i = 0; i < actions.size(); ++i) {
        const Action &a = actions[i];
        if (a.type != Action::Install) continue;
        if (a.replace)
            if (graft_status s = do_backup(a.path); s != GRAFT_OK) return s;
        if (graft_status s = check_fault(); s != GRAFT_OK) return s;
        const std::string id = std::to_string(stage_ids[i]);
        if (graft_status s = t.log_step({"install", a.path, id}); s != GRAFT_OK) return s;
        fs::rename(t.stage / id, join(t.root, a.path), ec);
        if (ec) return GRAFT_E_IO;
    }
    for (const auto &a : actions) {
        if (a.type != Action::Chmod) continue;
        if (graft_status s = check_fault(); s != GRAFT_OK) return s;
        char mode[16];
        std::snprintf(mode, sizeof mode, "%o", mode_of(join(t.root, a.path)));
        if (graft_status s = t.log_step({"chmod", a.path, mode}); s != GRAFT_OK) return s;
        set_mode(join(t.root, a.path), a.mode, ec);
        if (ec) return GRAFT_E_IO;
    }
    if (graft_status s = check_fault(); s != GRAFT_OK) return s;
    return t.log_step({"commit"});
}

graft_status update_impl(const char *package_path, const char *root_dir, const graft_update_options &o) {
    Log log(o.log, o.log_ctx);
    const fs::path root = u8path(root_dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        log(GRAFT_LOG_ERROR, std::string(root_dir) + ": not a directory");
        return GRAFT_E_INVALID_ARG;
    }
    if (!(o.flags & GRAFT_UPDATE_DRY_RUN))
        if (graft_status s = recover(root, log); s != GRAFT_OK) return s;

    Package pk;
    if (graft_status s = open_package(u8path(package_path), pk, log); s != GRAFT_OK) {
        if (s == GRAFT_E_CORRUPT) log(GRAFT_LOG_ERROR, std::string(package_path) + ": damaged or not a package");
        return s;
    }

    Planner pl{root, pk, o.limits, log, {}, {}, {}, {}, {}};
    pl.plan();
    if (!pl.conflicts.empty()) {
        for (const auto &c : pl.conflicts) log(GRAFT_LOG_ERROR, "conflict: " + c);
        return GRAFT_E_CONFLICT;
    }
    for (const auto &n : pl.notes) log(GRAFT_LOG_INFO, n);
    for (const auto &a : pl.actions) log(GRAFT_LOG_INFO, pl.describe(a));
    if (pl.actions.empty()) log(GRAFT_LOG_INFO, "already up to date");
    if ((o.flags & GRAFT_UPDATE_DRY_RUN) || pl.actions.empty()) return GRAFT_OK;

    Txn t(root);
    if (!fs::create_directory(t.dir, ec) || ec) {
        log(GRAFT_LOG_ERROR, u8str(t.dir) + ": can't create (another update running?)");
        return GRAFT_E_IO;
    }
    fs::create_directory(t.stage, ec);
    if (!ec) fs::create_directory(t.backup, ec);
    if (ec || !t.journal.open(t.dir / "journal", "wb")) {
        fs::remove_all(t.dir, ec);
        return GRAFT_E_IO;
    }

    // Stage: nothing outside .graft-txn changes yet.
    std::vector<size_t> stage_ids(pl.actions.size());
    size_t next = 0;
    for (size_t i = 0; i < pl.actions.size(); ++i) {
        const Action &a = pl.actions[i];
        if (a.type != Action::Install) continue;
        stage_ids[i] = next++;
        if (graft_status s = stage_one(root, pk, a, t.stage / std::to_string(stage_ids[i]), o.limits);
            s != GRAFT_OK || fault("stage", stage_ids[i]) == 1) {
            log(GRAFT_LOG_ERROR, "staging " + a.path + " failed: " + graft_status_string(s == GRAFT_OK ? GRAFT_E_IO : s));
            t.journal.close();
            make_tree_writable(t.dir);
            fs::remove_all(t.dir, ec);
            return s == GRAFT_OK ? GRAFT_E_IO : s;
        }
    }

    graft_status s;
    try {
        s = commit(t, pl.actions, stage_ids, log);
    } catch (const FaultCrash &) {
        return GRAFT_E_IO;  // test hook: simulate a crash; leave everything for recover()
    }
    if (s != GRAFT_OK) {
        log(GRAFT_LOG_ERROR, std::string("update failed (") + graft_status_string(s) + "); rolling back");
        rollback(t, log);
        t.journal.close();
        make_tree_writable(t.dir);
        fs::remove_all(t.dir, ec);
        return s;
    }
    t.journal.close();
    make_tree_writable(t.dir);
    fs::remove_all(t.dir, ec);
    if (ec) log(GRAFT_LOG_WARN, u8str(t.dir) + ": couldn't remove: " + ec.message());
    return GRAFT_OK;
}

graft_status describe_impl(const char *package_path, const Log &log) {
    Package pk;
    if (graft_status s = open_package(u8path(package_path), pk, log); s != GRAFT_OK) return s;
    const Manifest &m = pk.m;
    std::string from;
    for (const auto &l : m.labels) from += (from.empty() ? "" : ", ") + l;
    log(GRAFT_LOG_INFO, "updates [" + from + "] to " + m.target_label);
    for (const auto &e : m.entries) {
        switch (e.kind) {
        case kFile: {
            std::string line = "file    " + e.path + " (" + std::to_string(e.size) + " bytes";
            if (!e.regions.empty()) line += ", " + std::to_string(e.regions.size()) + " masked";
            line += ")";
            for (const auto &r : e.recipes) {
                const Blob &b = m.blobs[static_cast<size_t>(r.blob)];
                line += r.kind == kFull ? "  full" : "  delta";
                if (r.kind == kDelta && r.source_path != e.path) line += " from " + r.source_path;
                line += " " + std::to_string(b.length) + "B [" + join_labels(m, r.labels) + "]";
            }
            log(GRAFT_LOG_INFO, line);
            break;
        }
        case kSeed:
            log(GRAFT_LOG_INFO, "seed    " + e.path + " (" + std::to_string(e.size) +
                                    " bytes, user-owned: installed only where missing)");
            break;
        case kDir: log(GRAFT_LOG_INFO, "dir     " + e.path); break;
        case kSymlink: log(GRAFT_LOG_INFO, "symlink " + e.path + " -> " + e.link); break;
        case kRemove: log(GRAFT_LOG_INFO, "remove  " + e.path); break;
        default: break;
        }
    }
    return GRAFT_OK;
}

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

}  // namespace
}  // namespace graft::pkg

extern "C" void graft_update_options_init(graft_update_options *o) {
    if (!o) return;
    o->struct_size = sizeof *o;
    o->flags = 0;
    graft_apply_limits_init(&o->limits);
    o->log = nullptr;
    o->log_ctx = nullptr;
}

extern "C" graft_status graft_update(const char *package_path, const char *root_dir, const graft_update_options *opts) {
    if (!package_path || !root_dir) return GRAFT_E_INVALID_ARG;
    graft_update_options o;
    graft_update_options_init(&o);
    if (opts) {
        if (opts->struct_size < sizeof o || opts->limits.struct_size < sizeof o.limits) return GRAFT_E_INVALID_ARG;
        o = *opts;
    }
    return graft::pkg::guarded([&] { return graft::pkg::update_impl(package_path, root_dir, o); });
}

extern "C" graft_status graft_package_describe(const char *package_path, graft_log_fn log, void *log_ctx) {
    if (!package_path) return GRAFT_E_INVALID_ARG;
    return graft::pkg::guarded(
        [&] { return graft::pkg::describe_impl(package_path, graft::Log(log, log_ctx)); });
}
