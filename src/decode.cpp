// decode.cpp: streaming .gft v1 decoder. Treats the patch as hostile.

#include "format.h"

#include <blake3.h>
#include <zstd.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace graft {
namespace {

// Buffered patch input that hashes exactly the bytes consumed.
class PatchIn {
public:
    explicit PatchIn(const graft_reader *r) : r_(r), buf_(1 << 16) { blake3_hasher_init(&h_); }

    graft_status read(void *dst, size_t n) {
        auto *d = static_cast<uint8_t *>(dst);
        while (n) {
            if (pos_ == len_) {
                if (graft_status st = fill(); st != GRAFT_OK) return st;
                if (len_ == 0) return GRAFT_E_CORRUPT;  // truncated
            }
            const size_t k = std::min(n, len_ - pos_);
            std::memcpy(d, buf_.data() + pos_, k);
            blake3_hasher_update(&h_, d, k);
            pos_ += k;
            d += k;
            n -= k;
        }
        return GRAFT_OK;
    }

    graft_status read_uvarint(uint64_t &v) {
        VarintReader vr;
        for (;;) {
            uint8_t b;
            if (graft_status st = read(&b, 1); st != GRAFT_OK) return st;
            if (vr.feed(b)) break;
        }
        if (!vr.ok()) return GRAFT_E_CORRUPT;
        v = vr.value();
        return GRAFT_OK;
    }

    graft_status expect_eof() {
        if (pos_ < len_) return GRAFT_E_CORRUPT;
        if (graft_status st = fill(); st != GRAFT_OK) return st;
        return len_ == 0 ? GRAFT_OK : GRAFT_E_CORRUPT;
    }

    void digest(uint8_t out[GRAFT_HASH_SIZE]) const { blake3_hasher_finalize(&h_, out, GRAFT_HASH_SIZE); }

private:
    graft_status fill() {
        size_t got = 0;
        pos_ = len_ = 0;
        if (graft_status st = r_->read(r_->ctx, buf_.data(), buf_.size(), &got); st != GRAFT_OK) return st;
        if (got > buf_.size()) return GRAFT_E_IO;  // misbehaving reader
        len_ = got;
        return GRAFT_OK;
    }

    const graft_reader *r_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0, len_ = 0;
    blake3_hasher h_;
};

struct DCtxDeleter {
    void operator()(ZSTD_DCtx *d) const { ZSTD_freeDCtx(d); }
};

bool hashes_equal(const uint8_t *a, const uint8_t *b) {
    if (!kCheckHashes) return true;
    return std::memcmp(a, b, GRAFT_HASH_SIZE) == 0;
}

// Header plus, when flagged, the mask table.
graft_status read_preamble(PatchIn &in, Header &h, std::vector<Mask> &masks) {
    uint8_t raw[kHeaderSize];
    if (graft_status st = in.read(raw, sizeof raw); st != GRAFT_OK) return st;
    if (graft_status st = decode_header(raw, h); st != GRAFT_OK) return st;
    masks.clear();
    if (!(h.flags & kFlagMasks)) return GRAFT_OK;

    uint64_t count;
    if (graft_status st = in.read_uvarint(count); st != GRAFT_OK) return st;
    if (count == 0 || count > kMaxMasks) return GRAFT_E_CORRUPT;
    masks.resize(static_cast<size_t>(count));
    for (Mask &k : masks) {
        uint8_t mp[2];
        if (graft_status st = in.read_uvarint(k.old_offset); st != GRAFT_OK) return st;
        if (graft_status st = in.read_uvarint(k.old_length); st != GRAFT_OK) return st;
        if (graft_status st = in.read_uvarint(k.new_offset); st != GRAFT_OK) return st;
        if (graft_status st = in.read_uvarint(k.new_length); st != GRAFT_OK) return st;
        if (graft_status st = in.read(mp, 2); st != GRAFT_OK) return st;
        k.mode = mp[0];
        k.pad = mp[1];
    }
    return masks_valid(masks, h.source_size, h.target_size) ? GRAFT_OK : GRAFT_E_CORRUPT;
}

// The installed source as the patch sees it: masked regions read as kMaskFill.
struct NormalizedSource {
    const graft_source *real;
    const std::vector<Mask> *masks;  // sorted by old_offset, disjoint

