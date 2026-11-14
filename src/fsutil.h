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
    bool open(const fs::path &p, const char *mode);
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

// Reserved directory at the top of an updated tree (journal and staging).
inline constexpr const char *kTxnDir = ".graft-txn";

}  // namespace graft
