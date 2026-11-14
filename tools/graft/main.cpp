// graft: command-line front end to libgraft.

#include "graft/graft.h"
#include "masks.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  define GRAFT_HAVE_MMAP 1
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

void usage(std::FILE *out) {
    std::fputs(
        "usage: graft <command> [args]\n"
        "\n"
        "commands:\n"
        "  diff [-l LEVEL] [-b BLOCK] [-f FILTER] [--mask SPEC]... [--mask-file FILE] OLD NEW PATCH\n"
        "                   write a patch turning OLD into NEW. Masked regions\n"
        "                   keep the installed file's bytes (serials, keys):\n"
        "                     at=OFF:LEN | old=OFF:LEN,new=OFF:LEN |\n"
        "                     marker=TEXT|hex:HEX,len=N[,skip=K]\n"
        "                     [,newlen=N,pad=BYTE|truncate]\n"
        "                   FILTER: auto (default), none, x86, x86-16, z80, arm64, slow32\n"
        "                   -m METHOD: auto (default), suffix (smallest), rolling (big files)\n"
        "  apply OLD PATCH NEW\n"
        "                   apply PATCH to OLD, writing NEW\n"
        "  verify OLD PATCH check that PATCH applies cleanly to OLD\n"
        "  info PATCH       print a patch's header\n"
        "\n"
        "  pack [-l LEVEL] [--label NAME] [--mask GLOB=SPEC]... [--mask-file FILE]\n"
        "       [--filter GLOB=FILTER]...\n"
        "       -o PACKAGE NEW_DIR OLD_DIR[=LABEL]...\n"
        "                   write a package updating any OLD_DIR tree to NEW_DIR.\n"
        "                   GLOB: * within a path component, ** across, ? one char\n"
        "  update [--dry-run] PACKAGE DIR\n"
        "                   update the tree at DIR in place (all or nothing)\n"
        "  list PACKAGE     show a package's versions and contents\n"
        "\n"
        "  archive add [-l LEVEL] ARCHIVE DIR LABEL   add DIR as a new version\n"
        "                                             (creates ARCHIVE if needed)\n"
        "  archive list ARCHIVE\n"
        "  archive extract ARCHIVE LABEL DIR          DIR must not exist\n"
        "  archive diff ARCHIVE FROM TO\n"
        "  archive mkpatch [pack options] -o PACKAGE ARCHIVE TO [FROM...]\n"
        "                                             package from FROM versions\n"
        "                                             (default: all others) to TO\n"
        "  archive drop [-l LEVEL] ARCHIVE LABEL...   remove versions (rewrites)\n"
        "  hash FILE...     print the BLAKE3-256 of each FILE ('-' = stdin)\n"
        "  version          print graft and dependency versions\n"
        "  help             show this message\n",
        out);
}

void hex(std::FILE *out, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; ++i) std::fprintf(out, "%02x", d[i]);
}

int fail(const char *what, graft_status st) {
    std::fprintf(stderr, "graft: %s: %s\n", what, graft_status_string(st));
    return kExitFail;
}

int fail_errno(const char *what) {
    std::fprintf(stderr, "graft: %s: %s\n", what, std::strerror(errno));
    return kExitFail;
}

// --- input files ---------------------------------------------------------------

// A whole file, memory-mapped where possible.
class InputFile {
public:
    bool open(const char *path) {
#if GRAFT_HAVE_MMAP
        const int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0) {
            ::close(fd);
            return false;
        }
        size_ = static_cast<size_t>(st.st_size);
        if (size_ > 0) {
            void *p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                return false;
            }
            map_ = p;
            data_ = static_cast<const uint8_t *>(p);
        }
        ::close(fd);
        return true;
#else
        std::FILE *f = std::fopen(path, "rb");
        if (!f) return false;
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) owned_.insert(owned_.end(), buf, buf + n);
        const bool ok = !std::ferror(f);
        std::fclose(f);
        data_ = owned_.data();
        size_ = owned_.size();
        return ok;