    static graft_status read_at(void *ctx, uint64_t off, void *buf, size_t len) {
        auto *self = static_cast<NormalizedSource *>(ctx);
        if (graft_status st = self->real->read_at(self->real->ctx, off, buf, len); st != GRAFT_OK) return st;
        const auto &m = *self->masks;
        auto it = std::partition_point(m.begin(), m.end(),
                                       [&](const Mask &k) { return k.old_offset + k.old_length <= off; });
        for (; it != m.end() && it->old_offset < off + len; ++it) {
            const uint64_t lo = std::max(it->old_offset, off);
            const uint64_t hi = std::min(it->old_offset + it->old_length, off + len);
            std::memset(static_cast<uint8_t *>(buf) + (lo - off), kMaskFill, static_cast<size_t>(hi - lo));
        }
        return GRAFT_OK;
    }
};

// Bytes to write over the output at [offset, offset + bytes.size()).
struct Stamp {
    uint64_t offset;
    std::vector<uint8_t> bytes;
    uint64_t end() const { return offset + bytes.size(); }
};

graft_status hash_source(const graft_source &src, uint8_t out[GRAFT_HASH_SIZE]) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(src.size, 1 << 20)));
    for (uint64_t off = 0; off < src.size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), src.size - off));
        if (graft_status st = src.read_at(src.ctx, off, buf.data(), n); st != GRAFT_OK) return st;
        blake3_hasher_update(&h, buf.data(), n);
        off += n;
    }
    blake3_hasher_finalize(&h, out, GRAFT_HASH_SIZE);
    return GRAFT_OK;
}

class Decoder {
public:
    Decoder(const graft_source &src, PatchIn &in, const graft_sink &out, const Header &h,
            const std::vector<Stamp> &stamps)
        : src_(src), in_(in), out_(out), h_(h), stamps_(stamps), scratch_(1 << 16) {
        blake3_hasher_init(&target_hash_);
    }

    graft_status run() {
        dctx_.reset(ZSTD_createDCtx());
        if (!dctx_) return GRAFT_E_NOMEM;
        for (;;) {
            uint8_t tag;
            if (graft_status st = in_.read(&tag, 1); st != GRAFT_OK) return st;
            if (tag == kTagEnd) break;
            if (tag != kTagBlock) return GRAFT_E_CORRUPT;
            if (graft_status st = block(); st != GRAFT_OK) return st;
        }
        if (produced_ != h_.target_size) return GRAFT_E_CORRUPT;

        uint8_t want[GRAFT_HASH_SIZE], got[GRAFT_HASH_SIZE];
        in_.digest(got);
        if (graft_status st = in_.read(want, sizeof want); st != GRAFT_OK) return st;
        if (!hashes_equal(want, got)) return GRAFT_E_CORRUPT;
        if (graft_status st = in_.expect_eof(); st != GRAFT_OK) return st;

        blake3_hasher_finalize(&target_hash_, got, GRAFT_HASH_SIZE);
        if (!hashes_equal(h_.target_hash, got)) return GRAFT_E_CORRUPT;
        return GRAFT_OK;
    }

private:
    graft_status stream(uint64_t raw, uint64_t comp, std::vector<uint8_t> &out) {
        out.clear();
        if (raw > h_.max_stream) return GRAFT_E_CORRUPT;
        if (raw == 0) return comp == 0 ? GRAFT_OK : GRAFT_E_CORRUPT;
        if (comp == 0 || comp > ZSTD_compressBound(static_cast<size_t>(raw))) return GRAFT_E_CORRUPT;
        cbuf_.resize(static_cast<size_t>(comp));
        if (graft_status st = in_.read(cbuf_.data(), cbuf_.size()); st != GRAFT_OK) return st;
        out.resize(static_cast<size_t>(raw));
        const size_t n = ZSTD_decompressDCtx(dctx_.get(), out.data(), out.size(), cbuf_.data(), cbuf_.size());
        if (ZSTD_isError(n) || n != raw) return GRAFT_E_CORRUPT;
        return GRAFT_OK;
    }

