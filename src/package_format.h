// package_format.h: the .gfp container and its manifest (docs/PACKAGE.md).
#pragma once

#include "format.h"
#include "fsutil.h"

#include <string>
#include <vector>

namespace graft::pkg {

inline constexpr uint8_t kMagic[8] = {0x89, 'G', 'F', 'P', '\r', '\n', 0x1a, '\n'};
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kHeaderSize = 12;  // magic, version:u16, flags:u16
inline constexpr size_t kFooterSize = 24;  // manifest_offset, manifest_comp, manifest_raw (u64 each)
inline constexpr uint64_t kMaxManifest = 256ull << 20;
inline constexpr size_t kMaxString = 4096;

enum EntryKind : uint8_t { kFile = 0, kDir = 1, kSymlink = 2, kRemove = 3 };
enum RecipeKind : uint8_t { kDelta = 0, kFull = 1 };
enum BlobKind : uint8_t { kBlobGft = 0, kBlobZstd = 1 };

struct Blob {
    uint8_t kind = kBlobGft;
    uint64_t offset = 0, length = 0;
    uint64_t raw_size = 0;  // kBlobZstd: decompressed size
};

struct Recipe {
    uint8_t kind = kDelta;
    std::string source_path;      // kDelta: file in the installed tree to patch
    uint64_t blob = 0;
    std::vector<uint64_t> labels;  // versions this recipe serves (indices into labels)
};

struct Entry {
    std::string path;
    uint8_t kind = kFile;
    // kFile
    uint32_t mode = 0644;
    uint64_t size = 0;
    Hash current{};        // hash of the target file with `regions` zeroed
    Regions regions;       // masked regions of the target file
    std::vector<Recipe> recipes;
    // kDir: mode. kSymlink: link + known (old links it may replace).
    std::string link;
    std::vector<std::string> known_links;
    // kRemove: what (kFile/kDir/kSymlink) and the contents it may remove.
    uint8_t what = kFile;
    std::vector<Hash> known_hashes;
};

struct Manifest {
    std::string target_label;
    std::vector<std::string> labels;  // old versions
    std::vector<Blob> blobs;
    std::vector<Entry> entries;       // sorted: by path, a kRemove before a create
};

// --- encoding --------------------------------------------------------------------

class Writer {
public:
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void uv(uint64_t v) { put_uvarint(out, v); }
    void str(const std::string &s) {
        uv(s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    void hash(const Hash &h) { out.insert(out.end(), h.begin(), h.end()); }
};

inline void encode_manifest(const Manifest &m, std::vector<uint8_t> &out) {
    Writer w;
    w.str(m.target_label);
    w.uv(m.labels.size());
    for (const auto &l : m.labels) w.str(l);
    w.uv(m.blobs.size());
    for (const auto &b : m.blobs) {
        w.u8(b.kind);
        w.uv(b.offset);
        w.uv(b.length);
        w.uv(b.raw_size);
    }
    w.uv(m.entries.size());
    for (const auto &e : m.entries) {
        w.str(e.path);
        w.u8(e.kind);
        switch (e.kind) {
        case kFile:
            w.uv(e.mode);
            w.uv(e.size);
            w.hash(e.current);
            w.uv(e.regions.size());
            for (const auto &[o, l] : e.regions) {
                w.uv(o);
                w.uv(l);
            }
            w.uv(e.recipes.size());
            for (const auto &r : e.recipes) {
                w.u8(r.kind);
                if (r.kind == kDelta) w.str(r.source_path);
                w.uv(r.blob);
                w.uv(r.labels.size());
                for (uint64_t l : r.labels) w.uv(l);
            }
            break;
        case kDir: w.uv(e.mode); break;
        case kSymlink:
            w.str(e.link);
            w.uv(e.known_links.size());
            for (const auto &k : e.known_links) w.str(k);
            break;
        case kRemove:
            w.u8(e.what);
            if (e.what == kFile) {
                w.uv(e.known_hashes.size());
                for (const auto &h : e.known_hashes) w.hash(h);
            } else if (e.what == kSymlink) {
                w.uv(e.known_links.size());
                for (const auto &k : e.known_links) w.str(k);
            }
            break;
        default: break;
        }
    }
    out = std::move(w.out);
}

// --- decoding (hostile input) ----------------------------------------------------

class Reader {
public:
    Reader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}
    bool u8(uint8_t &v) {
        if (p_ == end_) return false;
        v = *p_++;
        return true;
    }
    bool uv(uint64_t &v) { return get_uvarint(p_, end_, v); }
    // A count of items, each at least `min_size` bytes: bounded by what's left.
    bool count(uint64_t &v, size_t min_size = 1) {
        return uv(v) && v <= static_cast<uint64_t>(end_ - p_) / min_size;
    }
    bool str(std::string &s, size_t max = kMaxString) {
        uint64_t n;
        if (!uv(n) || n > max || n > static_cast<uint64_t>(end_ - p_)) return false;
        s.assign(reinterpret_cast<const char *>(p_), static_cast<size_t>(n));
        p_ += n;
        return true;
    }
    bool hash(Hash &h) {
        if (static_cast<size_t>(end_ - p_) < h.size()) return false;
        std::memcpy(h.data(), p_, h.size());
        p_ += h.size();
        return true;
    }
    bool done() const { return p_ == end_; }

private:
    const uint8_t *p_, *end_;
};

inline int kind_order(uint8_t kind) { return kind == kRemove ? 0 : 1; }

// Structural and semantic validation. Blob ranges are checked against the
// container separately (validate_blobs).
inline bool decode_manifest(const uint8_t *p, size_t n, Manifest &m) {
    Reader r(p, n);
    uint64_t count;
    if (!r.str(m.target_label) || !r.count(count)) return false;
    m.labels.resize(static_cast<size_t>(count));
    for (auto &l : m.labels)
        if (!r.str(l)) return false;
    if (!r.count(count, 4)) return false;
    m.blobs.resize(static_cast<size_t>(count));
    for (auto &b : m.blobs) {
        if (!r.u8(b.kind) || !r.uv(b.offset) || !r.uv(b.length) || !r.uv(b.raw_size)) return false;
        if (b.kind != kBlobGft && b.kind != kBlobZstd) return false;
    }
    if (!r.count(count, 2)) return false;
    m.entries.resize(static_cast<size_t>(count));
    for (auto &e : m.entries) {
        if (!r.str(e.path) || !r.u8(e.kind) || !valid_rel_path(e.path)) return false;
        if (e.path == kTxnDir || e.path.rfind(std::string(kTxnDir) + "/", 0) == 0) return false;
        uint64_t v;
        switch (e.kind) {
        case kFile: {
            if (!r.uv(v) || v > 07777) return false;
            e.mode = static_cast<uint32_t>(v);
            if (!r.uv(e.size) || !r.hash(e.current) || !r.count(count, 2)) return false;
            e.regions.resize(static_cast<size_t>(count));
            uint64_t prev_end = 0;
            for (auto &[o, l] : e.regions) {
                if (!r.uv(o) || !r.uv(l) || l == 0 || o < prev_end || o > e.size || l > e.size - o) return false;
                prev_end = o + l;
            }
            if (!r.count(count, 3)) return false;
            e.recipes.resize(static_cast<size_t>(count));
            for (auto &rc : e.recipes) {
                if (!r.u8(rc.kind)) return false;
                if (rc.kind == kDelta) {
                    if (!r.str(rc.source_path) || !valid_rel_path(rc.source_path)) return false;
                } else if (rc.kind != kFull) {
                    return false;
                }
                if (!r.uv(rc.blob) || rc.blob >= m.blobs.size() || !r.count(count)) return false;
                const uint8_t want = rc.kind == kDelta ? kBlobGft : kBlobZstd;
                if (m.blobs[static_cast<size_t>(rc.blob)].kind != want) return false;
                rc.labels.resize(static_cast<size_t>(count));
                for (auto &l : rc.labels)
                    if (!r.uv(l) || l >= m.labels.size()) return false;
            }
            break;
        }
        case kDir:
            if (!r.uv(v) || v > 07777) return false;
            e.mode = static_cast<uint32_t>(v);
            break;
        case kSymlink:
            if (!r.str(e.link) || e.link.empty() || !r.count(count)) return false;
            e.known_links.resize(static_cast<size_t>(count));
            for (auto &k : e.known_links)
                if (!r.str(k)) return false;
            break;
        case kRemove:
            if (!r.u8(e.what)) return false;
            if (e.what == kFile) {
                if (!r.count(count, GRAFT_HASH_SIZE)) return false;
                e.known_hashes.resize(static_cast<size_t>(count));
                for (auto &h : e.known_hashes)
                    if (!r.hash(h)) return false;
            } else if (e.what == kSymlink) {
                if (!r.count(count)) return false;
                e.known_links.resize(static_cast<size_t>(count));
                for (auto &k : e.known_links)
                    if (!r.str(k)) return false;
            } else if (e.what != kDir) {
                return false;
            }
            break;
        default: return false;
        }
    }
    if (!r.done()) return false;

    // Canonical order: by path, a removal before the creation at the same
    // path; nothing else may share a path. No two paths may differ only by
    // case (they would collide on case-insensitive filesystems).
    for (size_t i = 1; i < m.entries.size(); ++i) {
        const Entry &a = m.entries[i - 1], &b = m.entries[i];
        if (a.path > b.path) return false;
        if (a.path == b.path && !(a.kind == kRemove && b.kind != kRemove)) return false;
    }
    std::vector<std::pair<std::string, const std::string *>> folded;
    folded.reserve(m.entries.size());
    for (const auto &e : m.entries) folded.emplace_back(fold_case(e.path), &e.path);
    std::sort(folded.begin(), folded.end());
    for (size_t i = 1; i < folded.size(); ++i)
        if (folded[i].first == folded[i - 1].first && *folded[i].second != *folded[i - 1].second) return false;
    return true;
}

// Every blob lies within the blob area [kHeaderSize, area_end).
inline bool validate_blobs(const Manifest &m, uint64_t area_end) {
    for (const auto &b : m.blobs)
        if (b.offset < kHeaderSize || b.offset > area_end || b.length > area_end - b.offset) return false;
    return true;
}

}  // namespace graft::pkg
