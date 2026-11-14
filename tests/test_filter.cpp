#include "check.h"
#include "filter.h"
#include "util.h"

#include <string>

using namespace testutil;

namespace {

const uint8_t kFilters[] = {GRAFT_FILTER_X86, GRAFT_FILTER_X86_16, GRAFT_FILTER_Z80, GRAFT_FILTER_ARM64,
                            GRAFT_FILTER_SLOW32};

// Bytes dense in the opcodes the filters look for, to exercise every path
// (including back-to-back and truncated instructions at the end).
Bytes opcode_soup(std::mt19937_64 &rng, size_t n) {
    static const uint8_t ops[] = {0xe8, 0xe9, 0x0f, 0x80, 0x8f, 0xcd, 0xc3, 0xc4, 0xca, 0x00, 0xff, 0x40, 0x94, 0x14};
    Bytes b(n);
    for (auto &x : b) x = rng() % 3 ? ops[rng() % sizeof ops] : static_cast<uint8_t>(rng());
    return b;
}

Bytes streamed_decode(uint8_t id, const Bytes &enc, std::mt19937_64 &rng) {
    graft::FilterDecoder d(id);
    Bytes out;
    for (size_t i = 0; i < enc.size();) {
        const size_t n = std::min<size_t>(enc.size() - i, 1 + rng() % 9);
        d.push(enc.data() + i, n, out);
        i += n;
    }
    d.finish(out);
    return out;
}

}  // namespace

TEST(filter_exactly_reversible) {
    std::mt19937_64 rng(20);
    for (uint8_t id : kFilters) {
        for (int round = 0; round < 2000; ++round) {
            const size_t n = rng() % 300;
            const Bytes orig = round % 2 ? opcode_soup(rng, n) : random_bytes(rng, n);
            Bytes enc = orig;
            graft::filter_encode(id, enc.data(), enc.size());
            Bytes dec = enc;
            graft::filter_decode(id, dec.data(), dec.size());
            if (dec != orig || streamed_decode(id, enc, rng) != orig) {
                check::fail(__FILE__, __LINE__, "filter " + std::to_string(id) + " not reversible, round " +
                                                    std::to_string(round));
                break;
            }
        }
    }
}

TEST(filter_x86_overlapping_lookahead_regression) {
    // Found by fuzz_filter: a rewrite at offset 15 changed bytes that an
    // earlier decision (offset 13) had looked at, under the old xz-style
    // "top byte" rule. Decisions now look only at opcode bytes.
    const Bytes orig{0x71, 0xe9, 0x71, 0xe9, 0xe9, 0x71, 0xe9, 0x61, 0xff, 0xe9, 0x7e, 0xe9, 0x71, 0xe9,
                     0x71, 0xe9, 0xff, 0xfe, 0xe9, 0xff, 0x6d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0,
                     0xe9, 0xff, 0xe9, 0xff, 0x6d, 0xe9};
    std::mt19937_64 rng(23);
    Bytes enc = orig;
    graft::filter_encode(GRAFT_FILTER_X86, enc.data(), enc.size());
    Bytes dec = enc;
    graft::filter_decode(GRAFT_FILTER_X86, dec.data(), dec.size());
    CHECK(dec == orig);
    CHECK(streamed_decode(GRAFT_FILTER_X86, enc, rng) == orig);
}

TEST(filter_x86_makes_calls_absolute) {
    // Two calls to the same target from different places encode identically.
    Bytes b(64, 0x90);
    const uint32_t target = 40;
    for (uint32_t at : {0u, 20u}) {
        b[at] = 0xe8;
        const uint32_t rel = target - (at + 5);
        for (uint32_t i = 0; i < 4; ++i) b[at + 1 + i] = static_cast<uint8_t>(rel >> (8 * i));
    }
    graft::filter_encode(GRAFT_FILTER_X86, b.data(), b.size());
    CHECK(std::equal(b.begin() + 1, b.begin() + 5, b.begin() + 21));
    CHECK_EQ(b[1], target);
}

