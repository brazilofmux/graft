#include "graft/graft.h"

#include <cstring>

namespace {

graft_status memory_read_at(void *ctx, uint64_t offset, void *buf, size_t len) {
    // The library never reads past graft_source::size, so no bounds check here.
    if (len) std::memcpy(buf, static_cast<const unsigned char *>(ctx) + offset, len);
    return GRAFT_OK;
}

}  // namespace

extern "C" graft_source graft_source_from_memory(const void *data, size_t len) {
    graft_source s;
    s.ctx = const_cast<void *>(data);  // read-only use; the struct field is shared with writable sources
    s.size = len;
    s.read_at = memory_read_at;
    return s;
}
