#include "check.h"
#include "package_internal.h"

#include <zstd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <cstdlib>
#include <string>

namespace fs = std::filesystem;
using graft::pkg::Entry;
using graft::pkg::Manifest;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("graft-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
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

// Everything observable about a tree: kind, content, permissions, link target.
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

std::string random_text(std::mt19937 &rng, size_t n) {
    std::string s(n, ' ');
    for (auto &c : s) c = static_cast<char>('a' + rng() % 26);
    return s;
}

// Three releases with a little of everything: edits, a masked serial,
// a rename, additions, removals, a symlink, a mode change, a file that
// becomes a directory, and a directory that goes away.
struct Fixture {
    TempDir tmp;
    fs::path v1, v2, v3, pkg;
    std::string code;

    Fixture() : v1(tmp.path / "v1"), v2(tmp.path / "v2"), v3(tmp.path / "v3"), pkg(tmp.path / "p.gfp") {
        std::mt19937 rng(42);
        code = random_text(rng, 40000);
        const std::string serial = "SERIAL:0000-0000";
        write(v1 / "bin/app", code.substr(0, 20000) + serial + code.substr(20000));
        write(v2 / "bin/app", code.substr(0, 20000) + "v2" + serial + code.substr(20000));
        write(v3 / "bin/app", code.substr(0, 99) + "v3" + code.substr(99, 19901) + "v2" + serial + code.substr(20000));
        for (const auto &v : {v1, v2, v3}) write(v / "lib/same", code.substr(0, 5000));
        write(v1 / "lib/moved", code.substr(1000, 9000));
        write(v2 / "lib/moved", code.substr(1000, 9000));
        write(v3 / "bin/moved", code.substr(1000, 9000) + "!");
        write(v1 / "doc/gone.txt", "old docs");
        write(v2 / "doc/gone.txt", "old docs, revised");
        write(v1 / "olddir/a", "a");
        write(v2 / "olddir/a", "a");
        write(v1 / "thing", "a file");
        write(v2 / "thing", "a file");
        write(v3 / "thing/inside", "now a directory");
        write(v3 / "doc/new.txt", "new docs");
        write(v3 / "share/deep/x", "x");
        try_symlink("app", v3 / "bin/app-link");
        fs::permissions(v3 / "bin/app", fs::perms::owner_exec, fs::perm_options::add);
        fs::permissions(v3 / "lib/same", fs::perms::group_write, fs::perm_options::add);
    }

    static graft_status serial_mask(void *, const char *path, const uint8_t *o, size_t on, const uint8_t *n,
                                    size_t nn, graft_mask *out, size_t cap, size_t *count) {
        *count = 0;
        if (std::string(path) != "bin/app" || cap < 1) return GRAFT_OK;
        const std::string_view so(reinterpret_cast<const char *>(o), on), sn(reinterpret_cast<const char *>(n), nn);
        out[0] = {so.find("SERIAL:") + 7, 9, sn.find("SERIAL:") + 7, 9, GRAFT_MASK_EXACT, 0};
        *count = 1;
        return GRAFT_OK;
    }

    graft_status pack() {
        graft_pack_options o;
        graft_pack_options_init(&o);
        o.level = 3;
        o.masks_for = serial_mask;
        const std::string a = v1.string(), b = v2.string();
        const char *dirs[] = {a.c_str(), b.c_str()};
        const char *labels[] = {"1.0", "1.1"};
        return graft_pack(v3.string().c_str(), "2.0", dirs, labels, 2, pkg.string().c_str(), &o);
    }

    fs::path install(const fs::path &from, const std::string &name) {
        const fs::path dst = tmp.path / name;
        fs::copy(from, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        return dst;
    }
};

graft_status update(const fs::path &pkg, const fs::path &root, uint32_t flags = 0) {
    graft_update_options o;
    graft_update_options_init(&o);
    o.flags = flags;
    if (std::getenv("GRAFT_TEST_VERBOSE"))
        o.log = [](void *, int, const char *m) { std::fprintf(stderr, "    | %s\n", m); };
    return graft_update(pkg.string().c_str(), root.string().c_str(), &o);
}

void set_serial(const fs::path &app, const std::string &s) {
    std::string b = read(app);
    b.replace(b.find("SERIAL:") + 7, s.size(), s);
    write(app, b);
}

}  // namespace

TEST(package_updates_every_old_version) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const auto want = snapshot(f.v3);
    for (const auto &[from, name] : {std::pair{f.v1, "i1"}, std::pair{f.v2, "i2"}}) {
        const fs::path inst = f.install(from, name);
        write(inst / "user-notes.txt", "mine");  // not the package's business
        CHECK_EQ(update(f.pkg, inst), GRAFT_OK);
        auto got = snapshot(inst);
        CHECK(got.erase("user-notes.txt") == 1);
        CHECK(got == want);
        CHECK(!fs::exists(inst / graft::kTxnDir));
        CHECK_EQ(update(f.pkg, inst), GRAFT_OK);  // already current: a no-op
    }
}

