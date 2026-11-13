#include "graft/graft.h"

#include <blake3.h>

#include <new>

struct graft_hasher {
    blake3_hasher state;
};

static_assert(GRAFT_HASH_SIZE == BLAKE3_OUT_LEN);

extern "C" graft_status graft_hash(const void *data, size_t len, uint8_t out[GRAFT_HASH_SIZE]) {
    if (!out || (!data && len != 0)) return GRAFT_E_INVALID_ARG;
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (len) blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out, GRAFT_HASH_SIZE);
    return GRAFT_OK;
}

extern "C" graft_status graft_hasher_new(graft_hasher **out) {
    if (!out) return GRAFT_E_INVALID_ARG;
    *out = nullptr;
    auto *h = new (std::nothrow) graft_hasher;
    if (!h) return GRAFT_E_NOMEM;
    blake3_hasher_init(&h->state);
    *out = h;
    return GRAFT_OK;
}

extern "C" graft_status graft_hasher_update(graft_hasher *h, const void *data, size_t len) {
    if (!h || (!data && len != 0)) return GRAFT_E_INVALID_ARG;
    if (len) blake3_hasher_update(&h->state, data, len);
    return GRAFT_OK;
}

extern "C" graft_status graft_hasher_final(const graft_hasher *h, uint8_t out[GRAFT_HASH_SIZE]) {
    if (!h || !out) return GRAFT_E_INVALID_ARG;
    blake3_hasher_finalize(&h->state, out, GRAFT_HASH_SIZE);
    return GRAFT_OK;
}

extern "C" void graft_hasher_free(graft_hasher *h) { delete h; }