TEST(filter_z80_walks_instruction_boundaries) {
    // MVI A,0CDh; LXI H,0C3C3h; CALL 1234h at offset 5. The CD and C3 bytes
    // are operands, not opcodes: only LXI's and CALL's operands change.
    Bytes b{0x3e, 0xcd, 0x21, 0xc3, 0xc3, 0xcd, 0x34, 0x12, 0x00};
    const Bytes orig = b;
    graft::filter_encode(GRAFT_FILTER_Z80, b.data(), b.size());
    CHECK_EQ(b[0], 0x3e);
    CHECK_EQ(b[1], 0xcd);  // MVI's immediate untouched
    CHECK_EQ(b[5], 0xcd);
    const uint16_t call = static_cast<uint16_t>(b[6] | b[7] << 8);
    CHECK_EQ(call, static_cast<uint16_t>(0x1234 - 8));  // relative to the next instruction
    const uint16_t lxi = static_cast<uint16_t>(b[3] | b[4] << 8);
    CHECK_EQ(lxi, static_cast<uint16_t>(0xc3c3 - 5));
    graft::filter_decode(GRAFT_FILTER_Z80, b.data(), b.size());
    CHECK(b == orig);
}

TEST(filter_auto_never_worse) {
    // Executable-looking input: AUTO must produce a patch no bigger than NONE.
    std::mt19937_64 rng(24);
    Bytes oldb = opcode_soup(rng, 30000);
    oldb[0] = 0x7f, oldb[1] = 'E', oldb[2] = 'L', oldb[3] = 'F';
    Bytes newb = oldb;
    newb.insert(newb.begin() + 1000, 7, 0x90);
    size_t sizes[2];
    for (int i = 0; i < 2; ++i) {
        graft_diff_options o;
        graft_diff_options_init(&o);
        o.level = 3;
        o.filter = i ? GRAFT_FILTER_AUTO : GRAFT_FILTER_NONE;
        VecSink s;
        graft_sink sink = s.sink();
        CHECK_EQ(graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &sink), GRAFT_OK);
        sizes[i] = s.data.size();
    }
    CHECK(sizes[1] <= sizes[0]);
}

TEST(filter_patch_roundtrip_every_filter) {
    std::mt19937_64 rng(21);
    const Bytes oldb = opcode_soup(rng, 20000);
    Bytes newb = oldb;
    newb.insert(newb.begin() + 5000, {0x90, 0x90, 0x90, 0x90, 0xe8, 1, 2});
    newb.erase(newb.begin() + 15000, newb.begin() + 15010);
    for (uint8_t id : {uint8_t{GRAFT_FILTER_NONE}, kFilters[0], kFilters[1], kFilters[2], kFilters[3], kFilters[4],
                       uint8_t{GRAFT_FILTER_AUTO}}) {
        graft_diff_options o;
        graft_diff_options_init(&o);
        o.level = 3;
        o.filter = id;
        o.block_size = 4096;  // several blocks: the streaming inverse crosses them
        VecSink s;
        graft_sink sink = s.sink();
        CHECK_EQ(graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &sink), GRAFT_OK);
        for (size_t chunk : {SIZE_MAX, size_t{1}, size_t{333}}) {
            Bytes out;
            CHECK_EQ(apply_patch(oldb, s.data, out, chunk), GRAFT_OK);
            CHECK(out == newb);
        }
        MemReader r{&s.data};
        graft_reader rd = r.reader();
        graft_patch_info info;
        CHECK_EQ(graft_patch_read_info(&rd, &info), GRAFT_OK);
        if (id != GRAFT_FILTER_AUTO) CHECK_EQ(info.filter, id);
    }
}

TEST(filter_with_masks) {
    std::mt19937_64 rng(22);
    const Bytes oldb = opcode_soup(rng, 8000);
    Bytes newb = oldb;
    newb.insert(newb.begin() + 100, {0xe8, 0, 0, 0, 0});
    Bytes installed = oldb;
    for (size_t i = 0; i < 8; ++i) installed[4000 + i] = static_cast<uint8_t>('A' + i);
    const graft_mask m{4000, 8, 4005, 8, GRAFT_MASK_EXACT, 0};
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 3;
    o.filter = GRAFT_FILTER_X86;
    o.masks = &m;
    o.mask_count = 1;
    VecSink s;
    graft_sink sink = s.sink();
    CHECK_EQ(graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &sink), GRAFT_OK);
    Bytes out, want = newb;
    for (size_t i = 0; i < 8; ++i) want[4005 + i] = static_cast<uint8_t>('A' + i);
    CHECK_EQ(apply_patch(installed, s.data, out), GRAFT_OK);
    CHECK(out == want);
}

TEST(filter_unknown_rejected) {
    const Bytes a{1, 2, 3};
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.filter = 42;
    VecSink s;
    graft_sink sink = s.sink();
    CHECK_EQ(graft_diff(a.data(), a.size(), a.data(), a.size(), &o, &sink), GRAFT_E_INVALID_ARG);
}