    // The target hash covers the normalized output; stamps go on after.
    graft_status emit(const uint8_t *p, size_t n) {
        if (n == 0) return GRAFT_OK;
        blake3_hasher_update(&target_hash_, p, n);
        const uint8_t *outp = p;
        while (stamp_ < stamps_.size() && stamps_[stamp_].end() <= produced_) ++stamp_;
        if (stamp_ < stamps_.size() && stamps_[stamp_].offset < produced_ + n) {
            overlay_.assign(p, p + n);
            for (size_t i = stamp_; i < stamps_.size() && stamps_[i].offset < produced_ + n; ++i) {
                const Stamp &s = stamps_[i];
                const uint64_t lo = std::max(s.offset, produced_), hi = std::min(s.end(), produced_ + n);
                std::memcpy(overlay_.data() + (lo - produced_), s.bytes.data() + (lo - s.offset),
                            static_cast<size_t>(hi - lo));
            }
            outp = overlay_.data();
        }
        produced_ += n;
        return out_.write(out_.ctx, outp, n);
    }

    graft_status block() {
        uint64_t len[6];
        for (auto &v : len)
            if (graft_status st = in_.read_uvarint(v); st != GRAFT_OK) return st;
        // len = ctrl_raw ctrl_comp diff_raw diff_comp extra_raw extra_comp
        const uint64_t remaining = h_.target_size - produced_;
        if (len[2] > remaining || len[4] > remaining - len[2]) return GRAFT_E_CORRUPT;
        if (graft_status st = stream(len[0], len[1], ctrl_); st != GRAFT_OK) return st;
        if (graft_status st = stream(len[2], len[3], diff_); st != GRAFT_OK) return st;
        if (graft_status st = stream(len[4], len[5], extra_); st != GRAFT_OK) return st;

        const uint8_t *cp = ctrl_.data(), *cend = cp + ctrl_.size();
        size_t dpos = 0, epos = 0;
        while (cp < cend) {
            uint64_t add, copy, zseek;
            if (!get_uvarint(cp, cend, add) || !get_uvarint(cp, cend, copy) || !get_uvarint(cp, cend, zseek))
                return GRAFT_E_CORRUPT;

            if (add > diff_.size() - dpos || add > h_.source_size - oldpos_) return GRAFT_E_CORRUPT;
            for (uint64_t done = 0; done < add;) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(scratch_.size(), add - done));
                if (graft_status st = src_.read_at(src_.ctx, oldpos_ + done, scratch_.data(), n); st != GRAFT_OK)
                    return st;
                uint8_t *d = diff_.data() + dpos + done;
                for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(d[i] + scratch_[i]);
                if (graft_status st = emit(d, n); st != GRAFT_OK) return st;
                done += n;
            }
            dpos += static_cast<size_t>(add);
            oldpos_ += add;

            if (copy > extra_.size() - epos) return GRAFT_E_CORRUPT;
            if (graft_status st = emit(extra_.data() + epos, static_cast<size_t>(copy)); st != GRAFT_OK) return st;
            epos += static_cast<size_t>(copy);

            const int64_t seek = unzigzag(zseek);
            if (seek < 0) {
                const uint64_t back = static_cast<uint64_t>(-(seek + 1)) + 1;  // no overflow at INT64_MIN
                if (back > oldpos_) return GRAFT_E_CORRUPT;
                oldpos_ -= back;
            } else {
                if (static_cast<uint64_t>(seek) > h_.source_size - oldpos_) return GRAFT_E_CORRUPT;
                oldpos_ += static_cast<uint64_t>(seek);
            }
        }
        if (dpos != diff_.size() || epos != extra_.size()) return GRAFT_E_CORRUPT;
        return GRAFT_OK;
    }

    const graft_source &src_;
    PatchIn &in_;
    const graft_sink &out_;
    const Header &h_;
    const std::vector<Stamp> &stamps_;  // sorted by offset, disjoint
    size_t stamp_ = 0;
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx_;
    std::vector<uint8_t> ctrl_, diff_, extra_, cbuf_, scratch_, overlay_;
    uint64_t oldpos_ = 0, produced_ = 0;
    blake3_hasher target_hash_;
};

