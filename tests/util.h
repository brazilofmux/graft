// util.h: in-memory readers/sinks and patch helpers for tests.
#pragma once

#include "graft/graft.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace testutil {

using Bytes = std::vector<uint8_t>;

struct VecSink {
    Bytes data;
    graft_sink sink() {
        return {this, [](void *ctx, const void *p, size_t n) {
                    auto *self = static_cast<VecSink *>(ctx);
                    const auto *b = static_cast<const uint8_t *>(p);
                    self->data.insert(self->data.end(), b, b + n);
                    return GRAFT_OK;
                }};
    }
};

// Serves the input in chunks of `chunk` bytes to exercise buffering.
struct MemReader {
    const Bytes *data;
    size_t pos = 0;
    size_t chunk = SIZE_MAX;
    graft_reader reader() {
        return {this, [](void *ctx, void *buf, size_t cap, size_t *got) {
                    auto *self = static_cast<MemReader *>(ctx);
                    const size_t n = std::min({cap, self->chunk, self->data->size() - self->pos});
                    if (n) std::memcpy(buf, self->data->data() + self->pos, n);
                    self->pos += n;
                    *got = n;
                    return GRAFT_OK;
                }};
    }
};

inline graft_status make_patch(const Bytes &oldb, const Bytes &newb, Bytes &patch, uint32_t block_size = 0,
                               int level = 3, uint8_t method = GRAFT_METHOD_AUTO) {
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = level;
    o.method = method;
    if (block_size) o.block_size = block_size;
    VecSink s;
    graft_sink sink = s.sink();
    const graft_status st = graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &sink);
    patch = std::move(s.data);
    return st;
}

inline graft_status apply_patch(const Bytes &oldb, const Bytes &patch, Bytes &out, size_t chunk = SIZE_MAX,
                                const graft_apply_limits *lim = nullptr) {
    graft_source src = graft_source_from_memory(oldb.data(), oldb.size());
    MemReader r{&patch, 0, chunk};
    graft_reader rd = r.reader();
    VecSink s;
    graft_sink sink = s.sink();
    const graft_status st = graft_apply(&src, &rd, &sink, lim);
    out = std::move(s.data);
    return st;
}

inline Bytes random_bytes(std::mt19937_64 &rng, size_t n) {
    Bytes b(n);
    for (auto &x : b) x = static_cast<uint8_t>(rng());
    return b;
}

}  // namespace testutil
