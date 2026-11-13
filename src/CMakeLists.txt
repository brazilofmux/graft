configure_file(build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/build_info.h @ONLY)

add_library(graft
  version.cpp
  status.cpp
  hash.cpp)
add_library(graft::graft ALIAS graft)

target_include_directories(graft
  PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(graft PRIVATE GRAFT_BUILDING)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(graft PUBLIC GRAFT_SHARED)
endif()
set_target_properties(graft PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

target_link_libraries(graft PRIVATE graft::zstd graft::sais graft::blake3)
graft_warnings(graft)
