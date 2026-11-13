// graft: command-line front end to libgraft.
//
// M0 commands: version, hash. diff/apply arrive in M1.

#include "graft/graft.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

void usage(std::FILE *out) {
    std::fputs(
        "usage: graft <command> [args]\n"
        "\n"
        "commands:\n"
        "  version          print graft and dependency versions\n"
        "  hash FILE...     print the BLAKE3-256 of each FILE ('-' = stdin)\n"
        "  help             show this message\n",
        out);
}

struct HasherDeleter {
    void operator()(graft_hasher *h) const { graft_hasher_free(h); }
};
using HasherPtr = std::unique_ptr<graft_hasher, HasherDeleter>;

int hash_one(const char *path) {
    const bool is_stdin = std::strcmp(path, "-") == 0;
    std::FILE *f = is_stdin ? stdin : std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "graft: %s: %s\n", path, std::strerror(errno));
        return kExitFail;
    }

    graft_hasher *raw = nullptr;
    if (graft_hasher_new(&raw) != GRAFT_OK) {
        if (!is_stdin) std::fclose(f);
        std::fprintf(stderr, "graft: out of memory\n");
        return kExitFail;
    }
    HasherPtr h(raw);

    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) graft_hasher_update(h.get(), buf.data(), n);
    const bool failed = std::ferror(f) != 0;
    if (!is_stdin) std::fclose(f);
    if (failed) {
        std::fprintf(stderr, "graft: %s: read error\n", path);
        return kExitFail;
    }

    uint8_t digest[GRAFT_HASH_SIZE];
    graft_hasher_final(h.get(), digest);
    for (uint8_t b : digest) std::printf("%02x", b);
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

    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        usage(stdout);
        return kExitOk;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("graft %s (%s)\n", graft_version(), graft_dependency_versions());
        return kExitOk;
    }
    if (cmd == "hash") {
        if (argc < 3) {
            std::fputs("usage: graft hash FILE...\n", stderr);
            return kExitUsage;
        }
        int rc = kExitOk;
        for (int i = 2; i < argc; ++i)
            if (hash_one(argv[i]) != kExitOk) rc = kExitFail;
        return rc;
    }

    std::fprintf(stderr, "graft: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return kExitUsage;
}