#endif
    }
    ~InputFile() {
#if GRAFT_HAVE_MMAP
        if (map_) munmap(map_, size_);
#endif
    }
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    void *map_ = nullptr;
    std::vector<uint8_t> owned_;
};

graft_status file_read(void *ctx, void *buf, size_t cap, size_t *got) {
    auto *f = static_cast<std::FILE *>(ctx);
    *got = std::fread(buf, 1, cap, f);
    return std::ferror(f) ? GRAFT_E_IO : GRAFT_OK;
}

graft_status file_write(void *ctx, const void *buf, size_t len) {
    return std::fwrite(buf, 1, len, static_cast<std::FILE *>(ctx)) == len ? GRAFT_OK : GRAFT_E_IO;
}

graft_status discard_write(void *, const void *, size_t) { return GRAFT_OK; }

// --- output files: written to a temporary, renamed into place on success -------

class OutputFile {
public:
    explicit OutputFile(std::string path) : path_(std::move(path)), tmp_(path_ + ".graft-tmp") {}
    ~OutputFile() {
        if (f_) std::fclose(f_);
        if (!committed_) std::remove(tmp_.c_str());
    }
    bool open() {
        f_ = std::fopen(tmp_.c_str(), "wbx");  // exclusive: never clobber a stray temp
        return f_ != nullptr;
    }
    std::FILE *file() const { return f_; }
    const std::string &tmp_path() const { return tmp_; }
    bool commit() {
        const bool ok = std::fflush(f_) == 0 && !std::ferror(f_);
        std::fclose(f_);
        f_ = nullptr;
        if (!ok || std::rename(tmp_.c_str(), path_.c_str()) != 0) return false;
        committed_ = true;
        return true;
    }

private:
    std::string path_, tmp_;
    std::FILE *f_ = nullptr;
    bool committed_ = false;
};

// --- commands ------------------------------------------------------------------

const char *const kFilterNames[] = {"none", "x86", "x86-16", "z80", "arm64", "slow32"};

bool parse_filter(std::string_view name, uint8_t &out) {
    if (name == "auto") {
        out = GRAFT_FILTER_AUTO;
        return true;
    }
    for (uint8_t i = 0; i < sizeof kFilterNames / sizeof *kFilterNames; ++i)
        if (name == kFilterNames[i]) {
            out = i;
            return true;
        }
    return false;
}

bool parse_uint(const char *s, unsigned long &out) {
    char *end;
    errno = 0;
    out = std::strtoul(s, &end, 0);
    return errno == 0 && end != s && *end == '\0';
}

