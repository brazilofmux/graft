// format.h: .gft v1 constants and primitive encodings (see docs/FORMAT.md).
#pragma once

#include "graft/graft.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace graft {

inline constexpr uint8_t kMagic[8] = {0x89, 'G', 'F', 'T', '\r', '\n', 0x1a, '\n'};
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr size_t kHeaderSize = 96;
inline constexpr uint8_t kTagEnd = 0x00;
inline constexpr uint8_t kTagBlock = 0x01;
inline constexpr size_t kMaxVarint = 10;
inline constexpr size_t kMaxTriple = 3 * kMaxVarint;

// Fuzz builds skip hash comparisons (the fuzzer can't forge BLAKE3), so it
// can reach the block decoder. Never defined in a production build.
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
inline constexpr bool kCheckHashes = false;
#else
inline constexpr bool kCheckHashes = true;
#endif

struct Header {
    uint16_t version = kFormatVersion;
    uint16_t flags = 0;
    uint64_t source_size = 0;
    uint64_t target_size = 0;
    uint8_t source_hash[GRAFT_HASH_SIZE] = {};
    uint8_t target_hash[GRAFT_HASH_SIZE] = {};
    uint32_t max_stream = 0;
};

// --- little-endian fixed width ---------------------------------------------

template <class T>
inline void put_le(uint8_t *p, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

template <class T>
inline T get_le(const uint8_t *p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v = static_cast<T>(v | static_cast<T>(T{p[i]} << (8 * i)));
    return v;
}

inline void encode_header(const Header &h, uint8_t out[kHeaderSize]) {
    std::memcpy(out, kMagic, 8);
    put_le<uint16_t>(out + 8, h.version);
    put_le<uint16_t>(out + 10, h.flags);
    put_le<uint64_t>(out + 12, h.source_size);
    put_le<uint64_t>(out + 20, h.target_size);
    std::memcpy(out + 28, h.source_hash, GRAFT_HASH_SIZE);
    std::memcpy(out + 60, h.target_hash, GRAFT_HASH_SIZE);
    put_le<uint32_t>(out + 92, h.max_stream);
}

inline graft_status decode_header(const uint8_t in[kHeaderSize], Header &h) {
    if (std::memcmp(in, kMagic, 8) != 0) return GRAFT_E_CORRUPT;
    h.version = get_le<uint16_t>(in + 8);
    h.flags = get_le<uint16_t>(in + 10);
    h.source_size = get_le<uint64_t>(in + 12);
    h.target_size = get_le<uint64_t>(in + 20);
    std::memcpy(h.source_hash, in + 28, GRAFT_HASH_SIZE);
    std::memcpy(h.target_hash, in + 60, GRAFT_HASH_SIZE);
    h.max_stream = get_le<uint32_t>(in + 92);
    if (h.version != kFormatVersion || h.flags != 0) return GRAFT_E_UNSUPPORTED;
    return GRAFT_OK;
}

// --- varints -----------------------------------------------------------------

inline void put_uvarint(std::vector<uint8_t> &out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

inline uint64_t zigzag(int64_t v) {
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

inline int64_t unzigzag(uint64_t v) {
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

// Accepts a uvarint byte by byte. feed() returns true once complete.
// After a complete value, ok() reports whether it was well formed.
class VarintReader {
public:
    bool feed(uint8_t b) {
        if (shift_ == 63 && b > 1) bad_ = true;  // bits beyond 64
        value_ |= static_cast<uint64_t>(b & 0x7f) << shift_;
        if (!(b & 0x80)) return true;
        shift_ += 7;
        if (shift_ > 63) {
            bad_ = true;
            return true;
        }
        return false;
    }
    bool ok() const { return !bad_; }
    uint64_t value() const { return value_; }

private:
    uint64_t value_ = 0;
    unsigned shift_ = 0;
    bool bad_ = false;
};

// Decodes a uvarint from [p, end). Advances p on success.
inline bool get_uvarint(const uint8_t *&p, const uint8_t *end, uint64_t &v) {
    VarintReader r;
    while (p < end) {
        if (r.feed(*p++)) {
            v = r.value();
            return r.ok();
        }
    }
    return false;
}

}  // namespace graft