TEST(package_keeps_installed_serial) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const fs::path inst = f.install(f.v1, "i");
    set_serial(inst / "bin/app", "CUST-0042");
    CHECK_EQ(update(f.pkg, inst), GRAFT_OK);
    std::string want = read(f.v3 / "bin/app");
    want.replace(want.find("SERIAL:") + 7, 9, "CUST-0042");
    CHECK(read(inst / "bin/app") == want);
    CHECK_EQ(update(f.pkg, inst), GRAFT_OK);  // stamped and current: still a no-op
    CHECK(read(inst / "bin/app") == want);
}

TEST(package_dry_run_changes_nothing) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const fs::path inst = f.install(f.v1, "i");
    const auto before = snapshot(inst);
    CHECK_EQ(update(f.pkg, inst, GRAFT_UPDATE_DRY_RUN), GRAFT_OK);
    CHECK(snapshot(inst) == before);
}

TEST(package_conflicts_change_nothing) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    // A locally modified file the update must patch, then one it must delete.
    for (const char *victim : {"lib/moved", "doc/gone.txt"}) {
        TempDir t;
        const fs::path inst = t.path / "i";
        fs::copy(f.v1, inst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        write(inst / victim, "local edits");
        const auto before = snapshot(inst);
        CHECK_EQ(update(f.pkg, inst), GRAFT_E_CONFLICT);
        CHECK(snapshot(inst) == before);
    }
}

TEST(package_refuses_symlinked_parent) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    if (!try_symlink("x", f.tmp.path / "probe")) return;  // no symlinks here
    const fs::path inst = f.install(f.v1, "i");
    const fs::path outside = f.tmp.path / "outside";
    fs::rename(inst / "lib", outside);
    fs::create_symlink(outside, inst / "lib");
    const auto before = snapshot(outside);
    CHECK_EQ(update(f.pkg, inst), GRAFT_E_CONFLICT);
    CHECK(snapshot(outside) == before);
}

TEST(package_rolls_back_on_failure_at_every_step) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const auto original = snapshot(f.v1);
    for (const char *phase : {"stage", "commit"}) {
        for (size_t k = 0;; ++k) {
            TempDir t;
            const fs::path inst = t.path / "i";
            fs::copy(f.v1, inst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
            static const char *s_phase;
            static size_t s_k;
            s_phase = phase;
            s_k = k;
            graft::pkg::g_fault_hook = [](const char *ph, size_t step) {
                return std::strcmp(ph, s_phase) == 0 && step == s_k ? 1 : 0;
            };
            const graft_status st = update(f.pkg, inst);
            graft::pkg::g_fault_hook = nullptr;
            if (st == GRAFT_OK) {  // k is past the last step
                CHECK(snapshot(inst) == snapshot(f.v3));
                CHECK(k > 3);
                break;
            }
            if (snapshot(inst) != original) {
                check::fail(__FILE__, __LINE__, std::string("not rolled back after ") + phase + " step " +
                                                    std::to_string(k));
                break;
            }
            if (k > 1000) break;
        }
    }
}

TEST(package_recovers_from_crash_at_every_step) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const auto want = snapshot(f.v3);
    for (size_t k = 0;; ++k) {
        TempDir t;
        const fs::path inst = t.path / "i";
        fs::copy(f.v1, inst, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        static size_t s_k;
        s_k = k;
        graft::pkg::g_fault_hook = [](const char *ph, size_t step) {
            return std::strcmp(ph, "commit") == 0 && step == s_k ? 2 : 0;
        };
        const graft_status st = update(f.pkg, inst);
        graft::pkg::g_fault_hook = nullptr;
        if (st == GRAFT_OK) break;  // past the last step
        CHECK(fs::exists(inst / graft::kTxnDir));  // a "crash" leaves the journal behind
        // The next run rolls the interrupted update back, then updates.
        CHECK_EQ(update(f.pkg, inst), GRAFT_OK);
        if (snapshot(inst) != want) {
            check::fail(__FILE__, __LINE__, "wrong tree after recovering from crash at step " + std::to_string(k));
            break;
        }
        if (k > 1000) break;
    }
}

