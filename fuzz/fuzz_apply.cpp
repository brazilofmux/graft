// The decoder against arbitrary patches.
//
// Input: [len:u16 LE][source: len bytes][patch: rest]. Fuzz builds define
// FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION, which skips hash comparisons so
// mutated patches reach the block decoder. Seeds come from fuzz_mkseeds.
#include "graft/graft.h"

#include <algorithm>
#include <cstring>

namespace {

struct Reader {
    const uint8_t *p;
    size_t n;
};

graft_status rd(void *ctx, void *buf, size_t cap, size_t *got) {
    auto *r = static_cast<Reader *>(ctx);
    // Odd chunk sizes exercise the decoder's buffering.
    const size_t k = std::min({cap, r->n, size_t{4093}});
    std::memcpy(buf, r->p, k);
    r->p += k;
    r->n -= k;
    *got = k;
    return GRAFT_OK;
}

graft_status sink(void *, const void *, size_t) { return GRAFT_OK; }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    size_t slen = size_t{data[0]} | size_t{data[1]} << 8;
    data += 2;
    size -= 2;
    slen = std::min(slen, size);

    graft_source src = graft_source_from_memory(data, slen);
    Reader r{data + slen, size - slen};
    graft_reader reader{&r, rd};
    graft_sink out{nullptr, sink};
    graft_apply_limits lim;
    graft_apply_limits_init(&lim);
    lim.max_stream = 1 << 20;
    lim.max_target_size = 4 << 20;
    graft_apply(&src, &reader, &out, &lim);

    Reader r2{data + slen, size - slen};
    graft_reader reader2{&r2, rd};
    graft_patch_info info;
    graft_patch_read_info(&reader2, &info);
    return 0;
}
