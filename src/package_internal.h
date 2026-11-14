// package_internal.h: test hooks for the updater. Not part of the public API.
#pragma once

#include "package_format.h"

#include <cstddef>

namespace graft::pkg {

// Called before each commit step (phase "commit") and after each staged file
// (phase "stage"). Return 0 to continue, 1 to fail the step (exercises
// rollback), 2 to stop dead as if the process crashed (exercises recovery).
using FaultHook = int (*)(const char *phase, size_t step);
extern FaultHook g_fault_hook;

struct FaultCrash {};

}  // namespace graft::pkg
