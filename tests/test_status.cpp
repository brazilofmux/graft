#include "check.h"
#include "graft/graft.h"

#include <cstring>
#include <string>

TEST(status_strings_are_distinct_and_nonnull) {
    const graft_status all[] = {GRAFT_OK,       GRAFT_E_INVALID_ARG, GRAFT_E_NOMEM,        GRAFT_E_IO,
                                GRAFT_E_CORRUPT, GRAFT_E_LIMIT,       GRAFT_E_WRONG_SOURCE, GRAFT_E_UNSUPPORTED, GRAFT_E_CONFLICT};
    for (auto a : all) {
        CHECK(graft_status_string(a) != nullptr);
        CHECK(std::strcmp(graft_status_string(a), "unknown status") != 0);
        for (auto b : all)
            if (a != b) CHECK(std::strcmp(graft_status_string(a), graft_status_string(b)) != 0);
    }
    CHECK(std::strcmp(graft_status_string(static_cast<graft_status>(9999)), "unknown status") == 0);
}

TEST(versions_are_populated) {
    CHECK_EQ(std::string(graft_version()), std::string("0.1.0"));
    const std::string deps = graft_dependency_versions();
    CHECK(deps.find("zstd ") != std::string::npos);
    CHECK(deps.find("libsais ") != std::string::npos);
    CHECK(deps.find("blake3 ") != std::string::npos);
}
