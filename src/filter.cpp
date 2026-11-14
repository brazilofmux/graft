#include "filter.h"

#include <algorithm>
#include <cstring>

namespace graft {
namespace {

uint32_t le32(const uint8_t *p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}
void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// One decision at absolute offset `pos`, with `avail` bytes at p. Returns how
// far to advance, or 0 if it needs more bytes to decide (never when final:
// then a short instruction at the end is left alone, as the encoder, which
// saw the whole buffer, also left it).
using Step = size_t (*)(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final);

#define NEED(k)                                    \
    do {                                           \
        if (avail < (k)) return final ? 1 : 0;     \
    } while (0)

// x86 rel32: every displacement after an E8/E9/0F 8x opcode, mod 2^32.
// (xz's BCJ rewrites only displacements whose top byte looks like a code
// distance, but that byte can be changed by a nearby later rewrite, which
// takes extra state to undo. Deciding on the opcode bytes alone, which no
// rewrite ever touches, makes the inverse exact by construction.)
void rel32(uint8_t *d, uint64_t next, bool enc) {
    const uint32_t v = le32(d), p = static_cast<uint32_t>(next);
    put32(d, enc ? v + p : v - p);
}

size_t step_x86(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final) {
    NEED(1);
    if (p[0] == 0xe8 || p[0] == 0xe9) {
        NEED(5);
        rel32(p + 1, pos + 5, enc);
        return 5;
    }
    if (p[0] == 0x0f) {
        NEED(2);
        if ((p[1] & 0xf0) == 0x80) {
            NEED(6);
            rel32(p + 2, pos + 6, enc);
            return 6;
        }
    }
    return 1;
}

size_t step_x86_16(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final) {
    NEED(1);
    size_t at = 0, len = 0;
    if (p[0] == 0xe8 || p[0] == 0xe9) {
        at = 1;
        len = 3;
    } else if (p[0] == 0x0f) {
        NEED(2);
        if ((p[1] & 0xf0) == 0x80) {
            at = 2;
            len = 4;
        }
    }
    if (!len) return 1;
    NEED(len);
    const uint16_t v = le16(p + at), next = static_cast<uint16_t>(pos + len);
    put16(p + at, static_cast<uint16_t>(enc ? v + next : v - next));
    return len;
}

// Length of an unprefixed 8080/Z80 instruction from its opcode.
size_t z80_base_len(uint8_t op) {
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31:                        // LD rr,nn
    case 0x22: case 0x2a: case 0x32: case 0x3a:                        // LD (nn),HL etc.
    case 0xc2: case 0xc3: case 0xc4: case 0xca: case 0xcc: case 0xcd:  // JP / CALL
    case 0xd2: case 0xd4: case 0xda: case 0xdc: case 0xe2: case 0xe4:
    case 0xea: case 0xec: case 0xf2: case 0xf4: case 0xfa: case 0xfc:
        return 3;
    case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x36: case 0x3e:  // LD r,n
    case 0xc6: case 0xce: case 0xd6: case 0xde: case 0xe6: case 0xee: case 0xf6: case 0xfe:  // ALU n
    case 0xd3: case 0xdb:                                              // OUT/IN (n)
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:  // DJNZ / JR
        return 2;
    default: return 1;
    }
}

// Instructions that address (HL), which take a displacement under DD/FD.
bool z80_uses_hl_mem(uint8_t op) {
    return op == 0x34 || op == 0x35 || op == 0x36 || (op >= 0x70 && op <= 0x77 && op != 0x76) ||
           ((op & 0xc7) == 0x46 && op != 0x76) || ((op & 0xc7) == 0x86);
}

// Z80/8080 code addresses everything absolutely; make CALL/JP targets and
// direct data addresses (LD rr,nn; LD (nn),HL; LD HL,(nn); LD (nn),A;
// LD A,(nn)) relative to the next instruction, so code and data that moved
// together look unchanged. (On CP/M 2.2's BDOS with a 3-byte fix inserted,
// this cut the patch by a third against bsdiff; relativizing only CALL/JP
// gave 8%.) Walks instruction boundaries (an opcode-length table) so operand and
// immediate bytes are never mistaken for CALL/JP opcodes. Lengths depend
// only on opcode bytes and only CALL/JP operands are rewritten, so the
// walk is identical in both directions.
size_t step_z80(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final) {
    NEED(1);
    const uint8_t op = p[0];
    if (op == 0xcb) return 2;
    if (op == 0xed) {
        NEED(2);
        return (p[1] & 0xc7) == 0x43 ? 4 : 2;  // ED 43/4B/53/5B/63/6B/73/7B nn
    }
    if (op == 0xdd || op == 0xfd) {
        NEED(2);
        const uint8_t nx = p[1];
        if (nx == 0xcb) return 4;                          // DD CB d op
        if (nx == 0xdd || nx == 0xfd || nx == 0xed) return 1;  // a lone prefix
        return 1 + z80_base_len(nx) + (z80_uses_hl_mem(nx) ? 1 : 0);
    }
    const bool call = op == 0xcd || (op & 0xc7) == 0xc4;  // CALL nn, CALL cc,nn
    const bool jp = op == 0xc3 || (op & 0xc7) == 0xc2;    // JP nn, JP cc,nn
    const bool addr = (op & 0xcf) == 0x01 || op == 0x22 || op == 0x2a || op == 0x32 || op == 0x3a;
    if (!call && !jp && !addr) return z80_base_len(op);
    NEED(3);
    const uint16_t v = le16(p + 1), next = static_cast<uint16_t>(pos + 3);
    put16(p + 1, static_cast<uint16_t>(enc ? v - next : v + next));
    return 3;
}

// ARM64 B/BL: imm26 words, relative -> absolute (in words).
size_t step_arm64(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final) {
    NEED(4);
    const uint32_t w = le32(p);
    if ((w & 0x7c000000) == 0x14000000) {
        const uint32_t pc = static_cast<uint32_t>(pos >> 2);
        const uint32_t imm = w & 0x03ffffff;
        put32(p, (w & 0xfc000000) | ((enc ? imm + pc : imm - pc) & 0x03ffffff));
    }
    return 4;
}

// SLOW-32 JAL (opcode 0x40, J-type, offset from the JAL itself).
size_t step_slow32(uint8_t *p, size_t avail, uint64_t pos, bool enc, bool final) {
    NEED(4);
    const uint32_t w = le32(p);
    if ((w & 0x7f) == 0x40) {
        const uint32_t imm = ((w >> 31) & 1) << 20 | ((w >> 21) & 0x3ff) << 1 | ((w >> 20) & 1) << 11 |
                             ((w >> 12) & 0xff) << 12;
        const uint32_t pc = static_cast<uint32_t>(pos);
        const uint32_t r = (enc ? imm + pc : imm - pc) & 0x1ffffe;
        const uint32_t hi = ((r >> 20) & 1) << 31 | ((r >> 1) & 0x3ff) << 21 | ((r >> 11) & 1) << 20 |
                            ((r >> 12) & 0xff) << 12;
        put32(p, hi | (w & 0xfff));
    }
    return 4;
}

#undef NEED

Step step_for(uint8_t id) {
    switch (id) {
    case GRAFT_FILTER_X86: return step_x86;
    case GRAFT_FILTER_X86_16: return step_x86_16;
    case GRAFT_FILTER_Z80: return step_z80;
    case GRAFT_FILTER_ARM64: return step_arm64;
    case GRAFT_FILTER_SLOW32: return step_slow32;
    default: return nullptr;
    }
}

void whole(uint8_t id, uint8_t *buf, size_t n, bool enc) {
    const Step step = step_for(id);
    if (!step) return;
    for (size_t i = 0; i < n;) i += step(buf + i, n - i, i, enc, true);
}

}  // namespace

