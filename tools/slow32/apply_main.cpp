// graft-apply: a minimal patch applier, the part of graft that has to run on
// the machine being updated. Built for SLOW-32 by tools/slow32/build.sh (and
// buildable for any host): it exercises libgraft's decoder on a 32-bit target
// with a different compiler and C library, and gives a way to apply an
// untrusted patch inside the SLOW-32 sandbox.
//
//   graft-apply OLD PATCH NEW     apply; NEW is written only if all checks pass
//   graft-apply -v OLD PATCH      verify only
#include "graft/graft.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool read_file(const char *path, std::vector<unsigned char> &out) {
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

graft_status file_read(void *ctx, void *buf, size_t cap, size_t *got) {
    auto *f = static_cast<std::FILE *>(ctx);
    *got = std::fread(buf, 1, cap, f);
    return std::ferror(f) ? GRAFT_E_IO : GRAFT_OK;
}

graft_status file_write(void *ctx, const void *buf, size_t len) {
    return std::fwrite(buf, 1, len, static_cast<std::FILE *>(ctx)) == len ? GRAFT_OK : GRAFT_E_IO;
}

graft_status discard(void *, const void *, size_t) { return GRAFT_OK; }

}  // namespace

int main(int argc, char **argv) {
    const bool verify = argc == 4 && std::strcmp(argv[1], "-v") == 0;
    if (!verify && argc != 4) {
        std::fputs("usage: graft-apply OLD PATCH NEW | graft-apply -v OLD PATCH\n", stderr);
        return 2;
    }
    const char *old_path = verify ? argv[2] : argv[1];
    const char *patch_path = verify ? argv[3] : argv[2];

    std::vector<unsigned char> old;
    if (!read_file(old_path, old)) {
        std::fprintf(stderr, "graft-apply: can't read %s\n", old_path);
        return 1;
    }
    std::FILE *pf = std::fopen(patch_path, "rb");
    if (!pf) {
        std::fprintf(stderr, "graft-apply: can't read %s\n", patch_path);
        return 1;
    }
    graft_source src = graft_source_from_memory(old.data(), old.size());
    graft_reader rd{pf, file_read};

    std::string tmp;
    std::FILE *out = nullptr;
    graft_sink sink{nullptr, discard};
    if (!verify) {
        tmp = std::string(argv[3]) + ".graft-tmp";
        out = std::fopen(tmp.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "graft-apply: can't create %s\n", tmp.c_str());
            std::fclose(pf);
            return 1;
        }
        sink = {out, file_write};
    }
    graft_status st = graft_apply(&src, &rd, &sink, nullptr);
    std::fclose(pf);
    if (out && std::fclose(out) != 0 && st == GRAFT_OK) st = GRAFT_E_IO;
    if (st != GRAFT_OK) {
        if (out) std::remove(tmp.c_str());
        std::fprintf(stderr, "graft-apply: %s\n", graft_status_string(st));
        return 1;
    }
    if (out && std::rename(tmp.c_str(), argv[3]) != 0) {
        std::remove(tmp.c_str());
        std::fprintf(stderr, "graft-apply: can't write %s\n", argv[3]);
        return 1;
    }
    std::puts(verify ? "ok" : "applied");
    return 0;
}
