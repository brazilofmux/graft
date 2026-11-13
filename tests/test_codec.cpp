#include "check.h"
#include "util.h"

#include <chrono>
#include <string>

using namespace testutil;

namespace {

void roundtrip(const Bytes &oldb, const Bytes &newb, uint32_t block_size = 0, size_t chunk = SIZE_MAX) {
    Bytes patch, out;
    CHECK_EQ(make_patch(oldb, newb, patch, block_size), GRAFT_OK);
    CHECK_EQ(apply_patch(oldb, patch, out, chunk), GRAFT_OK);
    CHECK(out == newb);
}

Bytes edited(const Bytes &b, std::mt19937_64 &rng, size_t edits) {
    Bytes r = b;
    for (size_t i = 0; i < edits && !r.empty(); ++i) {
        const size_t pos = rng() % r.size();
        switch (rng() % 3) {
        case 0: r[pos] = static_cast<uint8_t>(rng()); break;
        case 1: r.insert(r.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<uint8_t>(rng())); break;
        default: r.erase(r.begin() + static_cast<std::ptrdiff_t>(pos)); break;
        }
    }
    return r;
}

}  // namespace

TEST(codec_roundtrip_edge_sizes) {
    std::mt19937_64 rng(1);
    const Bytes empty;
    const Bytes one{42};
    const Bytes some = random_bytes(rng, 1000);
    roundtrip(empty, empty);
    roundtrip(empty, some);
    roundtrip(some, empty);
    roundtrip(one, one);
    roundtrip(one, some);
    roundtrip(some, one);
    roundtrip(some, some);
}

TEST(codec_roundtrip_random_edits) {
    std::mt19937_64 rng(2);
    for (size_t n : {size_t{10}, size_t{4096}, size_t{100000}}) {
        const Bytes oldb = random_bytes(rng, n);
        for (size_t edits : {size_t{1}, size_t{10}, n / 10}) roundtrip(oldb, edited(oldb, rng, edits));
    }
}

TEST(codec_roundtrip_multiblock_and_chunked_reads) {
    std::mt19937_64 rng(3);
    const Bytes oldb = random_bytes(rng, 50000);
    const Bytes newb = edited(oldb, rng, 200);
    for (uint32_t bs : {GRAFT_MIN_BLOCK_SIZE, 100u, 4096u}) {
        roundtrip(oldb, newb, bs);
        roundtrip(oldb, newb, bs, 1);  // 1-byte reads from the patch
        roundtrip(oldb, newb, bs, 7);
    }
}

TEST(codec_patch_is_small_for_small_edits) {
    std::mt19937_64 rng(4);
    const Bytes oldb = random_bytes(rng, 1 << 20);
    Bytes newb = oldb;
    for (size_t i = 0; i < 100; ++i) newb[rng() % newb.size()] ^= 0xff;
    Bytes patch;
    CHECK_EQ(make_patch(oldb, newb, patch), GRAFT_OK);
    CHECK(patch.size() < 4096);
}

TEST(codec_repetitive_input_is_not_quadratic) {
    // Long runs and periodic data are bsdiff's slow cases.
    Bytes oldb(1 << 20, 0), newb(1 << 20, 0);
    for (size_t i = 0; i < newb.size(); i += 1000) newb[i] = 1;
    Bytes periodic_old(1 << 20), periodic_new;
    for (size_t i = 0; i < periodic_old.size(); ++i) periodic_old[i] = static_cast<uint8_t>("abcabcabd"[i % 9]);
    periodic_new = periodic_old;
    periodic_new.insert(periodic_new.begin() + 1000, {'x', 'y'});

    const auto t0 = std::chrono::steady_clock::now();
    roundtrip(oldb, newb);
    roundtrip(periodic_old, periodic_new);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(secs < 20.0);  // generous: sanitizer builds are slow
}

TEST(codec_rejects_every_single_byte_corruption) {
    std::mt19937_64 rng(5);
    const Bytes oldb = random_bytes(rng, 3000);
    const Bytes newb = edited(oldb, rng, 30);
    Bytes patch;
    CHECK_EQ(make_patch(oldb, newb, patch, 1000), GRAFT_OK);
    for (size_t i = 0; i < patch.size(); ++i) {
        Bytes bad = patch, out;
        bad[i] ^= static_cast<uint8_t>(1u << (i % 8));
        if (apply_patch(oldb, bad, out) == GRAFT_OK) {
            check::fail(__FILE__, __LINE__, "corruption at offset " + std::to_string(i) + " accepted");
            break;
        }
    }
}