TEST(package_rejects_damage) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const fs::path inst = f.install(f.v1, "i");
    const auto before = snapshot(inst);
    const std::string good = read(f.pkg);
    for (size_t at : {size_t{0}, size_t{13}, good.size() / 2, good.size() - 40, good.size() - 1}) {
        std::string bad = good;
        bad[at] = static_cast<char>(bad[at] ^ 0x40);
        write(f.pkg, bad);
        CHECK_EQ(update(f.pkg, inst), GRAFT_E_CORRUPT);
    }
    write(f.pkg, good.substr(0, good.size() - 1));
    CHECK_EQ(update(f.pkg, inst), GRAFT_E_CORRUPT);
    CHECK(snapshot(inst) == before);
}

namespace {

// Writes a well-formed container around an arbitrary manifest.
void write_package(const fs::path &p, const Manifest &m) {
    std::vector<uint8_t> raw, out(graft::pkg::kHeaderSize);
    graft::pkg::encode_manifest(m, raw);
    std::memcpy(out.data(), graft::pkg::kMagic, 8);
    graft::put_le<uint16_t>(out.data() + 8, 1);
    graft::put_le<uint16_t>(out.data() + 10, 0);
    std::vector<uint8_t> comp(ZSTD_compressBound(raw.size()));
    comp.resize(ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), 1));
    const uint64_t moff = out.size();
    out.insert(out.end(), comp.begin(), comp.end());
    uint8_t foot[24];
    graft::put_le<uint64_t>(foot, moff);
    graft::put_le<uint64_t>(foot + 8, comp.size());
    graft::put_le<uint64_t>(foot + 16, raw.size());
    out.insert(out.end(), foot, foot + 24);
    uint8_t h[GRAFT_HASH_SIZE];
    graft_hash(out.data(), out.size(), h);
    out.insert(out.end(), h, h + sizeof h);
    write(p, std::string(out.begin(), out.end()));
}

}  // namespace