void filter_encode(uint8_t id, uint8_t *buf, size_t n) { whole(id, buf, n, true); }
void filter_decode(uint8_t id, uint8_t *buf, size_t n) { whole(id, buf, n, false); }

void FilterDecoder::run(bool final, std::vector<uint8_t> &out) {
    const Step step = step_for(id_);
    const uint64_t end = base_ + buf_.size();
    while (next_ < end) {
        const size_t off = static_cast<size_t>(next_ - base_);
        const size_t adv = step(buf_.data() + off, buf_.size() - off, next_, false, final);
        if (adv == 0) break;
        next_ += adv;
    }
    // Bytes before the next decision are final (decisions only rewrite bytes
    // after their own offset, and those were skipped over).
    const size_t done = static_cast<size_t>(std::min(next_, end) - base_);
    out.insert(out.end(), buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(done));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(done));
    base_ += done;
}

void FilterDecoder::push(const uint8_t *p, size_t n, std::vector<uint8_t> &out) {
    buf_.insert(buf_.end(), p, p + n);
    run(false, out);
}

void FilterDecoder::finish(std::vector<uint8_t> &out) { run(true, out); }

// AUTO tries only filters measured to help this encoder. The rel->abs
// filters (x86, x86-16, ARM64, SLOW-32) are BCJ-style: they help
// compressors, but on real ARM64 and x86-64 builds they made bsdiff-class
// patches 12-36% larger, because relative branches in moved code already
// match exactly. They remain available explicitly. Formats with no
// header (CP/M .COM, raw ROMs) can't be detected; pass GRAFT_FILTER_Z80.
uint8_t filter_detect(const uint8_t *p, size_t n) {
    (void)p;
    (void)n;
    return GRAFT_FILTER_NONE;
}

}  // namespace graft
