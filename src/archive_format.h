// archive_format.h: the .gfa versioned archive (docs/ARCHIVE.md).
#pragma once

#include "package_format.h"  // Writer/Reader, path rules
#include "tree.h"

#include <set>
#include <string>
#include <vector>

namespace graft::arc {

inline constexpr uint8_t kMagic[8] = {0x89, 'G', 'F', 'A', '\r', '\n', 0x1a, '\n'};
inline constexpr uint8_t kEndMagic[8] = {'G', 'F', 'A', 'S', 'E', 'G', 0x00, 0x01};
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kHeaderSize = 12;
inline constexpr size_t kFooterSize = 72;  // offset, comp, raw, prev (u64 each), hash[32], end magic[8]
inline constexpr uint64_t kNoPrev = UINT64_MAX;
inline constexpr uint64_t kMaxSegment = 256ull << 20;
inline constexpr uint32_t kMaxDepth = 64;      // accepted when reading
inline constexpr uint32_t kDefaultDepth = 8;   // used when writing
inline constexpr size_t kMaxLabel = 256;

enum ObjectKind : uint8_t { kObjFull = 0, kObjDelta = 1 };

struct Object {
    Hash hash{};          // of the content
    uint64_t size = 0;    // of the content
    uint8_t kind = kObjFull;
    uint64_t base = 0;    // kObjDelta: an earlier object
    uint64_t offset = 0, length = 0;
    uint32_t depth = 0;   // derived: 0 for full, base depth + 1 for a delta
};

struct VEntry {
    std::string path;
    uint8_t kind = kNodeFile;
    uint32_t mode = 0;
    uint64_t object = 0;  // kNodeFile
    std::string link;     // kNodeSymlink
};

struct Version {
    std::string label;
    uint64_t time = 0;    // seconds since 1970
    std::vector<VEntry> entries;
};

struct Footer {
    uint64_t offset = 0, comp = 0, raw = 0, prev = kNoPrev;
    Hash hash{};
};

inline void encode_footer(const Footer &f, uint8_t out[kFooterSize]) {
    put_le<uint64_t>(out, f.offset);
    put_le<uint64_t>(out + 8, f.comp);
    put_le<uint64_t>(out + 16, f.raw);
    put_le<uint64_t>(out + 24, f.prev);
    std::memcpy(out + 32, f.hash.data(), 32);
    std::memcpy(out + 64, kEndMagic, 8);
}

inline bool decode_footer(const uint8_t in[kFooterSize], Footer &f) {
    if (std::memcmp(in + 64, kEndMagic, 8) != 0) return false;
    f.offset = get_le<uint64_t>(in);
    f.comp = get_le<uint64_t>(in + 8);
    f.raw = get_le<uint64_t>(in + 16);
    f.prev = get_le<uint64_t>(in + 24);
    std::memcpy(f.hash.data(), in + 32, 32);
    return true;
}

inline bool valid_label(const std::string &l) {
    if (l.empty() || l.size() > kMaxLabel) return false;
    for (char ch : l)
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) return false;
    return true;
}

// A segment: the objects one `add` created, then the version it added.
//   segment := n:uv object[n] version
//   object  := hash[32] size:uv kind:u8 [base:uv if delta] offset:uv length:uv
//   version := label:str time:uv n:uv entry[n]
//   entry   := path:str kind:u8 (file: mode:uv object:uv | dir: mode:uv | symlink: link:str)
inline void encode_segment(const std::vector<Object> &objs, const Version &v, std::vector<uint8_t> &out) {
    pkg::Writer w;
    w.uv(objs.size());
    for (const auto &o : objs) {
        w.hash(o.hash);
        w.uv(o.size);
        w.u8(o.kind);
        if (o.kind == kObjDelta) w.uv(o.base);
        w.uv(o.offset);
        w.uv(o.length);
    }
    w.str(v.label);
    w.uv(v.time);
    w.uv(v.entries.size());
    for (const auto &e : v.entries) {
        w.str(e.path);
        w.u8(e.kind);
        if (e.kind == kNodeFile) {
            w.uv(e.mode);
            w.uv(e.object);
        } else if (e.kind == kNodeDir) {
            w.uv(e.mode);
        } else {
            w.str(e.link);
        }
    }
    out = std::move(w.out);
}

// Decodes one segment, appending its objects to `objs` (whose existing size
// is the first new object's id). Validates everything that can be checked
// without the data: references, depths, data bounds, paths, and the shape of
// the tree (no entry below a non-directory, no case collisions), so that
// extracting a version can't escape its directory.
inline bool decode_segment(const uint8_t *p, size_t n, std::vector<Object> &objs, Version &v, uint64_t data_end,
                           size_t header_size = kHeaderSize) {
    pkg::Reader r(p, n);
    uint64_t count;
    if (!r.count(count, 36)) return false;
    const uint64_t first = objs.size();
    for (uint64_t i = 0; i < count; ++i) {
        Object o;
        if (!r.hash(o.hash) || !r.uv(o.size) || !r.u8(o.kind)) return false;
        if (o.kind == kObjDelta) {
            if (!r.uv(o.base) || o.base >= first + i) return false;
            o.depth = objs[static_cast<size_t>(o.base)].depth + 1;
            if (o.depth > kMaxDepth) return false;
        } else if (o.kind != kObjFull) {
            return false;
        }
        if (!r.uv(o.offset) || !r.uv(o.length)) return false;
        if (o.offset < header_size || o.offset > data_end || o.length > data_end - o.offset) return false;
        objs.push_back(o);
    }
    if (!r.str(v.label, kMaxLabel) || !valid_label(v.label) || !r.uv(v.time) || !r.count(count, 2)) return false;
    v.entries.resize(static_cast<size_t>(count));
    for (auto &e : v.entries) {
        uint64_t mode;
        if (!r.str(e.path) || !valid_rel_path(e.path) || !r.u8(e.kind)) return false;
        if (e.path == kTxnDir || e.path.rfind(std::string(kTxnDir) + "/", 0) == 0) return false;
        if (e.kind == kNodeFile) {
            if (!r.uv(mode) || mode > 07777 || !r.uv(e.object) || e.object >= objs.size()) return false;
            e.mode = static_cast<uint32_t>(mode);
        } else if (e.kind == kNodeDir) {
            if (!r.uv(mode) || mode > 07777) return false;
            e.mode = static_cast<uint32_t>(mode);
        } else if (e.kind == kNodeSymlink) {
            if (!r.str(e.link) || e.link.empty()) return false;
        } else {
            return false;
        }
    }
    if (!r.done()) return false;

    // Sorted, unique; every parent present and a directory; no case collisions.
    std::set<std::string> dirs, folded;
    for (size_t i = 0; i < v.entries.size(); ++i) {
        const VEntry &e = v.entries[i];
        if (i > 0 && !(v.entries[i - 1].path < e.path)) return false;
        const auto slash = e.path.rfind('/');
        if (slash != std::string::npos && !dirs.count(e.path.substr(0, slash))) return false;
        if (e.kind == kNodeDir) dirs.insert(e.path);
        if (!folded.insert(fold_case(e.path)).second) return false;
    }
    return true;
}

}  // namespace graft::arc
