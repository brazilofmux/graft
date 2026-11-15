#include "archive_format.h"
#include "check.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("graft-arc-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            if (it->is_directory() && !it->is_symlink())
                fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

// Creating symlinks needs a privilege on Windows; tests skip those parts.
bool try_symlink(const fs::path &target, const fs::path &link) {
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    return !ec;
}

void write(const fs::path &p, const std::string &data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::map<std::string, std::string> snapshot(const fs::path &root) {
    std::map<std::string, std::string> s;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        const std::string rel = it->path().lexically_relative(root).generic_string();
        const auto st = it->symlink_status();
        const auto perms = std::to_string(static_cast<unsigned>(st.permissions() & fs::perms::mask));
        if (fs::is_symlink(st)) s[rel] = "L " + fs::read_symlink(it->path()).generic_string();
        else if (fs::is_directory(st)) s[rel] = "D " + perms;
        else s[rel] = "F " + perms + " " + read(it->path());
    }
    return s;
}

std::string text(std::mt19937 &rng, size_t n) {
    std::string s(n, ' ');
    for (auto &c : s) c = static_cast<char>('a' + rng() % 26);
    return s;
}

struct Archive {
    graft_archive *a = nullptr;
    graft_status st;
    Archive(const fs::path &p, uint32_t flags) {
        st = graft_archive_open(p.string().c_str(), flags, nullptr, nullptr, &a);
    }
    ~Archive() { graft_archive_close(a); }
    size_t count() const { return graft_archive_count(a); }
    graft_status add(const fs::path &dir, const std::string &label, int level = 3) {
        return graft_archive_add(a, dir.string().c_str(), label.c_str(), level);
    }
    graft_status extract(const std::string &label, const fs::path &dir) {
        return graft_archive_extract(a, label.c_str(), dir.string().c_str());
    }
};

// Three releases: edits, adds, removes, a symlink, modes, empty file and dir.
struct Releases {
    TempDir tmp;
    fs::path r[3];
    Releases() {
        std::mt19937 rng(7);
        const std::string code = text(rng, 30000);
        for (int i = 0; i < 3; ++i) r[i] = tmp.path / ("r" + std::to_string(i));
        write(r[0] / "bin/app", code);
        write(r[1] / "bin/app", code.substr(0, 1000) + "v1" + code.substr(1000));
        write(r[2] / "bin/app", code.substr(0, 1000) + "v1" + code.substr(1000, 9000) + "v2" + code.substr(10000));
        for (auto &d : r) write(d / "lib/shared", code.substr(5000, 5000));
        write(r[0] / "doc/old", "old");
        write(r[2] / "doc/new", "new");
        write(r[2] / "empty", "");
        fs::create_directories(r[2] / "var/empty-dir");
        try_symlink("app", r[2] / "bin/link");
        fs::permissions(r[2] / "bin/app", fs::perms::owner_exec, fs::perm_options::add);
    }
};

}  // namespace

TEST(archive_roundtrip) {
    Releases rs;
    const fs::path ap = rs.tmp.path / "a.gfa";
    {
        Archive a(ap, GRAFT_ARCHIVE_CREATE);
        CHECK_EQ(a.st, GRAFT_OK);
        for (int i = 0; i < 3; ++i) CHECK_EQ(a.add(rs.r[i], "v" + std::to_string(i)), GRAFT_OK);
        CHECK_EQ(a.count(), 3u);
        CHECK_EQ(a.add(rs.r[0], "v1"), GRAFT_E_INVALID_ARG);  // duplicate label
    }
    Archive a(ap, 0);  // reopen read-only
    CHECK_EQ(a.st, GRAFT_OK);
    CHECK_EQ(a.count(), 3u);
    for (int i = 0; i < 3; ++i) {
        const fs::path out = rs.tmp.path / ("x" + std::to_string(i));
        CHECK_EQ(a.extract("v" + std::to_string(i), out), GRAFT_OK);
        CHECK(snapshot(out) == snapshot(rs.r[i]));
    }
    CHECK_EQ(a.extract("v0", rs.tmp.path / "x0"), GRAFT_E_INVALID_ARG);  // exists
    CHECK_EQ(a.extract("nope", rs.tmp.path / "y"), GRAFT_E_INVALID_ARG);
    CHECK_EQ(a.add(rs.r[0], "v9"), GRAFT_E_INVALID_ARG);  // opened read-only

    graft_archive_version v;
    CHECK_EQ(graft_archive_get(a.a, 2, &v), GRAFT_OK);
    CHECK_EQ(std::string(v.label), std::string("v2"));
    CHECK_EQ(v.file_count, 4u);  // bin/app, lib/shared, doc/new, empty (the link isn't a file)
}

