// tree.h: scanning a directory tree, shared by packages and archives.
#pragma once

#include "fsutil.h"

#include <map>
#include <string>

namespace graft {

enum NodeKind : uint8_t { kNodeFile = 0, kNodeDir = 1, kNodeSymlink = 2 };

struct Node {
    uint8_t kind = kNodeFile;
    uint32_t mode = 0;
    uint64_t size = 0;
    Hash hash{};
    std::string link;
};
using Tree = std::map<std::string, Node>;  // relative '/'-separated path -> node

class Log {
public:
    Log(graft_log_fn log_fn, void *log_ctx) : fn_(log_fn), ctx_(log_ctx) {}
    void operator()(int level, const std::string &msg) const {
        if (fn_) fn_(ctx_, level, msg.c_str());
    }
    graft_log_fn fn() const { return fn_; }
    void *ctx() const { return ctx_; }

private:
    graft_log_fn fn_;
    void *ctx_;
};

// Regular files, directories and symlinks under root (not following links);
// skips the reserved .graft-txn and warns about anything else.
graft_status scan_tree(const fs::path &root, Tree &tree, const Log &log);

uint32_t file_mode(const fs::file_status &st);

}  // namespace graft
