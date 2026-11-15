# Dependencies, normalized to three targets regardless of where they came from:
#   graft::zstd    graft::sais    graft::blake3
#
# GRAFT_DEPS=auto   use an installed package (CMake config), else download
# GRAFT_DEPS=system installed packages only; fail if missing
# GRAFT_DEPS=fetch  always download the pinned release below
#
# Downloads are pinned by version and SHA-256.

include(FetchContent)

set(GRAFT_ZSTD_VERSION   1.5.7)
set(GRAFT_ZSTD_URL       https://github.com/facebook/zstd/releases/download/v${GRAFT_ZSTD_VERSION}/zstd-${GRAFT_ZSTD_VERSION}.tar.gz)
set(GRAFT_ZSTD_SHA256    eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

set(GRAFT_SAIS_VERSION   2.12.2)
set(GRAFT_SAIS_URL       https://github.com/IlyaGrebnov/libsais/archive/refs/tags/v${GRAFT_SAIS_VERSION}.tar.gz)
set(GRAFT_SAIS_SHA256    9a3ab90beb94ef4d9cf633f39f3d45fe355401dcffcda067a4e330895afc20d7)

set(GRAFT_BLAKE3_VERSION 1.8.7)
set(GRAFT_BLAKE3_URL     https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/${GRAFT_BLAKE3_VERSION}.tar.gz)
set(GRAFT_BLAKE3_SHA256  c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c)

if(NOT GRAFT_DEPS MATCHES "^(auto|system|fetch)$")
  message(FATAL_ERROR "GRAFT_DEPS must be auto, system or fetch (got '${GRAFT_DEPS}')")
endif()

# _graft_try_system(<package>) sets _found if an installed config package is usable.
macro(_graft_try_system pkg)
  set(_found FALSE)
  if(NOT GRAFT_DEPS STREQUAL "fetch")
    find_package(${pkg} CONFIG QUIET)
    if(${pkg}_FOUND)
      set(_found TRUE)
    elseif(GRAFT_DEPS STREQUAL "system")
      message(FATAL_ERROR "GRAFT_DEPS=system but package '${pkg}' was not found")
    endif()
  endif()
endmacro()

# --- zstd --------------------------------------------------------------------
_graft_try_system(zstd)
if(_found)
  if(TARGET zstd::libzstd_static)
    set(_zstd_target zstd::libzstd_static)
  else()
    set(_zstd_target zstd::libzstd)
  endif()
  set(GRAFT_ZSTD_SOURCE "system ${zstd_VERSION}")
  set(_zstd_include "")
else()
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
  if(MSVC)
    set(ZSTD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)  # match CMAKE_MSVC_RUNTIME_LIBRARY in the windows preset
  endif()
  FetchContent_Declare(graft_zstd
    URL ${GRAFT_ZSTD_URL} URL_HASH SHA256=${GRAFT_ZSTD_SHA256}
    SOURCE_SUBDIR build/cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(graft_zstd)
  set(_zstd_target libzstd_static)
  set(_zstd_include ${graft_zstd_SOURCE_DIR}/lib)
  set(GRAFT_ZSTD_SOURCE "fetched ${GRAFT_ZSTD_VERSION}")
endif()
add_library(graft::zstd INTERFACE IMPORTED GLOBAL)
target_link_libraries(graft::zstd INTERFACE ${_zstd_target})
if(_zstd_include)
  # Belt and braces: zstd's CMake doesn't always export its header dir to subprojects.
  target_include_directories(graft::zstd SYSTEM INTERFACE ${_zstd_include})
endif()

# --- libsais -----------------------------------------------------------------
_graft_try_system(libsais)
if(_found)
  set(GRAFT_SAIS_SOURCE "system ${libsais_VERSION}")
else()
  set(LIBSAIS_USE_OPENMP        OFF CACHE BOOL "" FORCE)
  set(LIBSAIS_BUILD_SHARED_LIB  OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(graft_sais
    URL ${GRAFT_SAIS_URL} URL_HASH SHA256=${GRAFT_SAIS_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(graft_sais)
  set(GRAFT_SAIS_SOURCE "fetched ${GRAFT_SAIS_VERSION}")
endif()
add_library(graft::sais INTERFACE IMPORTED GLOBAL)
target_link_libraries(graft::sais INTERFACE libsais)

# --- BLAKE3 ------------------------------------------------------------------
_graft_try_system(blake3)
if(_found)
  set(GRAFT_BLAKE3_SOURCE "system ${blake3_VERSION}")
else()
  set(BLAKE3_USE_TBB OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(graft_blake3
    URL ${GRAFT_BLAKE3_URL} URL_HASH SHA256=${GRAFT_BLAKE3_SHA256}
    SOURCE_SUBDIR c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL SYSTEM)
  FetchContent_MakeAvailable(graft_blake3)
  set(GRAFT_BLAKE3_SOURCE "fetched ${GRAFT_BLAKE3_VERSION}")
endif()
add_library(graft::blake3 INTERFACE IMPORTED GLOBAL)
target_link_libraries(graft::blake3 INTERFACE BLAKE3::blake3)

message(STATUS "graft deps: zstd ${GRAFT_ZSTD_SOURCE}, libsais ${GRAFT_SAIS_SOURCE}, blake3 ${GRAFT_BLAKE3_SOURCE}")
