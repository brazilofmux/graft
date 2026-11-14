// The package manifest parser against arbitrary bytes. (In a real update the
// whole-package hash is checked first, but a hostile package can carry a
// valid hash, so the parser must stand on its own.) Anything it accepts must
// re-encode to the same bytes: one canonical encoding per manifest.
#include "package_format.h"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    graft::pkg::Manifest m;
    if (!graft::pkg::decode_manifest(data, size, m)) return 0;
    std::vector<uint8_t> again;
    graft::pkg::encode_manifest(m, again);
    if (again.size() != size || std::memcmp(again.data(), data, size) != 0) std::abort();
    return 0;
}
