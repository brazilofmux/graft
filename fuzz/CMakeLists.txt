# graft_fuzz_target(<name> <source>): a libFuzzer binary named fuzz_<name>.
# Run: build/fuzz/fuzz/fuzz_<name> fuzz/corpus/<name> -max_total_time=60
function(graft_fuzz_target name src)
  add_executable(fuzz_${name} ${src})
  target_link_libraries(fuzz_${name} PRIVATE graft::graft)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  graft_warnings(fuzz_${name})
endfunction()

# M0: a harness that exercises the pipeline. Real targets (patch decoder,
# archive reader, mask spec parser) are added with those parsers.
graft_fuzz_target(hash fuzz_hash.cpp)
