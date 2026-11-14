// The archive segment parser against arbitrary bytes. Anything it accepts
// must re-encode to the same bytes.
#include "archive_format.h"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::vector<graft::arc::Object> objs;
    graft::arc::Version v;
    if (!graft::arc::decode_segment(data, size, objs, v, UINT64_MAX)) return 0;
    std::vector<uint8_t> again;
    graft::arc::encode_segment(objs, v, again);
    if (again.size() != size || std::memcmp(again.data(), data, size) != 0) std::abort();
    return 0;
}
