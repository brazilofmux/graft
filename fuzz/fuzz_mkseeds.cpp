// Writes seed inputs for fuzz_apply: valid patches, so mutation starts from
// well-formed structure instead of having to discover the header.
//
//   fuzz_mkseeds OUTDIR
#include "graft/graft.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

graft_status vec_write(void *ctx, const void *p, size_t n) {
    auto *b = static_cast<const uint8_t *>(p);
    static_cast<Bytes *>(ctx)->insert(static_cast<Bytes *>(ctx)->end(), b, b + n);
    return GRAFT_OK;
}

bool seed(const std::string &dir, int i, const Bytes &oldb, const Bytes &newb, uint32_t block) {
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 1;
    o.block_size = block;
    Bytes patch;
    graft_sink s{&patch, vec_write};
    if (graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &s) != GRAFT_OK) return false;
    Bytes in{static_cast<uint8_t>(oldb.size()), static_cast<uint8_t>(oldb.size() >> 8)};
    in.insert(in.end(), oldb.begin(), oldb.end());
    in.insert(in.end(), patch.begin(), patch.end());
    const std::string path = dir + "/seed" + std::to_string(i);
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(in.data(), 1, in.size(), f) == in.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fputs("usage: fuzz_mkseeds OUTDIR\n", stderr);
        return 2;
    }
    std::mt19937 rng(1);
    auto rnd = [&](size_t n) {
        Bytes b(n);
        for (auto &x : b) x = static_cast<uint8_t>(rng() % 16);  // low entropy: more matches
        return b;
    };
    int i = 0;
    bool ok = true;
    ok &= seed(argv[1], i++, {}, {}, GRAFT_DEFAULT_BLOCK_SIZE);
    ok &= seed(argv[1], i++, {}, rnd(50), GRAFT_DEFAULT_BLOCK_SIZE);
    for (size_t n : {size_t{64}, size_t{300}, size_t{2000}}) {
        Bytes a = rnd(n), b = a;
        for (size_t k = 0; k < n / 20 + 1; ++k) b[rng() % n] ^= 0x5a;
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(n / 2), {1, 2, 3, 4});
        ok &= seed(argv[1], i++, a, b, GRAFT_DEFAULT_BLOCK_SIZE);
        ok &= seed(argv[1], i++, a, b, GRAFT_MIN_BLOCK_SIZE);
    }
    return ok ? 0 : 1;
}
