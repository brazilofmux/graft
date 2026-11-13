cmake_minimum_required(VERSION 3.24)

project(graft
  VERSION 0.0.1
  DESCRIPTION "Binary patching: deltas, masked/multi-source patch packages, versioned archives"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- Options -----------------------------------------------------------------
set(GRAFT_DEPS "auto" CACHE STRING
  "Where dependencies come from: auto (installed, else download), system (installed only), fetch (always download)")
set_property(CACHE GRAFT_DEPS PROPERTY STRINGS auto system fetch)
option(GRAFT_WERROR   "Treat warnings as errors" OFF)
option(GRAFT_SANITIZE "Build with ASan + UBSan" OFF)
option(GRAFT_FUZZ     "Build libFuzzer targets (needs a clang with libFuzzer)" OFF)
option(GRAFT_TESTS    "Build tests" ON)
option(GRAFT_TOOLS    "Build the graft CLI" ON)

include(cmake/Warnings.cmake)
include(cmake/Deps.cmake)

if(GRAFT_SANITIZE OR GRAFT_FUZZ)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()
if(GRAFT_FUZZ)
  # Instrument everything for coverage; only fuzz targets link the libFuzzer main.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_subdirectory(src)
if(GRAFT_TOOLS)
  add_subdirectory(tools)
endif()
if(GRAFT_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(GRAFT_FUZZ)
  add_subdirectory(fuzz)
endif()
