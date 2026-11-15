// encode.cpp: bsdiff-class encoder.
//
// Matching follows Colin Percival's bsdiff 4 (suffix array of the source,
// then a forward scan that extends approximate matches in both directions);
// the suffix array comes from libsais, and the output is .gft v1.

#include "filter.h"
#include "format.h"

#include <blake3.h>
#include <libsais.h>
#include <libsais64.h>
#include <zstd.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace graft {
namespace {

// AUTO uses the suffix array (4-8 bytes per source byte) up to this size.
constexpr size_t kSuffixLimit = size_t{512} << 20;

struct Control {
    uint64_t add;
    uint64_t copy;
    int64_t seek;
};

// --- matching -------------------------------------------------------------------

int64_t matchlen(const uint8_t *a, int64_t alen, const uint8_t *b, int64_t blen) {
    const int64_t n = std::min(alen, blen);
    int64_t i = 0;
    while (i + 8 <= n) {
        uint64_t x, y;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        if (x != y) break;
        i += 8;
    }
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

// Longest match for `nw` among source suffixes (sa sorted). Sets pos.
//
// Unlike bsdiff 4's search, a suffix that is a proper prefix of the query
// sorts *before* it (true lexicographic order). bsdiff treats it as equal,
// which on repetitive data steers the search to the shortest such suffix:
// a short match where a long one exists, and quadratic time overall. The
// longest common prefix with the bracketing suffixes is carried along
// (Manber-Myers), so bytes already known to match are not compared again.
template <class Idx>
int64_t search(const Idx *sa, const uint8_t *old, int64_t oldsize, const uint8_t *nw, int64_t newsize,
               int64_t &pos) {
    int64_t lo = -1, hi = oldsize;  // sa[lo] < query < sa[hi]; -1 and oldsize are sentinels
    int64_t llo = 0, lhi = 0;       // match lengths with sa[lo], sa[hi]
    while (hi - lo > 1) {
        const int64_t mid = lo + (hi - lo) / 2;
        const int64_t s = static_cast<int64_t>(sa[mid]);
        const int64_t k = std::min(llo, lhi);
        const int64_t l = k + matchlen(old + s + k, oldsize - s - k, nw + k, newsize - k);
        if (l == newsize) {  // the whole query matches: can't do better
            pos = s;
            return l;
        }
        if (s + l == oldsize || old[s + l] < nw[l]) {
            lo = mid;
            llo = l;
        } else {
            hi = mid;
            lhi = l;
        }
    }
    if (lo >= 0 && (hi >= oldsize || llo >= lhi)) {
        pos = static_cast<int64_t>(sa[lo]);
        return llo;
    }
    pos = static_cast<int64_t>(sa[hi]);
    return lhi;
}

// bsdiff's step between two matches: extend the previous match (at
// lastscan/lastpos) forward and the next one (at scan/pos) backward, both
// approximately (more than half the bytes agree), split any overlap at the
// best point, and emit the control. scan == newsize finishes.
void emit_between(const uint8_t *old, int64_t oldsize, const uint8_t *nw, int64_t newsize, int64_t &lastscan,
                  int64_t &lastpos, int64_t scan, int64_t pos, std::vector<Control> &out,
                  int64_t max_back = INT64_MAX) {
    int64_t s = 0, sf = 0, lenf = 0;
    for (int64_t i = 0; lastscan + i < scan && lastpos + i < oldsize;) {
        if (old[lastpos + i] == nw[lastscan + i]) ++s;
        ++i;
        if (s * 2 - i > sf * 2 - lenf) {
            sf = s;
            lenf = i;
        }
    }
    int64_t lenb = 0;
    if (scan < newsize) {
        int64_t sb = 0;
        s = 0;
        for (int64_t i = 1; scan >= lastscan + i && pos >= i && i <= max_back; ++i) {
            if (old[pos - i] == nw[scan - i]) ++s;
            if (s * 2 - i > sb * 2 - lenb) {
                sb = s;
                lenb = i;
            }
        }
    }
    if (lastscan + lenf > scan - lenb) {
        const int64_t overlap = (lastscan + lenf) - (scan - lenb);
        int64_t ss = 0, lens = 0;
        s = 0;
        for (int64_t i = 0; i < overlap; ++i) {
            if (nw[lastscan + lenf - overlap + i] == old[lastpos + lenf - overlap + i]) ++s;
            if (nw[scan - lenb + i] == old[pos - lenb + i]) --s;
            if (s > ss) {
                ss = s;
                lens = i + 1;
            }
        }
        lenf += lens - overlap;
        lenb -= lens;
    }
    out.push_back({static_cast<uint64_t>(lenf), static_cast<uint64_t>((scan - lenb) - (lastscan + lenf)),
                   (pos - lenb) - (lastpos + lenf)});
    lastscan = scan - lenb;
    lastpos = pos - lenb;
}

template <class Idx>
void bsdiff_controls(const Idx *sa, const uint8_t *old, int64_t oldsize, const uint8_t *nw, int64_t newsize,
                     std::vector<Control> &out) {
    int64_t scan = 0, len = 0, pos = 0;
    int64_t lastscan = 0, lastpos = 0, lastoffset = 0;

    while (scan < newsize) {
        int64_t oldscore = 0;
        int64_t scsc;
        for (scsc = scan += len; scan < newsize; ++scan) {
            len = search(sa, old, oldsize, nw + scan, newsize - scan, pos);
            for (; scsc < scan + len; ++scsc)
                if (scsc + lastoffset < oldsize && old[scsc + lastoffset] == nw[scsc]) ++oldscore;
            if ((len == oldscore && len != 0) || len > oldscore + 8) break;
            if (scan + lastoffset < oldsize && old[scan + lastoffset] == nw[scan]) --oldscore;
        }

        if (len != oldscore || scan == newsize) {
            emit_between(old, oldsize, nw, newsize, lastscan, lastpos, scan, pos, out);
            lastoffset = pos - scan;
        }
    }
}

// xdelta-class matching for sources too big for a suffix array: index the
// source by hashes of 32-byte blocks every `stride` bytes (the stride grows
// to keep the table within budget), roll the same hash over the target, and
// take exact matches (preferring to continue at the current alignment).
// Gaps between matches get bsdiff's approximate extension, so mostly-equal
// regions still become cheap diff runs.
void rolling_controls(const uint8_t *old, int64_t on, const uint8_t *nw, int64_t nn, std::vector<Control> &out) {
    constexpr int64_t B = 32;
    constexpr uint64_t M = 0x100000001b3ull;
    constexpr size_t kMaxTable = size_t{1} << 25;  // 256 MiB of offsets
    constexpr int64_t kMaxBack = 1 << 16;          // bounds each gap fill's backward scan
    int64_t lastscan = 0, lastpos = 0;
    if (on >= B && nn >= B) {
        uint64_t mb = 1;  // M^(B-1)
        for (int64_t k = 1; k < B; ++k) mb *= M;
        auto hash = [&](const uint8_t *p) {
            uint64_t h = 0;
            for (int64_t k = 0; k < B; ++k) h = h * M + p[k];
            return h;
        };
        const int64_t blocks_wanted = on / B;
        size_t bits = 10;
        while ((size_t{1} << bits) < static_cast<size_t>(blocks_wanted) * 2 && (size_t{1} << bits) < kMaxTable) ++bits;
        const size_t tsize = size_t{1} << bits;
        const int64_t stride = std::max<int64_t>(B, (on + static_cast<int64_t>(tsize / 2) - 1) / static_cast<int64_t>(tsize / 2));
        auto slot = [&](uint64_t h) { return static_cast<size_t>((h * 0x9e3779b97f4a7c15ull) >> (64 - bits)); };
        std::vector<int64_t> table(tsize, -1);
        for (int64_t p = 0; p + B <= on; p += stride) {
            int64_t &t = table[slot(hash(old + p))];
            if (t < 0) t = p;
        }

        int64_t i = 0, off = 0, last_end = 0;
        uint64_t h = hash(nw);
        while (i + B <= nn) {
            int64_t best = 0, bpos = 0;
            const int64_t p = i + off;  // continue at the current alignment
            if (p >= 0 && p + B <= on && std::memcmp(old + p, nw + i, B) == 0) {
                best = matchlen(old + p, on - p, nw + i, nn - i);
                bpos = p;
            }
            if (best < 4096) {  // a long continuation wins outright
                const int64_t c = table[slot(h)];
                if (c >= 0 && c != p && std::memcmp(old + c, nw + i, B) == 0) {
                    const int64_t l = matchlen(old + c, on - c, nw + i, nn - i);
                    if (l > best) {
                        best = l;
                        bpos = c;
                    }
                }
            }
            if (best >= B && bpos - i == off) {
                // The current alignment again (after an edit): the running
                // approximate match continues, as in bsdiff. Not a new anchor;
                // recording one would make each gap fill rescan the whole run.
                i += best;
                last_end = i;
                if (i + B <= nn) h = hash(nw + i);
            } else if (best >= B) {
                while (i > last_end && bpos > 0 && old[bpos - 1] == nw[i - 1]) {  // exact backward
                    --i;
                    --bpos;
                    ++best;
                }
                emit_between(old, on, nw, nn, lastscan, lastpos, i, bpos, out, kMaxBack);
                off = bpos - i;
                i += best;
                last_end = i;
                if (i + B <= nn) h = hash(nw + i);
            } else {
                if (i + B < nn) h = (h - nw[i] * mb) * M + nw[i + B];
                ++i;
            }
        }
    }
    emit_between(old, on, nw, nn, lastscan, lastpos, nn, 0, out);
}

graft_status make_controls(const uint8_t *old, size_t oldsize, const uint8_t *nw, size_t newsize, uint8_t method,
                           std::vector<Control> &out) {
    if (newsize == 0) return GRAFT_OK;
    if (oldsize == 0) {
        out.push_back({0, newsize, 0});
        return GRAFT_OK;
    }
    const auto on = static_cast<int64_t>(oldsize), nn = static_cast<int64_t>(newsize);
    if (method == GRAFT_METHOD_AUTO)
        method = static_cast<uint8_t>(oldsize <= kSuffixLimit ? GRAFT_METHOD_SUFFIX : GRAFT_METHOD_ROLLING);
    if (method == GRAFT_METHOD_ROLLING) {
        rolling_controls(old, on, nw, nn, out);
        return GRAFT_OK;
    }
    if (oldsize <= static_cast<size_t>(INT32_MAX)) {
        std::vector<int32_t> sa(oldsize);
        if (libsais(old, sa.data(), static_cast<int32_t>(oldsize), 0, nullptr) != 0) return GRAFT_E_NOMEM;
        bsdiff_controls(sa.data(), old, on, nw, nn, out);
    } else {
        std::vector<int64_t> sa(oldsize);
        if (libsais64(old, sa.data(), on, 0, nullptr) != 0) return GRAFT_E_NOMEM;
        bsdiff_controls(sa.data(), old, on, nw, nn, out);
    }
    return GRAFT_OK;
}

// --- output -------------------------------------------------------------------

struct RawBlock {
    std::vector<uint8_t> ctrl, diff, extra;
};

// A control, possibly split, placed in a block.
struct Piece {
    uint64_t add, copy;
    int64_t seek;
    size_t block;
};

size_t uvarint_len(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

// Splits controls into blocks of at most `limit` output bytes and at most
// `limit` ctrl bytes. Returns the largest stream any block will have.
size_t plan_blocks(const std::vector<Control> &controls, uint64_t limit, std::vector<Piece> &pieces) {
    size_t block = 0, ctrl = 0, max_stream = 0;
    uint64_t room = limit, diff = 0, extra = 0;
    auto flush = [&] {
        max_stream = std::max({max_stream, ctrl, static_cast<size_t>(diff), static_cast<size_t>(extra)});
        if (ctrl) ++block;
        ctrl = 0;
        diff = extra = 0;
        room = limit;
    };
    auto emit = [&](uint64_t add, uint64_t copy, int64_t seek) {
        if (ctrl + kMaxTriple > limit) flush();
        pieces.push_back({add, copy, seek, block});
        ctrl += uvarint_len(add) + uvarint_len(copy) + uvarint_len(zigzag(seek));
        diff += add;
        extra += copy;
        room -= add + copy;
    };
    for (Control c : controls) {
        for (;;) {
            if (room == 0) flush();
            if (c.add + c.copy <= room) {
                emit(c.add, c.copy, c.seek);
                break;
            }
            if (c.add >= room) {
                const uint64_t take = room;
                emit(take, 0, 0);
                c.add -= take;
            } else {
                const uint64_t take = room - c.add;
                emit(c.add, take, 0);
                c.add = 0;
                c.copy -= take;
            }
        }
    }
    flush();
    return max_stream;
}

class HashingSink {
public:
    explicit HashingSink(const graft_sink *s) : s_(s) { blake3_hasher_init(&h_); }
    graft_status write(const void *p, size_t n) {
        if (n == 0) return GRAFT_OK;
        blake3_hasher_update(&h_, p, n);
        return s_->write(s_->ctx, p, n);
    }
    void digest(uint8_t out[GRAFT_HASH_SIZE]) const { blake3_hasher_finalize(&h_, out, GRAFT_HASH_SIZE); }

private:
    const graft_sink *s_;
    blake3_hasher h_;
};

struct CCtxDeleter {
    void operator()(ZSTD_CCtx *c) const { ZSTD_freeCCtx(c); }
};

graft_status compress(ZSTD_CCtx *cctx, const std::vector<uint8_t> &raw, std::vector<uint8_t> &out) {
    out.clear();
    if (raw.empty()) return GRAFT_OK;
    out.resize(ZSTD_compressBound(raw.size()));
    const size_t n = ZSTD_compress2(cctx, out.data(), out.size(), raw.data(), raw.size());
    if (ZSTD_isError(n)) return GRAFT_E_NOMEM;
    out.resize(n);
    return GRAFT_OK;
}

graft_status diff_impl(const uint8_t *src, size_t slen, const uint8_t *tgt, size_t tlen,
                       const graft_diff_options &o, const std::vector<Mask> &masks, uint8_t filter,
                       const graft_sink *sink) {
    // With masks, diff the normalized files: every masked region zeroed. The
    // hashes are of the normalized files; a filter then applies on top.
    std::vector<uint8_t> nsrc, ntgt;
    if (!masks.empty() || filter) {
        nsrc.assign(src, src + slen);
        ntgt.assign(tgt, tgt + tlen);
        for (const Mask &k : masks) {
            std::memset(nsrc.data() + k.old_offset, kMaskFill, static_cast<size_t>(k.old_length));
            std::memset(ntgt.data() + k.new_offset, kMaskFill, static_cast<size_t>(k.new_length));
        }
    }
    Header h;
    graft_hash(masks.empty() ? src : nsrc.data(), slen, h.source_hash);
    graft_hash(masks.empty() ? tgt : ntgt.data(), tlen, h.target_hash);
    if (!masks.empty() || filter) {
        filter_encode(filter, nsrc.data(), nsrc.size());
        filter_encode(filter, ntgt.data(), ntgt.size());
        src = nsrc.data();
        tgt = ntgt.data();
    }

    std::vector<Control> controls;
    if (graft_status st = make_controls(src, slen, tgt, tlen, o.method, controls); st != GRAFT_OK) return st;

    // Pass 1 plans the blocks (the header needs the largest stream); pass 2
    // builds and writes one block at a time, so the patch is never all in memory.
    std::vector<Piece> pieces;
    const size_t max_stream = plan_blocks(controls, o.block_size, pieces);
    controls = {};

    h.flags = static_cast<uint16_t>((masks.empty() ? 0 : kFlagMasks) | (filter ? kFlagFilter : 0));
    h.source_size = slen;
    h.target_size = tlen;
    h.max_stream = static_cast<uint32_t>(max_stream);  // <= block_size <= 1 GiB

    HashingSink out(sink);
    uint8_t hdr[kHeaderSize];
    encode_header(h, hdr);
    if (graft_status st = out.write(hdr, sizeof hdr); st != GRAFT_OK) return st;
    if (filter)
        if (graft_status st = out.write(&filter, 1); st != GRAFT_OK) return st;
    if (!masks.empty()) {
        std::vector<uint8_t> table;
        encode_masks(masks, table);
        if (graft_status st = out.write(table.data(), table.size()); st != GRAFT_OK) return st;
    }

    std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
    if (!cctx) return GRAFT_E_NOMEM;
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, o.level);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 0);

    std::vector<uint8_t> c_ctrl, c_diff, c_extra, bh;
    RawBlock b;
    uint64_t oldpos = 0, newpos = 0;
    for (size_t i = 0; i < pieces.size();) {
        b.ctrl.clear();
        b.diff.clear();
        b.extra.clear();
        const size_t block = pieces[i].block;
        for (; i < pieces.size() && pieces[i].block == block; ++i) {
            const Piece &pc = pieces[i];
            put_uvarint(b.ctrl, pc.add);
            put_uvarint(b.ctrl, pc.copy);
            put_uvarint(b.ctrl, zigzag(pc.seek));
            for (uint64_t k = 0; k < pc.add; ++k) b.diff.push_back(static_cast<uint8_t>(tgt[newpos + k] - src[oldpos + k]));
            oldpos += pc.add;
            newpos += pc.add;
            b.extra.insert(b.extra.end(), tgt + newpos, tgt + newpos + pc.copy);
            newpos += pc.copy;
            oldpos = static_cast<uint64_t>(static_cast<int64_t>(oldpos) + pc.seek);
        }
        if (graft_status st = compress(cctx.get(), b.ctrl, c_ctrl); st != GRAFT_OK) return st;
        if (graft_status st = compress(cctx.get(), b.diff, c_diff); st != GRAFT_OK) return st;
        if (graft_status st = compress(cctx.get(), b.extra, c_extra); st != GRAFT_OK) return st;
        bh.clear();
        bh.push_back(kTagBlock);
        put_uvarint(bh, b.ctrl.size());
        put_uvarint(bh, c_ctrl.size());
        put_uvarint(bh, b.diff.size());
        put_uvarint(bh, c_diff.size());
        put_uvarint(bh, b.extra.size());
        put_uvarint(bh, c_extra.size());
        for (const auto *v : {&bh, &c_ctrl, &c_diff, &c_extra})
            if (graft_status st = out.write(v->data(), v->size()); st != GRAFT_OK) return st;
    }

    const uint8_t end = kTagEnd;
    if (graft_status st = out.write(&end, 1); st != GRAFT_OK) return st;
    uint8_t ph[GRAFT_HASH_SIZE];
    out.digest(ph);
    return out.write(ph, sizeof ph);
}

}  // namespace
}  // namespace graft

