// Encoder correctness: diff(old, new) then apply must reproduce new exactly.
//
// Input: [split:u16 LE][block:u8][old + new]. The block-size byte forces
// small blocks often, so triple splitting at block boundaries gets exercised.
#include "graft/graft.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Vec {
    std::vector<uint8_t> v;
};

graft_status vec_write(void *ctx, const void *p, size_t n) {
    auto *b = static_cast<const uint8_t *>(p);
    static_cast<Vec *>(ctx)->v.insert(static_cast<Vec *>(ctx)->v.end(), b, b + n);
    return GRAFT_OK;
}

struct Reader {
    const uint8_t *p;
    size_t n;
};

graft_status rd(void *ctx, void *buf, size_t cap, size_t *got) {
    auto *r = static_cast<Reader *>(ctx);
    const size_t k = std::min(cap, r->n);
    std::memcpy(buf, r->p, k);
    r->p += k;
    r->n -= k;
    *got = k;
    return GRAFT_OK;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return 0;
    size_t split = size_t{data[0]} | size_t{data[1]} << 8;
    const uint8_t bsel = data[2];
    data += 3;
    size -= 3;
    split = std::min(split, size);
    const uint8_t *oldp = data, *newp = data + split;
    const size_t oldn = split, newn = size - split;

    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 1;
    o.block_size = bsel < 128 ? GRAFT_MIN_BLOCK_SIZE + bsel : GRAFT_DEFAULT_BLOCK_SIZE;

    Vec patch;
    graft_sink ps{&patch, vec_write};
    if (graft_diff(oldp, oldn, newp, newn, &o, &ps) != GRAFT_OK) std::abort();

    graft_source src = graft_source_from_memory(oldp, oldn);
    Reader r{patch.v.data(), patch.v.size()};
    graft_reader reader{&r, rd};
    Vec out;
    graft_sink os{&out, vec_write};
    if (graft_apply(&src, &reader, &os, nullptr) != GRAFT_OK) std::abort();
    if (out.v.size() != newn || (newn && std::memcmp(out.v.data(), newp, newn) != 0)) std::abort();
    return 0;
}
