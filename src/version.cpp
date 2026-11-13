#include "graft/graft.h"
#include "build_info.h"

#include <blake3.h>
#include <libsais.h>
#include <zstd.h>

#include <string>

static_assert(GRAFT_VERSION_MAJOR == 0 && GRAFT_VERSION_MINOR == 1 && GRAFT_VERSION_PATCH == 0,
              "graft.h version macros out of sync with CMake project version");

extern "C" const char *graft_version(void) { return GRAFT_VERSION_STRING; }

extern "C" const char *graft_dependency_versions(void) {
    static const std::string s = std::string("zstd ") + ZSTD_versionString() +
                                 ", libsais " + LIBSAIS_VERSION_STRING +
                                 ", blake3 " + blake3_version();
    return s.c_str();
}
