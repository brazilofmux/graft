// fsutil.h: file-backed sources/readers/sinks, hashing, and path rules shared
// by the package builder and the updater.
#pragma once

#include "graft/graft.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace graft {

namespace fs = std::filesystem;
using Hash = std::array<uint8_t, GRAFT_HASH_SIZE>;
using Regions = std::vector<std::pair<uint64_t, uint64_t>>;  // (offset, length), sorted, disjoint

class File {
public:
    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() { close(); }
    bool open(const fs::path &p, const char *mode);  // fopen-style mode
    bool create(const fs::path &p);  // new file for writing; fails if it exists
    bool close();  // false if the close (flush) failed
    std::FILE *get() const { return f_; }

private:
    std::FILE *f_ = nullptr;
};

bool file_seek(std::FILE *f, uint64_t off);
graft_status read_whole(const fs::path &p, std::vector<uint8_t> &out);
// Hash with every region zeroed (empty regions: the plain hash).
graft_status hash_file(const fs::path &p, const Regions &zeroed, Hash &out, uint64_t *size = nullptr);
void hash_bytes(const uint8_t *p, size_t n, const Regions &zeroed, Hash &out);
graft_status sync_file(std::FILE *f);

// A graft_source over an open file of known size.
struct FileSource {
    std::FILE *f;
    uint64_t size;
    graft_source source();
};

// A graft_reader over [pos, end) of an open file.
struct RangeReader {
    std::FILE *f;
    uint64_t pos, end;
    bool positioned = false;
    graft_reader reader();
};

struct FileSink {
    std::FILE *f;
    graft_sink sink();
};

// Package-relative paths: '/'-separated, no empty/"."/".." components, no
// leading '/', no backslash, no control characters, at most 4096 bytes.
bool valid_rel_path(const std::string &p);
// ASCII case folding, for collision checks on case-insensitive filesystems.
std::string fold_case(const std::string &p);

// Paths inside packages, archives and the C API are UTF-8. These convert
// without going through the ANSI code page, which a narrow std::string path
// would on Windows. u8str gives the generic ('/'-separated) form.
fs::path u8path(const std::string &s);
std::string u8str(const fs::path &p);
inline fs::path join(const fs::path &root, const std::string &rel) { return root / u8path(rel); }

// --- platform rules ------------------------------------------------------------

// Whether this platform keeps POSIX permission bits (not on Windows, where
// comparing them would plan a pointless chmod on every update).
#if defined(_WIN32)
inline constexpr bool kModesSupported = false;
#else
inline constexpr bool kModesSupported = true;
#endif

// Whether a relative path can be created here. On Windows: no reserved
// device names (CON, NUL, COM1, ...), no < > " | ? *, no component ending in
// a dot or space. Always true elsewhere.
bool platform_safe_path(const std::string &rel);

// Whether another process holds the file open (Windows: an exclusive open
// fails with a sharing or lock violation). Always false elsewhere.
bool file_in_use(const fs::path &p);

// Clears read-only attributes under a directory so it can be removed.
void make_tree_writable(const fs::path &dir);

// Reserved directory at the top of an updated tree (journal and staging).
inline constexpr const char *kTxnDir = ".graft-txn";

}  // namespace graft
