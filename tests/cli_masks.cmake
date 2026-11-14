# cmake -DGRAFT=<graft binary> -DWORK=<scratch dir> -P cli_masks.cmake
#
# The serial-number scenario through the CLI: the vendor builds a patch from
# their placeholder-stamped v1 and v2 (the field moves in v2), and a
# customer's install keeps its own serial.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

string(REPEAT "code code code " 500 code)
file(WRITE "${WORK}/v1"        "${code}SERIAL:0000-0000-0000${code}")
file(WRITE "${WORK}/v2"        "${code}MORE CODE IN V2 SERIAL:0000-0000-0000${code}tail")
file(WRITE "${WORK}/installed" "${code}SERIAL:QWER-7788-ZXCV${code}")
file(WRITE "${WORK}/expected"  "${code}MORE CODE IN V2 SERIAL:QWER-7788-ZXCV${code}tail")
file(WRITE "${WORK}/tampered"  "${code}SERIAL:QWER-7788-ZXCV${code}!")

function(run expect_ok)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(expect_ok AND NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  elseif(NOT expect_ok AND rc EQUAL 0)
    message(FATAL_ERROR "unexpectedly succeeded: ${ARGN}\n${out}${err}")
  endif()
  set(last "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect_same a b)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${a}" "${b}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${a} and ${b} differ")
  endif()
endfunction()

# Marker spec.
run(TRUE "${GRAFT}" diff --mask "marker=SERIAL:,len=14" "${WORK}/v1" "${WORK}/v2" "${WORK}/p.gft")
if(NOT last MATCHES "mask: OLD 0x[0-9a-f]+\\+14 -> NEW")
  message(FATAL_ERROR "diff didn't report the resolved mask:\n${last}")
endif()
run(TRUE "${GRAFT}" info "${WORK}/p.gft")
if(NOT last MATCHES "masks: +1")
  message(FATAL_ERROR "info didn't report masks:\n${last}")
endif()
run(TRUE "${GRAFT}" apply "${WORK}/installed" "${WORK}/p.gft" "${WORK}/out")
expect_same("${WORK}/expected" "${WORK}/out")
run(FALSE "${GRAFT}" apply "${WORK}/tampered" "${WORK}/p.gft" "${WORK}/out2")

# The same from a mask file, with comments.
file(WRITE "${WORK}/masks.txt" "# serial field\nmarker=hex:53455249414c3a,len=14   # 'SERIAL:'\n")
run(TRUE "${GRAFT}" diff --mask-file "${WORK}/masks.txt" "${WORK}/v1" "${WORK}/v2" "${WORK}/p2.gft")
run(TRUE "${GRAFT}" apply "${WORK}/installed" "${WORK}/p2.gft" "${WORK}/out3")
expect_same("${WORK}/expected" "${WORK}/out3")

# Build-time errors: a marker that isn't unique, a size change without a rule.
run(FALSE "${GRAFT}" diff --mask "marker=code,len=4" "${WORK}/v1" "${WORK}/v2" "${WORK}/bad.gft")
if(NOT last MATCHES "more than once")
  message(FATAL_ERROR "expected a uniqueness error:\n${last}")
endif()
run(FALSE "${GRAFT}" diff --mask "marker=SERIAL:,len=14,newlen=16" "${WORK}/v1" "${WORK}/v2" "${WORK}/bad.gft")
if(NOT last MATCHES "pad=BYTE")
  message(FATAL_ERROR "expected a size-change error:\n${last}")
endif()
if(EXISTS "${WORK}/bad.gft")
  message(FATAL_ERROR "failed diff left a patch behind")
endif()