graft_status apply_impl(const graft_source &src, const graft_reader &patch, const graft_sink &out,
                        const graft_apply_limits &lim) {
    PatchIn in(&patch);
    Header h;
    std::vector<Mask> masks;
    if (graft_status st = read_preamble(in, h, masks); st != GRAFT_OK) return st;
    if (h.max_stream > lim.max_stream || h.target_size > lim.max_target_size) return GRAFT_E_LIMIT;
    uint64_t old_total = 0, new_total = 0;  // each <= source/target size: no overflow
    for (const Mask &k : masks) {
        old_total += k.old_length;
        new_total += k.new_length;
    }
    if (old_total > lim.max_stream || new_total > lim.max_stream) return GRAFT_E_LIMIT;
    if (src.size != h.source_size) return GRAFT_E_WRONG_SOURCE;

    // Lift the installed bytes out of each masked region.
    std::vector<Stamp> stamps;
    for (const Mask &k : masks) {
        Stamp s{k.new_offset, std::vector<uint8_t>(static_cast<size_t>(k.new_length), k.pad)};
        const size_t keep = static_cast<size_t>(std::min(k.old_length, k.new_length));
        if (graft_status st = src.read_at(src.ctx, k.old_offset, s.bytes.data(), keep); st != GRAFT_OK) return st;
        stamps.push_back(std::move(s));
    }
    std::sort(stamps.begin(), stamps.end(), [](const Stamp &a, const Stamp &b) { return a.offset < b.offset; });

    NormalizedSource norm{&src, &masks};
    const graft_source nsrc{&norm, src.size, &NormalizedSource::read_at};
    const graft_source &use = masks.empty() ? src : nsrc;

    uint8_t sh[GRAFT_HASH_SIZE];
    if (graft_status st = hash_source(use, sh); st != GRAFT_OK) return st;
    if (!hashes_equal(sh, h.source_hash)) return GRAFT_E_WRONG_SOURCE;

    Decoder d(use, in, out, h, stamps);
    return d.run();
}

template <class F>
graft_status guarded(F &&f) {
    try {
        return f();
    } catch (const std::bad_alloc &) {
        return GRAFT_E_NOMEM;
    } catch (const std::length_error &) {
        return GRAFT_E_NOMEM;
    }
}

}  // namespace
}  // namespace graft

extern "C" void graft_apply_limits_init(graft_apply_limits *l) {
    if (!l) return;
    l->struct_size = sizeof *l;
    l->max_stream = GRAFT_DEFAULT_MAX_STREAM;
    l->max_target_size = UINT64_MAX;
}

extern "C" graft_status graft_apply(const graft_source *source, const graft_reader *patch, const graft_sink *out,
                                    const graft_apply_limits *limits) {
    if (!source || !source->read_at || !patch || !patch->read || !out || !out->write) return GRAFT_E_INVALID_ARG;
    graft_apply_limits l;
    graft_apply_limits_init(&l);
    if (limits) {
        if (limits->struct_size < sizeof l) return GRAFT_E_INVALID_ARG;
        l.max_stream = limits->max_stream;
        l.max_target_size = limits->max_target_size;
    }
    return graft::guarded([&] { return graft::apply_impl(*source, *patch, *out, l); });
}

extern "C" graft_status graft_patch_read_info(const graft_reader *patch, graft_patch_info *info) {
    if (!patch || !patch->read || !info) return GRAFT_E_INVALID_ARG;
    return graft::guarded([&] {
        graft::PatchIn in(patch);
        graft::Header h;
        std::vector<graft::Mask> masks;
        if (graft_status st = graft::read_preamble(in, h, masks); st != GRAFT_OK) return st;
        info->mask_count = static_cast<uint32_t>(masks.size());
        info->format_version = h.version;
        info->flags = h.flags;
        info->max_stream = h.max_stream;
        info->source_size = h.source_size;
        info->target_size = h.target_size;
        std::memcpy(info->source_hash, h.source_hash, GRAFT_HASH_SIZE);
        std::memcpy(info->target_hash, h.target_hash, GRAFT_HASH_SIZE);
        return GRAFT_OK;
    });
}
