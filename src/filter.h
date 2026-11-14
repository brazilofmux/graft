// filter.h: reversible executable filters.
//
// A filter rewrites branch/call targets so code that moved still looks the
// same to the delta encoder. Each is a same-length transform that's exactly
// invertible on *any* input: each decision looks only at opcode bytes, and a
// rewrite changes only bytes after its own offset, which the scan then skips.
// No decision can see a byte another decision changed, so encoding and
// decoding decide identically.
#pragma once

#include "graft/graft.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace graft {

inline bool filter_known(uint8_t id) { return id >= GRAFT_FILTER_X86 && id <= GRAFT_FILTER_SLOW32; }

// Whole-buffer transform (offsets are positions within the buffer).
void filter_encode(uint8_t id, uint8_t *buf, size_t n);
void filter_decode(uint8_t id, uint8_t *buf, size_t n);

// Streaming inverse: push filtered bytes in order, receive original bytes.
// Holds back at most a few bytes until it can decide about them.
class FilterDecoder {
public:
    explicit FilterDecoder(uint8_t id) : id_(id) {}
    // Appends any bytes that are final to `out`.
    void push(const uint8_t *p, size_t n, std::vector<uint8_t> &out);
    void finish(std::vector<uint8_t> &out);

private:
    void run(bool final, std::vector<uint8_t> &out);
    uint8_t id_;
    std::vector<uint8_t> buf_;  // bytes from absolute offset base_
    uint64_t base_ = 0;
    uint64_t next_ = 0;         // absolute offset of the next decision
};

// Guesses a filter from a file's header: PE, ELF, Mach-O, DOS MZ, S32X.
uint8_t filter_detect(const uint8_t *p, size_t n);

}  // namespace graft
