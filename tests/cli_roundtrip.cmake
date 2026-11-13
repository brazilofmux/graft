# cmake -DGRAFT=<graft binary> -DWORK=<scratch dir> -P cli_roundtrip.cmake
#
# End to end through the CLI: diff, info, verify, apply, and rejection of a
# wrong source without leaving an output file. (Corruption is covered
# byte-by-byte in the unit tests.)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

string(REPEAT "the quick brown fox jumps over the lazy dog. " 2000 base)
file(WRITE "${WORK}/old" "${base}")
string(REPLACE "lazy dog. the quick" "lazy cat. a quick" changed "${base}")
file(WRITE "${WORK}/new" "${changed}MORE AT THE END")
file(WRITE "${WORK}/other" "${base}x")

function(run expect_ok)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(expect_ok AND NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  elseif(NOT expect_ok AND rc EQUAL 0)
    message(FATAL_ERROR "unexpectedly succeeded: ${ARGN}\n${out}${err}")
  endif()
  set(last_out "${out}" PARENT_SCOPE)
endfunction()

run(TRUE  "${GRAFT}" diff -l 9 -b 4096 "${WORK}/old" "${WORK}/new" "${WORK}/p.gft")
run(TRUE  "${GRAFT}" info "${WORK}/p.gft")
if(NOT last_out MATCHES "format: +gft v1")
  message(FATAL_ERROR "info output unexpected:\n${last_out}")
endif()
run(TRUE  "${GRAFT}" verify "${WORK}/old" "${WORK}/p.gft")
run(TRUE  "${GRAFT}" apply "${WORK}/old" "${WORK}/p.gft" "${WORK}/out")
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/new" "${WORK}/out" RESULT_VARIABLE same)
if(NOT same EQUAL 0)
  message(FATAL_ERROR "apply output differs from NEW")
endif()

run(FALSE "${GRAFT}" apply "${WORK}/other" "${WORK}/p.gft" "${WORK}/out2")
if(EXISTS "${WORK}/out2" OR EXISTS "${WORK}/out2.graft-tmp")
  message(FATAL_ERROR "failed apply left output behind")
endif()

run(FALSE "${GRAFT}" verify "${WORK}/other" "${WORK}/p.gft")