TEST(codec_rejects_truncation_and_trailing_bytes) {
    std::mt19937_64 rng(6);
    const Bytes oldb = random_bytes(rng, 2000);
    const Bytes newb = edited(oldb, rng, 10);
    Bytes patch, out;
    CHECK_EQ(make_patch(oldb, newb, patch, 500), GRAFT_OK);
    for (size_t n = 0; n < patch.size(); ++n) {
        Bytes cut(patch.begin(), patch.begin() + static_cast<std::ptrdiff_t>(n));
        if (apply_patch(oldb, cut, out) == GRAFT_OK) {
            check::fail(__FILE__, __LINE__, "truncation to " + std::to_string(n) + " accepted");
            break;
        }
    }
    Bytes longer = patch;
    longer.push_back(0);
    CHECK_EQ(apply_patch(oldb, longer, out), GRAFT_E_CORRUPT);
}

TEST(codec_wrong_source) {
    std::mt19937_64 rng(7);
    const Bytes oldb = random_bytes(rng, 2000);
    const Bytes newb = edited(oldb, rng, 10);
    Bytes patch, out;
    CHECK_EQ(make_patch(oldb, newb, patch), GRAFT_OK);

    Bytes other = oldb;
    other[1000] ^= 1;
    CHECK_EQ(apply_patch(other, patch, out), GRAFT_E_WRONG_SOURCE);
    CHECK(out.empty());  // rejected before any output

    Bytes shorter(oldb.begin(), oldb.end() - 1);
    CHECK_EQ(apply_patch(shorter, patch, out), GRAFT_E_WRONG_SOURCE);
}

TEST(codec_limits) {
    std::mt19937_64 rng(8);
    const Bytes oldb = random_bytes(rng, 10000);
    const Bytes newb = random_bytes(rng, 10000);
    Bytes patch, out;
    CHECK_EQ(make_patch(oldb, newb, patch, 4096), GRAFT_OK);

    graft_apply_limits lim;
    graft_apply_limits_init(&lim);
    lim.max_stream = 4095;
    CHECK_EQ(apply_patch(oldb, patch, out, SIZE_MAX, &lim), GRAFT_E_LIMIT);
    lim.max_stream = 4096;
    lim.max_target_size = 9999;
    CHECK_EQ(apply_patch(oldb, patch, out, SIZE_MAX, &lim), GRAFT_E_LIMIT);
    lim.max_target_size = 10000;
    CHECK_EQ(apply_patch(oldb, patch, out, SIZE_MAX, &lim), GRAFT_OK);
    CHECK(out == newb);
}

TEST(codec_info) {
    std::mt19937_64 rng(9);
    const Bytes oldb = random_bytes(rng, 123);
    const Bytes newb = random_bytes(rng, 456);
    Bytes patch;
    CHECK_EQ(make_patch(oldb, newb, patch), GRAFT_OK);
    MemReader r{&patch};
    graft_reader rd = r.reader();
    graft_patch_info info;
    CHECK_EQ(graft_patch_read_info(&rd, &info), GRAFT_OK);
    CHECK_EQ(info.format_version, 1);
    CHECK_EQ(info.source_size, 123u);
    CHECK_EQ(info.target_size, 456u);
    uint8_t h[GRAFT_HASH_SIZE];
    graft_hash(newb.data(), newb.size(), h);
    CHECK(std::memcmp(h, info.target_hash, sizeof h) == 0);

    Bytes garbage(200, 'x');
    MemReader g{&garbage};
    graft_reader grd = g.reader();
    CHECK_EQ(graft_patch_read_info(&grd, &info), GRAFT_E_CORRUPT);
}

TEST(codec_rejects_bad_options) {
    const Bytes a{1, 2, 3};
    VecSink s;
    graft_sink sink = s.sink();
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 0;
    CHECK_EQ(graft_diff(a.data(), a.size(), a.data(), a.size(), &o, &sink), GRAFT_E_INVALID_ARG);
    graft_diff_options_init(&o);
    o.block_size = GRAFT_MIN_BLOCK_SIZE - 1;
    CHECK_EQ(graft_diff(a.data(), a.size(), a.data(), a.size(), &o, &sink), GRAFT_E_INVALID_ARG);
    graft_diff_options_init(&o);
    o.struct_size = 4;
    CHECK_EQ(graft_diff(a.data(), a.size(), a.data(), a.size(), &o, &sink), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_diff(nullptr, 1, a.data(), a.size(), nullptr, &sink), GRAFT_E_INVALID_ARG);
    CHECK_EQ(graft_diff(a.data(), a.size(), a.data(), a.size(), nullptr, nullptr), GRAFT_E_INVALID_ARG);
}
