#include "check.h"
#include "util.h"

#include <string>

using namespace testutil;

namespace {

// A "program" with a serial number field. `shift` moves everything from
// `at` onward, as a new build with more code before the field would.
struct Build {
    Bytes bytes;
    uint64_t serial_offset;
};

Build make_build(std::mt19937_64 &rng, const Bytes &code, size_t extra_code, const std::string &serial) {
    Build b;
    b.bytes = code;
    Bytes more = random_bytes(rng, extra_code);
    b.bytes.insert(b.bytes.begin() + 1000, more.begin(), more.end());
    const std::string marker = "SERIAL:";
    b.serial_offset = 5000 + extra_code + marker.size();
    b.bytes.insert(b.bytes.begin() + static_cast<std::ptrdiff_t>(5000 + extra_code), marker.begin(), marker.end());
    b.bytes.insert(b.bytes.begin() + static_cast<std::ptrdiff_t>(b.serial_offset), serial.begin(), serial.end());
    return b;
}

Bytes with_serial(Bytes b, uint64_t at, const std::string &serial) {
    std::copy(serial.begin(), serial.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
    return b;
}

graft_status make_masked(const Bytes &oldb, const Bytes &newb, const std::vector<graft_mask> &masks, Bytes &patch,
                         uint32_t block_size = 0) {
    graft_diff_options o;
    graft_diff_options_init(&o);
    o.level = 3;
    if (block_size) o.block_size = block_size;
    o.masks = masks.data();
    o.mask_count = masks.size();
    VecSink s;
    graft_sink sink = s.sink();
    const graft_status st = graft_diff(oldb.data(), oldb.size(), newb.data(), newb.size(), &o, &sink);
    patch = std::move(s.data);
    return st;
}

}  // namespace

TEST(masks_serial_preserved_across_moved_field) {
    std::mt19937_64 rng(10);
    const Bytes code = random_bytes(rng, 20000);
    // Vendor builds: v1 and v2 both carry a placeholder serial; v2 has more code.
    const Build v1 = make_build(rng, code, 0, "0000-0000-0000-0000");
    const Build v2 = make_build(rng, code, 333, "0000-0000-0000-0000");
    // A customer's installed v1, stamped with their serial.
    const Bytes installed = with_serial(v1.bytes, v1.serial_offset, "ABCD-1234-EFGH-5678");

    const std::vector<graft_mask> masks{{v1.serial_offset, 19, v2.serial_offset, 19, GRAFT_MASK_EXACT, 0}};
    Bytes patch, out;
    CHECK_EQ(make_masked(v1.bytes, v2.bytes, masks, patch), GRAFT_OK);

    CHECK_EQ(apply_patch(installed, patch, out), GRAFT_OK);
    CHECK(out == with_serial(v2.bytes, v2.serial_offset, "ABCD-1234-EFGH-5678"));

    // The vendor's own unstamped v1 works too, and keeps its placeholder.
    CHECK_EQ(apply_patch(v1.bytes, patch, out), GRAFT_OK);
    CHECK(out == v2.bytes);

    // A difference outside the mask is still the wrong source.
    Bytes tampered = installed;
    tampered[100] ^= 1;
    CHECK_EQ(apply_patch(tampered, patch, out), GRAFT_E_WRONG_SOURCE);
}

TEST(masks_pad_and_truncate) {
    std::mt19937_64 rng(11);
    const Bytes oldb = random_bytes(rng, 4000);
    Bytes newb = oldb;
    newb.insert(newb.begin() + 2000, 8, 0);  // the field grows from 8 to 16 bytes
    Bytes installed = oldb;
    for (size_t i = 0; i < 8; ++i) installed[2000 + i - 8] = static_cast<uint8_t>('a' + i);  // field at 1992..2000

    Bytes patch, out;
    CHECK_EQ(make_masked(oldb, newb, {{1992, 8, 1992, 16, GRAFT_MASK_PAD, '#'}}, patch), GRAFT_OK);
    CHECK_EQ(apply_patch(installed, patch, out), GRAFT_OK);
    Bytes want = newb;
    for (size_t i = 0; i < 8; ++i) want[1992 + i] = static_cast<uint8_t>('a' + i);
    for (size_t i = 8; i < 16; ++i) want[1992 + i] = '#';
    CHECK(out == want);

    // Shrinking: keep the leading bytes.
    Bytes small = oldb;
    small.erase(small.begin() + 1996, small.begin() + 2000);  // field shrinks from 8 to 4
    CHECK_EQ(make_masked(oldb, small, {{1992, 8, 1992, 4, GRAFT_MASK_TRUNCATE, 0}}, patch), GRAFT_OK);
    CHECK_EQ(apply_patch(installed, patch, out), GRAFT_OK);
    Bytes want2 = small;
    for (size_t i = 0; i < 4; ++i) want2[1992 + i] = static_cast<uint8_t>('a' + i);
    CHECK(out == want2);
}

TEST(masks_many_regions_across_blocks) {
    std::mt19937_64 rng(12);
    const Bytes oldb = random_bytes(rng, 30000);
    Bytes newb = oldb;
    newb.insert(newb.begin() + 50, {1, 2, 3});  // everything after shifts by 3
    std::vector<graft_mask> masks;
    Bytes installed = oldb, want = newb;
    // Unsorted on purpose; regions straddle the 1000-byte block boundaries.
    for (uint64_t at : {29000u, 995u, 12345u, 4990u, 100u}) {
        masks.push_back({at, 12, at + 3, 12, GRAFT_MASK_EXACT, 0});
        for (size_t i = 0; i < 12; ++i) {
            installed[at + i] = static_cast<uint8_t>(at + i);
            want[at + 3 + i] = static_cast<uint8_t>(at + i);
        }
    }
    Bytes patch, out;
    CHECK_EQ(make_masked(oldb, newb, masks, patch, 1000), GRAFT_OK);
    for (size_t chunk : {SIZE_MAX, size_t{1}, size_t{777}}) {
        CHECK_EQ(apply_patch(installed, patch, out, chunk), GRAFT_OK);
        CHECK(out == want);
    }
    MemReader r{&patch};
    graft_reader rd = r.reader();
    graft_patch_info info;
    CHECK_EQ(graft_patch_read_info(&rd, &info), GRAFT_OK);
    CHECK_EQ(info.mask_count, 5u);
}

TEST(masks_rejected_when_invalid) {
    const Bytes a(1000, 'a'), b(1000, 'b');
    Bytes patch;
    auto bad = [&](std::vector<graft_mask> m) { return make_masked(a, b, m, patch) == GRAFT_E_INVALID_ARG; };
    CHECK(bad({{990, 20, 0, 20, GRAFT_MASK_EXACT, 0}}));                                // past end of old
    CHECK(bad({{0, 20, 990, 20, GRAFT_MASK_EXACT, 0}}));                                // past end of new
    CHECK(bad({{0, 10, 0, 12, GRAFT_MASK_EXACT, 0}}));                                  // size change, no rule
    CHECK(bad({{0, 10, 0, 8, GRAFT_MASK_PAD, 0}}));                                     // pad can't shrink
    CHECK(bad({{0, 10, 0, 12, GRAFT_MASK_TRUNCATE, 0}}));                               // truncate can't grow
    CHECK(bad({{0, 0, 0, 0, GRAFT_MASK_EXACT, 0}}));                                    // empty
    CHECK(bad({{0, 10, 0, 10, 7, 0}}));                                                 // unknown mode
    CHECK(bad({{0, 10, 0, 10, GRAFT_MASK_EXACT, 0}, {5, 10, 100, 10, GRAFT_MASK_EXACT, 0}}));   // old overlap
    CHECK(bad({{0, 10, 0, 10, GRAFT_MASK_EXACT, 0}, {50, 10, 5, 10, GRAFT_MASK_EXACT, 0}}));    // new overlap
    CHECK(bad({{UINT64_MAX - 5, 10, 0, 10, GRAFT_MASK_EXACT, 0}}));                     // overflow

    graft_diff_options o;
    graft_diff_options_init(&o);
    o.mask_count = 1;  // but masks == NULL
    VecSink s;
    graft_sink sink = s.sink();
    CHECK_EQ(graft_diff(a.data(), a.size(), b.data(), b.size(), &o, &sink), GRAFT_E_INVALID_ARG);
}

TEST(masks_patch_rejects_every_single_byte_corruption) {
    std::mt19937_64 rng(13);
    const Bytes oldb = random_bytes(rng, 2000);
    Bytes newb = oldb;
    newb.insert(newb.begin() + 10, {9, 9});
    Bytes installed = oldb;
    installed[500] = 'S';
    Bytes patch;
    CHECK_EQ(make_masked(oldb, newb, {{495, 10, 497, 10, GRAFT_MASK_EXACT, 0}}, patch, 700), GRAFT_OK);
    for (size_t i = 0; i < patch.size(); ++i) {
        Bytes bad = patch, out;
        bad[i] ^= static_cast<uint8_t>(1u << (i % 8));
        if (apply_patch(installed, bad, out) == GRAFT_OK) {
            check::fail(__FILE__, __LINE__, "corruption at offset " + std::to_string(i) + " accepted");
            break;
        }
    }
}