TEST(archive_long_history_is_compact_and_exact) {
    TempDir t;
    std::mt19937 rng(8);
    std::string content = text(rng, 50000);
    const fs::path ap = t.path / "a.gfa";
    std::vector<std::string> versions;
    {
        Archive a(ap, GRAFT_ARCHIVE_CREATE);
        for (int i = 0; i < 20; ++i) {
            content.insert(rng() % content.size(), text(rng, 20));  // a small edit per version
            versions.push_back(content);
            const fs::path d = t.path / ("src" + std::to_string(i));
            write(d / "f", content);
            CHECK_EQ(a.add(d, std::to_string(i)), GRAFT_OK);
        }
    }
    // 20 versions of ~50 KB should cost little more than one: deltas, with
    // a full copy every few versions to bound the chains.
    CHECK(fs::file_size(ap) < 3 * 50000);
    Archive a(ap, 0);
    for (int i = 0; i < 20; ++i) {
        const fs::path out = t.path / ("x" + std::to_string(i));
        CHECK_EQ(a.extract(std::to_string(i), out), GRAFT_OK);
        CHECK(read(out / "f") == versions[static_cast<size_t>(i)]);
    }
}

TEST(archive_survives_interrupted_add) {
    Releases rs;
    const fs::path ap = rs.tmp.path / "a.gfa";
    {
        Archive a(ap, GRAFT_ARCHIVE_CREATE);
        CHECK_EQ(a.add(rs.r[0], "v0"), GRAFT_OK);
        CHECK_EQ(a.add(rs.r[1], "v1"), GRAFT_OK);
    }
    const auto good_size = fs::file_size(ap);
    // A torn add: contents written, no segment/footer yet.
    std::ofstream(ap, std::ios::binary | std::ios::app) << std::string(5000, 'z') << "GFASEG";
    {
        Archive a(ap, GRAFT_ARCHIVE_WRITE);
        CHECK_EQ(a.st, GRAFT_OK);
        CHECK_EQ(a.count(), 2u);
        CHECK_EQ(a.add(rs.r[2], "v2"), GRAFT_OK);  // trims the tail first
    }
    Archive a(ap, 0);
    CHECK_EQ(a.st, GRAFT_OK);
    CHECK_EQ(a.count(), 3u);
    CHECK_EQ(a.extract("v2", rs.tmp.path / "x2"), GRAFT_OK);
    CHECK(snapshot(rs.tmp.path / "x2") == snapshot(rs.r[2]));
    CHECK(fs::file_size(ap) > good_size);

    // An archive whose very first add was interrupted is just empty.
    const fs::path bp = rs.tmp.path / "b.gfa";
    { Archive b(bp, GRAFT_ARCHIVE_CREATE); }
    std::ofstream(bp, std::ios::binary | std::ios::app) << std::string(100, 'q');
    Archive b(bp, GRAFT_ARCHIVE_WRITE);
    CHECK_EQ(b.st, GRAFT_OK);
    CHECK_EQ(b.count(), 0u);
    CHECK_EQ(b.add(rs.r[0], "first"), GRAFT_OK);
}

TEST(archive_detects_damage) {
    Releases rs;
    const fs::path ap = rs.tmp.path / "a.gfa";
    {
        Archive a(ap, GRAFT_ARCHIVE_CREATE);
        CHECK_EQ(a.add(rs.r[0], "v0"), GRAFT_OK);
        CHECK_EQ(a.add(rs.r[1], "v1"), GRAFT_OK);
    }
    const std::string good = read(ap);
    auto flip = [&](size_t at) {
        std::string bad = good;
        bad[at] = static_cast<char>(bad[at] ^ 1);
        write(ap, bad);
    };
    // The last segment's bytes (just before the footer): the footer hash fails.
    flip(good.size() - graft::arc::kFooterSize - 3);
    { Archive a(ap, 0); CHECK_EQ(a.st, GRAFT_E_CORRUPT); }
    // The first content (bin/app of v0, stored in full right after the header).
    flip(graft::arc::kHeaderSize + 20);
    {
        Archive a(ap, 0);
        CHECK_EQ(a.st, GRAFT_OK);  // segments are fine...
        CHECK_EQ(a.extract("v0", rs.tmp.path / "x"), GRAFT_E_CORRUPT);  // ...the content isn't
        CHECK(!fs::exists(rs.tmp.path / "x"));
        CHECK(!fs::exists(rs.tmp.path / "x.graft-tmp"));
    }
    // Not an archive at all.
    write(ap, "hello, world");
    { Archive a(ap, 0); CHECK(a.st != GRAFT_OK); }
}