TEST(package_rejects_hostile_manifests) {
    TempDir t;
    const fs::path root = t.path / "root", pkg = t.path / "evil.gfp";
    write(root / "keep", "keep");
    write(t.path / "victim", "victim");
    const auto before = snapshot(t.path);

    auto try_paths = [&](std::vector<std::string> paths) {
        Manifest m;
        for (const auto &p : paths) {
            Entry e;
            e.path = p;
            e.kind = graft::pkg::kRemove;
            e.what = graft::pkg::kDir;
            m.entries.push_back(e);
        }
        write_package(pkg, m);
        return update(pkg, root);
    };
    CHECK_EQ(try_paths({"../victim"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"a/../../victim"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"/etc"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"a//b"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"./keep"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"a\\b"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"c:evil"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({std::string("bad\nline")}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({".graft-txn/journal"}), GRAFT_E_CORRUPT);
    CHECK_EQ(try_paths({"Keep", "keep"}), GRAFT_E_CORRUPT);  // case collision
    CHECK_EQ(try_paths({"b", "a"}), GRAFT_E_CORRUPT);        // unsorted
    CHECK_EQ(try_paths({"fine"}), GRAFT_OK);                  // control: a harmless no-op
    auto after = snapshot(t.path);
    after.erase("evil.gfp");
    auto b2 = before;
    CHECK(after == b2);
}

TEST(package_describe) {
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    static std::string out;
    out.clear();
    CHECK_EQ(graft_package_describe(f.pkg.string().c_str(),
                                    [](void *, int, const char *m) { out += std::string(m) + "\n"; }, nullptr),
             GRAFT_OK);
    CHECK(out.find("updates [1.0, 1.1] to 2.0") != std::string::npos);
    CHECK(out.find("bin/moved") != std::string::npos);
    CHECK(out.find("from lib/moved") != std::string::npos);
    CHECK(out.find("1 masked") != std::string::npos);
}

TEST(package_user_owned_files) {
    // Modeled on TinyMUX's TOC.*.unpatched: configuration, the database and
    // customizable text ship once and then belong to the admin.
    TempDir t;
    const fs::path v1 = t.path / "v1", v2 = t.path / "v2", pkg = t.path / "p.gfp";
    std::mt19937 rng(9);
    const std::string code = random_text(rng, 20000);
    write(v1 / "bin/netmux", code);
    write(v2 / "bin/netmux", code + "v2");
    write(v1 / "game/netmux.conf", "port 2860\n");
    write(v2 / "game/netmux.conf", "port 2860\nnew_option yes\n");  // the default changed
    write(v1 / "game/data/netmux.db", "starter db");
    write(v2 / "game/data/netmux.db", "starter db v2");
    write(v1 / "game/text/motd.txt", "welcome");
    write(v2 / "game/text/motd.txt", "welcome!");
    write(v1 / "game/text/old.txt", "a screen dropped in v2");
    write(v2 / "game/muxssl.conf", "ssl defaults");  // new in v2

    graft_pack_options o;
    graft_pack_options_init(&o);
    o.level = 3;
    o.user_owned = [](void *, const char *p) {
        const std::string s = p;
        return s.rfind("game/text/", 0) == 0 || s == "game/netmux.conf" || s == "game/muxssl.conf" ||
                       s == "game/data/netmux.db"
                   ? 1
                   : 0;
    };
    const std::string a = v1.string();
    const char *dirs[] = {a.c_str()};
    CHECK_EQ(graft_pack(v2.string().c_str(), "v2", dirs, nullptr, 1, pkg.string().c_str(), &o), GRAFT_OK);

    // An admin's install: edited config, a grown database, a deleted screen.
    const fs::path inst = t.path / "inst";
    fs::copy(v1, inst, fs::copy_options::recursive);
    write(inst / "game/netmux.conf", "port 4201\n# mine\n");
    write(inst / "game/data/netmux.db", "a year of building");
    fs::remove(inst / "game/text/motd.txt");

    CHECK_EQ(update(pkg, inst), GRAFT_OK);  // no conflicts from anything the admin owns
    CHECK(read(inst / "bin/netmux") == code + "v2");                   // patched
    CHECK(read(inst / "game/netmux.conf") == "port 4201\n# mine\n");  // left alone
    CHECK(read(inst / "game/data/netmux.db") == "a year of building");
    CHECK(read(inst / "game/text/old.txt") == "a screen dropped in v2");  // not removed
    CHECK(read(inst / "game/muxssl.conf") == "ssl defaults");             // new: installed
    CHECK(read(inst / "game/text/motd.txt") == "welcome!");               // missing: reinstalled
    CHECK_EQ(update(pkg, inst), GRAFT_OK);                                // and idempotent

    // A fresh copy of v1 gets the same treatment: unedited defaults stay too.
    const fs::path fresh = t.path / "fresh";
    fs::copy(v1, fresh, fs::copy_options::recursive);
    CHECK_EQ(update(pkg, fresh), GRAFT_OK);
    CHECK(read(fresh / "game/netmux.conf") == "port 2860\n");
}

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

TEST(package_windows_refuses_locked_file) {
    // Like updating while netmux.exe is still running: the file is held
    // open without sharing. The update must refuse, name it, change nothing.
    Fixture f;
    CHECK_EQ(f.pack(), GRAFT_OK);
    const fs::path inst = f.install(f.v1, "i");
    const auto before = snapshot(inst);
    HANDLE h = CreateFileW((inst / "lib/moved").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    static std::string log;
    log.clear();
    graft_update_options o;
    graft_update_options_init(&o);
    o.log = [](void *, int, const char *m) { log += std::string(m) + "\n"; };
    CHECK_EQ(graft_update(f.pkg.string().c_str(), inst.string().c_str(), &o), GRAFT_E_CONFLICT);
    CHECK(log.find("lib/moved: in use") != std::string::npos);
    CloseHandle(h);
    CHECK(snapshot(inst) == before);
    CHECK_EQ(update(f.pkg, inst), GRAFT_OK);  // once released, it goes through
}

TEST(package_windows_refuses_reserved_names) {
    TempDir t;
    const fs::path root = t.path / "root", pkg = t.path / "p.gfp";
    fs::create_directories(root);
    for (const char *bad : {"CON", "nul.txt", "dir/com1", "lpt9.log", "trailing.", "trailing ", "a|b", "q?"}) {
        Manifest m;
        Entry e;
        e.path = bad;
        e.kind = graft::pkg::kDir;
        e.mode = 0755;
        m.entries.push_back(e);
        if (std::string(bad).find('/') != std::string::npos) {
            Entry d;
            d.path = "dir";
            d.kind = graft::pkg::kDir;
            d.mode = 0755;
            m.entries.insert(m.entries.begin(), d);
        }
        write_package(pkg, m);
        if (update(pkg, root) != GRAFT_E_CONFLICT)
            check::fail(__FILE__, __LINE__, std::string("accepted a reserved name: '") + bad + "'");
    }
}
#endif