extern "C" void graft_diff_options_init(graft_diff_options *o) {
    if (!o) return;
    o->struct_size = sizeof *o;
    o->level = GRAFT_DEFAULT_LEVEL;
    o->block_size = GRAFT_DEFAULT_BLOCK_SIZE;
    o->masks = nullptr;
    o->mask_count = 0;
    o->filter = GRAFT_FILTER_AUTO;
    o->method = GRAFT_METHOD_AUTO;
}

extern "C" uint8_t graft_filter_detect(const void *data, size_t len) {
    if (!data) return GRAFT_FILTER_NONE;
    return graft::filter_detect(static_cast<const uint8_t *>(data), len);
}

extern "C" graft_status graft_diff(const void *source, size_t source_len, const void *target, size_t target_len,
                                   const graft_diff_options *opts, const graft_sink *out) {
    if ((!source && source_len) || (!target && target_len) || !out || !out->write) return GRAFT_E_INVALID_ARG;
    graft_diff_options o;
    graft_diff_options_init(&o);
    if (opts) {
        if (opts->struct_size < sizeof o) return GRAFT_E_INVALID_ARG;
        o = *opts;
    }
    if (o.mask_count > graft::kMaxMasks || (o.mask_count && !o.masks)) return GRAFT_E_INVALID_ARG;
    if (o.filter != GRAFT_FILTER_AUTO && o.filter != GRAFT_FILTER_NONE && !graft::filter_known(o.filter))
        return GRAFT_E_INVALID_ARG;
    if (o.level < 1 || o.level > ZSTD_maxCLevel()) return GRAFT_E_INVALID_ARG;
    if (o.method > GRAFT_METHOD_ROLLING) return GRAFT_E_INVALID_ARG;
    if (o.block_size < GRAFT_MIN_BLOCK_SIZE || o.block_size > GRAFT_MAX_BLOCK_SIZE) return GRAFT_E_INVALID_ARG;
    try {
        std::vector<graft::Mask> masks;
        for (size_t i = 0; i < o.mask_count; ++i) {
            const graft_mask &m = o.masks[i];
            masks.push_back({m.old_offset, m.old_length, m.new_offset, m.new_length, m.mode, m.pad});
        }
        std::sort(masks.begin(), masks.end(),
                  [](const graft::Mask &a, const graft::Mask &b) { return a.old_offset < b.old_offset; });
        if (!masks.empty() && !graft::masks_valid(masks, source_len, target_len)) return GRAFT_E_INVALID_ARG;
        const auto *s = static_cast<const uint8_t *>(source);
        const auto *t = static_cast<const uint8_t *>(target);
        if (o.filter != GRAFT_FILTER_AUTO) return graft::diff_impl(s, source_len, t, target_len, o, masks, o.filter, out);
        // AUTO: if the header suggests a filter, encode both ways and keep the smaller.
        const uint8_t guess = graft::filter_detect(t, target_len);
        if (guess == GRAFT_FILTER_NONE) return graft::diff_impl(s, source_len, t, target_len, o, masks, 0, out);
        std::vector<uint8_t> a, b;
        graft_sink sa{&a, [](void *ctx, const void *p, size_t n) {
                          auto *v = static_cast<std::vector<uint8_t> *>(ctx);
                          v->insert(v->end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
                          return GRAFT_OK;
                      }};
        graft_sink sb{&b, sa.write};
        if (graft_status st = graft::diff_impl(s, source_len, t, target_len, o, masks, 0, &sa); st != GRAFT_OK)
            return st;
        if (graft_status st = graft::diff_impl(s, source_len, t, target_len, o, masks, guess, &sb); st != GRAFT_OK)
            return st;
        const auto &best = b.size() < a.size() ? b : a;
        return out->write(out->ctx, best.data(), best.size());
    } catch (const std::bad_alloc &) {
        return GRAFT_E_NOMEM;
    } catch (const std::length_error &) {
        return GRAFT_E_NOMEM;
    }
}