TEST(archive_prune) {
    Releases rs;
    const fs::path ap = rs.tmp.path / "a.gfa";
    {
        Archive a(ap, GRAFT_ARCHIVE_CREATE);
        for (int i = 0; i < 3; ++i) CHECK_EQ(a.add(rs.r[i], "v" + std::to_string(i)), GRAFT_OK);
    }
    const char *keep[] = {"v0", "v2"};
    CHECK_EQ(graft_archive_prune(ap.string().c_str(), keep, 2, 3, nullptr, nullptr), GRAFT_OK);
    const char *missing[] = {"nope"};
    CHECK_EQ(graft_archive_prune(ap.string().c_str(), missing, 1, 3, nullptr, nullptr), GRAFT_E_INVALID_ARG);
    Archive a(ap, 0);
    CHECK_EQ(a.st, GRAFT_OK);
    CHECK_EQ(a.count(), 2u);
    CHECK_EQ(a.extract("v0", rs.tmp.path / "x0"), GRAFT_OK);
    CHECK_EQ(a.extract("v2", rs.tmp.path / "x2"), GRAFT_OK);
    CHECK(snapshot(rs.tmp.path / "x0") == snapshot(rs.r[0]));
    CHECK(snapshot(rs.tmp.path / "x2") == snapshot(rs.r[2]));
    CHECK_EQ(a.extract("v1", rs.tmp.path / "x1"), GRAFT_E_INVALID_ARG);
}

TEST(archive_mkpatch_updates_old_versions) {
    Releases rs;
    const fs::path ap = rs.tmp.path / "a.gfa", pkg = rs.tmp.path / "p.gfp";
    Archive a(ap, GRAFT_ARCHIVE_CREATE);
    for (int i = 0; i < 3; ++i) CHECK_EQ(a.add(rs.r[i], "v" + std::to_string(i)), GRAFT_OK);
    graft_pack_options o;
    graft_pack_options_init(&o);
    o.level = 3;
    CHECK_EQ(graft_archive_mkpatch(a.a, "v2", nullptr, 0, pkg.string().c_str(), &o), GRAFT_OK);
    for (const char *from : {"v0", "v1"}) {
        const fs::path inst = rs.tmp.path / (std::string("inst-") + from);
        CHECK_EQ(a.extract(from, inst), GRAFT_OK);
        CHECK_EQ(graft_update(pkg.string().c_str(), inst.string().c_str(), nullptr), GRAFT_OK);
        CHECK(snapshot(inst) == snapshot(rs.r[2]));
    }
}

TEST(archive_rejects_hostile_segments) {
    using namespace graft::arc;
    auto accepts = [](std::vector<VEntry> entries, std::vector<Object> objs = {}) {
        Version v;
        v.label = "x";
        v.entries = std::move(entries);
        std::vector<uint8_t> raw;
        encode_segment(objs, v, raw);
        std::vector<Object> into;
        Version out;
        return decode_segment(raw.data(), raw.size(), into, out, 1 << 20);
    };
    Object full;
    full.offset = kHeaderSize;
    full.length = 10;
    auto file = [](const std::string &p) { return VEntry{p, graft::kNodeFile, 0644, 0, {}}; };
    auto dir = [](const std::string &p) { return VEntry{p, graft::kNodeDir, 0755, 0, {}}; };
    auto link = [](const std::string &p) { return VEntry{p, graft::kNodeSymlink, 0, 0, "/etc"}; };

    CHECK(accepts({dir("a"), file("a/b")}, {full}));                // control
    CHECK(!accepts({link("a"), file("a/b")}, {full}));              // write through a symlink
    CHECK(!accepts({file("a"), file("a/b")}, {full}));              // below a file
    CHECK(!accepts({file("a/b")}, {full}));                         // parent missing
    CHECK(!accepts({file("../x")}, {full}));                        // traversal
    CHECK(!accepts({file("A"), file("a")}, {full}));                // case collision
    CHECK(!accepts({file("b"), file("a")}, {full}));                // unsorted
    CHECK(!accepts({file(".graft-txn")}, {full}));                  // reserved
    CHECK(!accepts({file("a")}, {}));                               // object out of range
    Object fwd;
    fwd.kind = kObjDelta;
    fwd.base = 0;  // refers to itself
    fwd.offset = kHeaderSize;
    CHECK(!accepts({}, {fwd}));
    Object past = full;
    past.length = 2 << 20;  // beyond the data
    CHECK(!accepts({}, {past}));
}
