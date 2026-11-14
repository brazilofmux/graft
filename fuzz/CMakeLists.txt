# graft_fuzz_target(<name> <source>): a libFuzzer binary named fuzz_<name>.
# Run: build/fuzz/fuzz/fuzz_<name> fuzz/corpus/<name> -max_total_time=60
function(graft_fuzz_target name src)
  add_executable(fuzz_${name} ${src})
  target_link_libraries(fuzz_${name} PRIVATE graft::graft)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  graft_warnings(fuzz_${name})
endfunction()

graft_fuzz_target(hash fuzz_hash.cpp)
graft_fuzz_target(apply fuzz_apply.cpp)
graft_fuzz_target(roundtrip fuzz_roundtrip.cpp)
graft_fuzz_target(manifest fuzz_manifest.cpp)
target_include_directories(fuzz_manifest PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Seed generator for fuzz_apply (an ordinary program, no libFuzzer main).
add_executable(fuzz_mkseeds fuzz_mkseeds.cpp)
target_link_libraries(fuzz_mkseeds PRIVATE graft::graft)
graft_warnings(fuzz_mkseeds)
