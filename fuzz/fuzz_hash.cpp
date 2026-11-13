// Differential: streaming the input in fuzzer-chosen chunk sizes must give
// the same digest as one-shot hashing.
#include "graft/graft.h"

#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const size_t chunk = size_t{data[0]} + 1;  // 1..256
    ++data;
    --size;

    uint8_t want[GRAFT_HASH_SIZE];
    if (graft_hash(data, size, want) != GRAFT_OK) std::abort();

    graft_hasher *h = nullptr;
    if (graft_hasher_new(&h) != GRAFT_OK) std::abort();
    for (size_t off = 0; off < size; off += chunk) {
        const size_t n = size - off < chunk ? size - off : chunk;
        if (graft_hasher_update(h, data + off, n) != GRAFT_OK) std::abort();
    }
    uint8_t got[GRAFT_HASH_SIZE];
    if (graft_hasher_final(h, got) != GRAFT_OK) std::abort();
    graft_hasher_free(h);

    if (std::memcmp(want, got, sizeof want) != 0) std::abort();
    return 0;
}
