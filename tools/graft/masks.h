// masks.h: parse mask specs and resolve them against the old and new files.
//
// A spec is comma-separated; exactly one way of locating the region:
//
//   at=OFF:LEN                 same offset in both files
//   old=OFF:LEN,new=OFF:LEN    explicit offsets in each
//   marker=TEXT,len=N[,skip=K] region starts K bytes after the start of a
//   marker=hex:HEX,len=N       marker that occurs exactly once in each file
//                              (K defaults to the marker's length)
//
// plus, when the region changes size:  newlen=N with pad=BYTE or truncate
//
// Numbers accept 0x prefixes. Use hex: for markers containing commas.
#pragma once

#include "graft/graft.h"

#include <string>
#include <vector>

namespace cli {

// Resolves `spec` against old/new. On failure returns false and sets `err`.
bool resolve_mask(const std::string &spec, const uint8_t *oldp, size_t oldn, const uint8_t *newp, size_t newn,
                  graft_mask &out, std::string &err);

// Reads specs from a file: one per line, '#' starts a comment.
bool read_mask_file(const char *path, std::vector<std::string> &specs, std::string &err);

}  // namespace cli
