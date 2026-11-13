#include "check.h"
#include "graft/graft.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

std::string hex(const uint8_t *d, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}

}  // namespace

TEST(hash_empty_known_vector) {
    uint8_t out[GRAFT_HASH_SIZE];
    CHECK_EQ(graft_hash(nullptr, 0, out), GRAFT_OK);
    CHECK_EQ(hex(out, sizeof out), std::string("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));
}

TEST(hash_abc_known_vector) {
    uint8_t out[GRAFT_HASH_SIZE];
    CHECK_EQ(graft_hash("abc", 3, out), GRAFT_OK);
    CHECK_EQ(hex(out, sizeof out), std::string("6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"));
}

TEST(hash_streaming_matches_oneshot) {
    std::vector<uint8_t> data(100000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 131u + (i >> 7));

    uint8_t want[GRAFT_HASH_SIZE];
    CHECK_EQ(graft_hash(data.data(), data.size(), want), GRAFT_OK);

    for (size_t chunk : {size_t{1}, size_t{63}, size_t{1024}, size_t{65537}}) {
        graft_hasher *h = nullptr;
        CHECK_EQ(graft_hasher_new(&h), GRAFT_OK);
        for (size_t off = 0; off < data.size(); off += chunk) {
            const size_t n = std::min(chunk, data.size() - off);
            CHECK_EQ(graft_hasher_update(h, data.data() + off, n), GRAFT_OK);
        }
        uint8_t got[GRAFT_HASH_SIZE];
        CHECK_EQ(graft_hasher_final(h, got), GRAFT_OK);
        CHECK(std::memcmp(got, want, sizeof got) == 0);
        graft_hasher_free(h);
    }
}

TEST(hash_rejects_bad_args) {
    uint8_t out[GRAFT_HASH_SIZE];
    CHECK_EQ(graft_hash(nullptr, 1, out), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_hash("x", 1, nullptr), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_hasher_new(nullptr), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_hasher_update(nullptr, "x", 1), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_hasher_final(nullptr, out), GRAFT_E_INVALID_ARG);
    graft_hasher_free(nullptr);  // no-op
}