int cmd_diff(int argc, char **argv) {
    graft_diff_options opts;
    graft_diff_options_init(&opts);
    std::vector<const char *> pos;
    std::vector<std::string> specs;
    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        unsigned long v;
        if ((a == "-l" || a == "--level") && i + 1 < argc && parse_uint(argv[i + 1], v)) {
            opts.level = static_cast<int>(v);
            ++i;
        } else if ((a == "-b" || a == "--block-size") && i + 1 < argc && parse_uint(argv[i + 1], v)) {
            opts.block_size = static_cast<uint32_t>(v);
            ++i;
        } else if ((a == "-m" || a == "--method") && i + 1 < argc) {
            const std::string_view m = argv[++i];
            if (m == "auto") opts.method = GRAFT_METHOD_AUTO;
            else if (m == "suffix") opts.method = GRAFT_METHOD_SUFFIX;
            else if (m == "rolling") opts.method = GRAFT_METHOD_ROLLING;
            else {
                std::fprintf(stderr, "graft diff: unknown method '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if ((a == "-f" || a == "--filter") && i + 1 < argc) {
            if (!parse_filter(argv[++i], opts.filter)) {
                std::fprintf(stderr, "graft diff: unknown filter '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if (a == "--mask" && i + 1 < argc) {
            specs.emplace_back(argv[++i]);
        } else if (a == "--mask-file" && i + 1 < argc) {
            std::string err;
            if (!cli::read_mask_file(argv[++i], specs, err)) {
                std::fprintf(stderr, "graft diff: %s\n", err.c_str());
                return kExitFail;
            }
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "graft diff: bad option '%s'\n", argv[i]);
            return kExitUsage;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() != 3) {
        std::fputs("usage: graft diff [-l LEVEL] [-b BLOCK] OLD NEW PATCH\n", stderr);
        return kExitUsage;
    }
    InputFile oldf, newf;
    if (!oldf.open(pos[0])) return fail_errno(pos[0]);
    if (!newf.open(pos[1])) return fail_errno(pos[1]);

    std::vector<graft_mask> masks;
    for (const auto &spec : specs) {
        graft_mask m;
        std::string err;
        if (!cli::resolve_mask(spec, oldf.data(), oldf.size(), newf.data(), newf.size(), m, err)) {
            std::fprintf(stderr, "graft diff: mask '%s': %s\n", spec.c_str(), err.c_str());
            return kExitFail;
        }
        std::fprintf(stderr, "mask: OLD 0x%llx+%llu -> NEW 0x%llx+%llu\n", static_cast<unsigned long long>(m.old_offset),
                     static_cast<unsigned long long>(m.old_length), static_cast<unsigned long long>(m.new_offset),
                     static_cast<unsigned long long>(m.new_length));
        masks.push_back(m);
    }
    opts.masks = masks.data();
    opts.mask_count = masks.size();

    OutputFile out(pos[2]);
    if (!out.open()) return fail_errno(out.tmp_path().c_str());
    graft_sink sink{out.file(), file_write};
    if (graft_status st = graft_diff(oldf.data(), oldf.size(), newf.data(), newf.size(), &opts, &sink); st != GRAFT_OK)
        return fail("diff", st);
    if (!out.commit()) return fail_errno(pos[2]);
    return kExitOk;
}

int run_apply(const char *old_path, const char *patch_path, const char *new_path) {
    InputFile oldf;
    if (!oldf.open(old_path)) return fail_errno(old_path);
    std::FILE *pf = std::strcmp(patch_path, "-") == 0 ? stdin : std::fopen(patch_path, "rb");
    if (!pf) return fail_errno(patch_path);
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> pf_guard(pf == stdin ? nullptr : pf, std::fclose);

    graft_source src = graft_source_from_memory(oldf.data(), oldf.size());
    graft_reader rd{pf, file_read};

    if (!new_path) {
        graft_sink sink{nullptr, discard_write};
        if (graft_status st = graft_apply(&src, &rd, &sink, nullptr); st != GRAFT_OK) return fail("verify", st);
        std::puts("ok");
        return kExitOk;
    }
    OutputFile out(new_path);
    if (!out.open()) return fail_errno(out.tmp_path().c_str());
    graft_sink sink{out.file(), file_write};
    if (graft_status st = graft_apply(&src, &rd, &sink, nullptr); st != GRAFT_OK) return fail("apply", st);
    if (!out.commit()) return fail_errno(new_path);
    return kExitOk;
}

// --- packages ------------------------------------------------------------------

void log_to_stderr(void *, int level, const char *msg) {
    std::fprintf(level == GRAFT_LOG_INFO ? stdout : stderr, "%s%s\n",
                 level == GRAFT_LOG_ERROR ? "error: " : level == GRAFT_LOG_WARN ? "warning: " : "", msg);
}

// '*' matches within a path component, '**' across components, '?' one character.
bool glob_match(const char *g, const char *s) {
    for (; *g; ++g) {
        if (g[0] == '*' && g[1] == '*') {
            g += 2;
            if (*g == '/') ++g;  // "**/" also matches zero directories
            for (const char *t = s;; ++t) {
                if (glob_match(g, t)) return true;
                if (!*t) return false;
            }
        }
        if (*g == '*') {
            for (const char *t = s;; ++t) {
                if (glob_match(g + 1, t)) return true;
                if (!*t || *t == '/') return false;
            }
        }
        if (!*s || (*g == '?' ? *s == '/' : *g != *s)) return false;
        ++s;
    }
    return *s == '\0';
}

struct MaskRules {
    std::vector<std::pair<std::string, std::string>> rules;  // glob, spec
    std::string error;
};

struct FilterRules {
    std::vector<std::pair<std::string, uint8_t>> rules;  // glob, filter; first match wins
};

uint8_t filter_for(void *ctx, const char *path) {
    for (const auto &[glob, f] : static_cast<FilterRules *>(ctx)->rules)
        if (glob_match(glob.c_str(), path)) return f;
    return GRAFT_FILTER_AUTO;
}

bool add_filter_rule(FilterRules &fr, const std::string &item) {
    const auto eq = item.rfind('=');
    uint8_t f;
    if (eq == std::string::npos || eq == 0 || !parse_filter(item.substr(eq + 1), f)) return false;
    fr.rules.emplace_back(item.substr(0, eq), f);
    return true;
}

graft_status masks_for(void *ctx, const char *path, const uint8_t *oldp, size_t oldn, const uint8_t *newp, size_t newn,
                       graft_mask *out, size_t cap, size_t *count) {
    auto *mr = static_cast<MaskRules *>(ctx);
    *count = 0;
    for (const auto &[glob, spec] : mr->rules) {
        if (!glob_match(glob.c_str(), path)) continue;
        if (*count == cap) {
            std::fprintf(stderr, "error: %s: too many masks\n", path);
            return GRAFT_E_LIMIT;
        }
        std::string err;
        if (!cli::resolve_mask(spec, oldp, oldn, newp, newn, out[*count], err)) {
            std::fprintf(stderr, "error: %s: mask '%s': %s\n", path, spec.c_str(), err.c_str());
            return GRAFT_E_INVALID_ARG;
        }
        ++*count;
    }
    return GRAFT_OK;
}

bool add_rule(MaskRules &mr, const std::string &item) {
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    mr.rules.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    return true;
}

int cmd_pack(int argc, char **argv) {
    graft_pack_options opts;
    graft_pack_options_init(&opts);
    MaskRules rules;
    FilterRules frules;
    const char *out = nullptr, *label = nullptr;
    std::vector<const char *> pos;
    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        unsigned long v;
        if ((a == "-l" || a == "--level") && i + 1 < argc && parse_uint(argv[i + 1], v)) {
            opts.level = static_cast<int>(v);
            ++i;
        } else if (a == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == "--label" && i + 1 < argc) {
            label = argv[++i];
        } else if (a == "--filter" && i + 1 < argc) {
            if (!add_filter_rule(frules, argv[++i])) {
                std::fprintf(stderr, "graft pack: --filter wants GLOB=NAME, got '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if (a == "--mask" && i + 1 < argc) {
            if (!add_rule(rules, argv[++i])) {
                std::fprintf(stderr, "graft pack: --mask wants GLOB=SPEC, got '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if (a == "--mask-file" && i + 1 < argc) {
            std::vector<std::string> lines;
            std::string err;
            if (!cli::read_mask_file(argv[++i], lines, err)) {
                std::fprintf(stderr, "graft pack: %s\n", err.c_str());
                return kExitFail;
            }
            for (const auto &l : lines)
                if (!add_rule(rules, l)) {
                    std::fprintf(stderr, "graft pack: %s: lines are GLOB=SPEC, got '%s'\n", argv[i], l.c_str());
                    return kExitUsage;
                }
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "graft pack: bad option '%s'\n", argv[i]);
            return kExitUsage;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (!out || pos.size() < 2) {
        std::fputs("usage: graft pack [options] -o PACKAGE NEW_DIR OLD_DIR[=LABEL]...\n", stderr);
        return kExitUsage;
    }
    std::vector<std::string> dirs, labels;
    for (size_t i = 1; i < pos.size(); ++i) {
        std::string d = pos[i], l;
        if (const auto eq = d.rfind('='); eq != std::string::npos) {
            l = d.substr(eq + 1);
            d.resize(eq);
        } else {
            std::string base = d;
            while (base.size() > 1 && base.back() == '/') base.pop_back();
            l = base.substr(base.rfind('/') == std::string::npos ? 0 : base.rfind('/') + 1);
        }
        dirs.push_back(d);
        labels.push_back(l);
    }
    std::vector<const char *> dp, lp;
    for (size_t i = 0; i < dirs.size(); ++i) {
        dp.push_back(dirs[i].c_str());
        lp.push_back(labels[i].c_str());
    }
    std::string new_label = label ? label : pos[0];
    if (rules.rules.size()) {
        opts.masks_for = masks_for;
        opts.masks_ctx = &rules;
    }
    if (frules.rules.size()) {
        opts.filter_for = filter_for;
        opts.filter_ctx = &frules;
    }
    opts.log = log_to_stderr;
    if (graft_status st = graft_pack(pos[0], new_label.c_str(), dp.data(), lp.data(), dp.size(), out, &opts);
        st != GRAFT_OK)
        return fail("pack", st);
    return kExitOk;
}

int cmd_update(int argc, char **argv) {
    graft_update_options opts;
    graft_update_options_init(&opts);
    opts.log = log_to_stderr;
    std::vector<const char *> pos;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0 || std::strcmp(argv[i], "-n") == 0) {
            opts.flags |= GRAFT_UPDATE_DRY_RUN;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            std::fprintf(stderr, "graft update: bad option '%s'\n", argv[i]);
            return kExitUsage;
        } else {
            pos.push_back(argv[i]);
        }
    }
    if (pos.size() != 2) {
        std::fputs("usage: graft update [--dry-run] PACKAGE DIR\n", stderr);
        return kExitUsage;
    }
    if (graft_status st = graft_update(pos[0], pos[1], &opts); st != GRAFT_OK) return fail("update", st);
    return kExitOk;
}

// --- archives ------------------------------------------------------------------

struct ArchiveCloser {
    void operator()(graft_archive *a) const { graft_archive_close(a); }
};
using ArchivePtr = std::unique_ptr<graft_archive, ArchiveCloser>;

int open_archive(const char *path, uint32_t flags, ArchivePtr &out) {
    graft_archive *a = nullptr;
    if (graft_status st = graft_archive_open(path, flags, log_to_stderr, nullptr, &a); st != GRAFT_OK)
        return fail(path, st);
    out.reset(a);
    return kExitOk;
}

int cmd_archive(int argc, char **argv) {
    if (argc < 1) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string_view sub = argv[0];
    int level = GRAFT_DEFAULT_LEVEL;
    std::vector<const char *> pos;
    const char *out = nullptr;
    graft_pack_options popts;
    graft_pack_options_init(&popts);
    MaskRules rules;
    FilterRules frules;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        unsigned long v;
        if ((a == "-l" || a == "--level") && i + 1 < argc && parse_uint(argv[i + 1], v)) {
            level = static_cast<int>(v);
            ++i;
        } else if (sub == "mkpatch" && a == "-o" && i + 1 < argc) {
            out = argv[++i];
        } else if (sub == "mkpatch" && a == "--mask" && i + 1 < argc) {
            if (!add_rule(rules, argv[++i])) {
                std::fprintf(stderr, "graft archive mkpatch: --mask wants GLOB=SPEC\n");
                return kExitUsage;
            }
        } else if (sub == "mkpatch" && a == "--filter" && i + 1 < argc) {
            if (!add_filter_rule(frules, argv[++i])) {
                std::fprintf(stderr, "graft archive mkpatch: --filter wants GLOB=NAME\n");
                return kExitUsage;
            }
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "graft archive %s: bad option '%s'\n", argv[0], argv[i]);
            return kExitUsage;
        } else {
            pos.push_back(argv[i]);
        }
    }
    auto bad_args = [&]() {
        std::fprintf(stderr, "graft archive %s: wrong arguments\n", argv[0]);
        usage(stderr);
        return kExitUsage;
    };
    ArchivePtr ar;

    if (sub == "add") {
        if (pos.size() != 3) return bad_args();
        if (int rc = open_archive(pos[0], GRAFT_ARCHIVE_CREATE, ar); rc != kExitOk) return rc;
        if (graft_status st = graft_archive_add(ar.get(), pos[1], pos[2], level); st != GRAFT_OK)
            return fail("archive add", st);
        return kExitOk;
    }
    if (sub == "list") {
        if (pos.size() != 1) return bad_args();
        if (int rc = open_archive(pos[0], 0, ar); rc != kExitOk) return rc;
        uint64_t content = 0;
        for (size_t i = 0; i < graft_archive_count(ar.get()); ++i) {
            graft_archive_version v;
            graft_archive_get(ar.get(), i, &v);
            char when[32] = "?";
            const std::time_t t = static_cast<std::time_t>(v.time);
            if (const std::tm *tm = std::gmtime(&t)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC", tm);
            std::printf("%-20s %s  %8llu files  %12llu bytes\n", v.label, when,
                        static_cast<unsigned long long>(v.file_count), static_cast<unsigned long long>(v.total_size));
            content += v.total_size;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(pos[0], ec);
        std::printf("%zu versions, %llu bytes of content in a %llu-byte archive\n", graft_archive_count(ar.get()),
                    static_cast<unsigned long long>(content), static_cast<unsigned long long>(size));
        return kExitOk;
    }
    if (sub == "extract") {
        if (pos.size() != 3) return bad_args();
        if (int rc = open_archive(pos[0], 0, ar); rc != kExitOk) return rc;
        if (graft_status st = graft_archive_extract(ar.get(), pos[1], pos[2]); st != GRAFT_OK)
            return fail("archive extract", st);
        return kExitOk;
    }
    if (sub == "diff") {
        if (pos.size() != 3) return bad_args();
        if (int rc = open_archive(pos[0], 0, ar); rc != kExitOk) return rc;
        if (graft_status st = graft_archive_diff(ar.get(), pos[1], pos[2]); st != GRAFT_OK)
            return fail("archive diff", st);
        return kExitOk;
    }
    if (sub == "mkpatch") {
        if (!out || pos.size() < 2) return bad_args();
        if (int rc = open_archive(pos[0], 0, ar); rc != kExitOk) return rc;
        popts.level = level;
        popts.log = log_to_stderr;
        if (!rules.rules.empty()) {
            popts.masks_for = masks_for;
            popts.masks_ctx = &rules;
        }
        if (!frules.rules.empty()) {
            popts.filter_for = filter_for;
            popts.filter_ctx = &frules;
        }
        if (graft_status st = graft_archive_mkpatch(ar.get(), pos[1], pos.data() + 2, pos.size() - 2, out, &popts);
            st != GRAFT_OK)
            return fail("archive mkpatch", st);
        return kExitOk;
    }
    if (sub == "drop") {
        if (pos.size() < 2) return bad_args();
        if (int rc = open_archive(pos[0], 0, ar); rc != kExitOk) return rc;
        std::vector<std::string> keep;
        size_t dropped = 0;
        for (size_t i = 0; i < graft_archive_count(ar.get()); ++i) {
            graft_archive_version v;
            graft_archive_get(ar.get(), i, &v);
            bool drop = false;
            for (size_t k = 1; k < pos.size(); ++k) drop |= std::strcmp(pos[k], v.label) == 0;
            if (drop) ++dropped;
            else keep.emplace_back(v.label);
        }
        if (dropped != pos.size() - 1) {
            std::fputs("graft archive drop: no such version (or a label given twice)\n", stderr);
            return kExitFail;
        }
        ar.reset();
        std::vector<const char *> kp;
        for (const auto &k : keep) kp.push_back(k.c_str());
        if (graft_status st = graft_archive_prune(pos[0], kp.data(), kp.size(), level, log_to_stderr, nullptr);
            st != GRAFT_OK)
            return fail("archive drop", st);
        return kExitOk;
    }
    return bad_args();
}

int cmd_info(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return fail_errno(path);
    graft_reader rd{f, file_read};
    graft_patch_info info;
    const graft_status st = graft_patch_read_info(&rd, &info);
    std::fclose(f);
    if (st != GRAFT_OK) return fail(path, st);
    std::printf("format:      gft v%u\n", info.format_version);
    std::printf("source:      %llu bytes  ", static_cast<unsigned long long>(info.source_size));
    hex(stdout, info.source_hash, GRAFT_HASH_SIZE);
    std::printf("\ntarget:      %llu bytes  ", static_cast<unsigned long long>(info.target_size));
    hex(stdout, info.target_hash, GRAFT_HASH_SIZE);
    std::printf("\nmax stream:  %u bytes\n", info.max_stream);
    if (info.filter)
        std::printf("filter:      %s\n", info.filter < sizeof kFilterNames / sizeof *kFilterNames
                                              ? kFilterNames[info.filter] : "?");
    if (info.mask_count) std::printf("masks:       %u (source and target hashes are of the masked forms)\n", info.mask_count);
    return kExitOk;
}

struct HasherDeleter {
    void operator()(graft_hasher *h) const { graft_hasher_free(h); }
};

int hash_one(const char *path) {
    const bool is_stdin = std::strcmp(path, "-") == 0;
    std::FILE *f = is_stdin ? stdin : std::fopen(path, "rb");
    if (!f) return fail_errno(path);
    graft_hasher *raw = nullptr;
    if (graft_hasher_new(&raw) != GRAFT_OK) {
        if (!is_stdin) std::fclose(f);
        return fail(path, GRAFT_E_NOMEM);
    }
    std::unique_ptr<graft_hasher, HasherDeleter> h(raw);
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) graft_hasher_update(h.get(), buf.data(), n);
    const bool failed = std::ferror(f) != 0;
    if (!is_stdin) std::fclose(f);
    if (failed) return fail(path, GRAFT_E_IO);
    uint8_t digest[GRAFT_HASH_SIZE];
    graft_hasher_final(h.get(), digest);
    hex(stdout, digest, sizeof digest);
    std::printf("  %s\n", path);
    return kExitOk;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return kExitUsage;
    }
    const std::string_view cmd = argv[1];
    const int nargs = argc - 2;
    char **args = argv + 2;

    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        usage(stdout);
        return kExitOk;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("graft %s (%s)\n", graft_version(), graft_dependency_versions());
        return kExitOk;
    }
    if (cmd == "diff") return cmd_diff(nargs, args);
    if (cmd == "apply" && nargs == 3) return run_apply(args[0], args[1], args[2]);
    if (cmd == "verify" && nargs == 2) return run_apply(args[0], args[1], nullptr);
    if (cmd == "info" && nargs == 1) return cmd_info(args[0]);
    if (cmd == "pack") return cmd_pack(nargs, args);
    if (cmd == "archive") return cmd_archive(nargs, args);
    if (cmd == "update") return cmd_update(nargs, args);
    if (cmd == "list" && nargs == 1) {
        if (graft_status st = graft_package_describe(args[0], log_to_stderr, nullptr); st != GRAFT_OK)
            return fail(args[0], st);
        return kExitOk;
    }
    if (cmd == "hash" && nargs >= 1) {
        int rc = kExitOk;
        for (int i = 0; i < nargs; ++i)
            if (hash_one(args[i]) != kExitOk) rc = kExitFail;
        return rc;
    }
    if (cmd == "apply" || cmd == "verify" || cmd == "info" || cmd == "hash" || cmd == "list") {
        std::fprintf(stderr, "graft %s: wrong number of arguments\n", argv[1]);
        usage(stderr);
        return kExitUsage;
    }
    std::fprintf(stderr, "graft: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return kExitUsage;
}
