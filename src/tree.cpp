#include "tree.h"

namespace graft {

uint32_t file_mode(const fs::file_status &st) {
#if defined(_WIN32)
    return fs::is_directory(st) ? 0755 : 0644;
#else
    return static_cast<uint32_t>(st.permissions() & fs::perms::mask) & 07777;
#endif
}

graft_status scan_tree(const fs::path &root, Tree &tree, const Log &log) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        log(GRAFT_LOG_ERROR, root.string() + ": not a directory");
        return GRAFT_E_INVALID_ARG;
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) return GRAFT_E_IO;
    for (; it != end; it.increment(ec)) {
        if (ec) return GRAFT_E_IO;
        const fs::path &p = it->path();
        const std::string rel = p.lexically_relative(root).generic_string();
        if (rel == kTxnDir) {
            it.disable_recursion_pending();
            continue;
        }
        if (!valid_rel_path(rel)) {
            log(GRAFT_LOG_ERROR, rel + ": path can't be stored (reserved characters)");
            return GRAFT_E_INVALID_ARG;
        }
        const fs::file_status st = it->symlink_status(ec);
        if (ec) return GRAFT_E_IO;
        Node n;
        if (fs::is_symlink(st)) {
            n.kind = kNodeSymlink;
            n.link = fs::read_symlink(p, ec).generic_string();
            if (ec) return GRAFT_E_IO;
        } else if (fs::is_directory(st)) {
            n.kind = kNodeDir;
            n.mode = file_mode(st);
        } else if (fs::is_regular_file(st)) {
            n.kind = kNodeFile;
            n.mode = file_mode(st);
            if (graft_status s = hash_file(p, {}, n.hash, &n.size); s != GRAFT_OK) return s;
        } else {
            log(GRAFT_LOG_WARN, rel + ": not a file, directory or symlink; skipped");
            continue;
        }
        tree.emplace(rel, std::move(n));
    }
    return GRAFT_OK;
}

}  // namespace graft
