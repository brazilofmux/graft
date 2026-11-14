// graft: command-line front end to libgraft.

#include "graft/graft.h"
#include "masks.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        "  diff [-l LEVEL] [-b BLOCK] [--mask SPEC]... [--mask-file FILE] OLD NEW PATCH\n"
        "                   write a patch turning OLD into NEW. Masked regions\n"
        "                   keep the installed file's bytes (serials, keys):\n"
        "                     at=OFF:LEN | old=OFF:LEN,new=OFF:LEN |\n"
        "                     marker=TEXT|hex:HEX,len=N[,skip=K]\n"
        "                     [,newlen=N,pad=BYTE|truncate]\n"
        "  apply OLD PATCH NEW\n"
        "                   apply PATCH to OLD, writing NEW\n"
        "  verify OLD PATCH check that PATCH applies cleanly to OLD\n"
        "  info PATCH       print a patch's header\n"
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
    if (cmd == "hash" && nargs >= 1) {
        int rc = kExitOk;
        for (int i = 0; i < nargs; ++i)
            if (hash_one(args[i]) != kExitOk) rc = kExitFail;
        return rc;
    }
    if (cmd == "apply" || cmd == "verify" || cmd == "info" || cmd == "hash") {
        std::fprintf(stderr, "graft %s: wrong number of arguments\n", argv[1]);
        usage(stderr);
        return kExitUsage;
    }
    std::fprintf(stderr, "graft: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return kExitUsage;
}
