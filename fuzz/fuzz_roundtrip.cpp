// Encoder correctness: diff(old, new) then apply must reproduce new exactly.
//
// Input: [split:u16 LE][block:u8][mask:7 bytes][old + new]. The block-size
// byte forces small blocks often, so triple splitting at block boundaries
// gets exercised. The mask bytes describe one masked region (mode byte >= 3:
// none); with a mask, old itself is the "installed" file, so the output must
// be new with old's region bytes stamped in.
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
    if (size < 10) return 0;
    size_t split = size_t{data[0]} | size_t{data[1]} << 8;
    const uint8_t bsel = data[2];
    const uint8_t *mk = data + 3;
    data += 10;
    size -= 10;
    split = std::min(split, size);
    const uint8_t *oldp = data, *newp = data + split;
    const size_t oldn = split, newn = size - split;

    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 1;
    o.block_size = bsel < 128 ? GRAFT_MIN_BLOCK_SIZE + bsel : GRAFT_DEFAULT_BLOCK_SIZE;

    graft_mask mask{size_t{mk[0]} | size_t{mk[1]} << 8, mk[2], size_t{mk[3]} | size_t{mk[4]} << 8, mk[5],
                    static_cast<uint8_t>(mk[6] & 3), static_cast<uint8_t>(mk[6] >> 2)};
    const bool masked = mask.mode < 3;
    if (masked) {
        o.masks = &mask;
        o.mask_count = 1;
    }

    Vec patch;
    graft_sink ps{&patch, vec_write};
    const graft_status st = graft_diff(oldp, oldn, newp, newn, &o, &ps);
    if (masked && st == GRAFT_E_INVALID_ARG) return 0;  // the mask didn't fit these inputs
    if (st != GRAFT_OK) std::abort();

    std::vector<uint8_t> want(newp, newp + newn);
    if (masked) {
        for (uint64_t i = 0; i < mask.new_length; ++i)
            want[mask.new_offset + i] = i < mask.old_length ? oldp[mask.old_offset + i] : mask.pad;
    }

    graft_source src = graft_source_from_memory(oldp, oldn);
    Reader r{patch.v.data(), patch.v.size()};
    graft_reader reader{&r, rd};
    Vec out;
    graft_sink os{&out, vec_write};
    if (graft_apply(&src, &reader, &os, nullptr) != GRAFT_OK) std::abort();
    if (out.v != want) std::abort();
    return 0;
}
