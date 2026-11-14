# cmake -DGRAFT=<graft binary> -DWORK=<scratch dir> -P cli_archive.cmake
#
# Archives through the CLI: add three releases, list, diff, extract, make a
# package and apply it, then drop a version.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

string(REPEAT "release payload " 500 code)
file(WRITE "${WORK}/r1/app"      "${code}")
file(WRITE "${WORK}/r1/old.txt"  "old")
file(WRITE "${WORK}/r2/app"      "${code}two")
file(WRITE "${WORK}/r2/old.txt"  "old")
file(WRITE "${WORK}/r3/app"      "three${code}two")
file(WRITE "${WORK}/r3/new.txt"  "new")

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

set(A "${WORK}/rel.gfa")
run(TRUE "${GRAFT}" archive add "${A}" "${WORK}/r1" 1.0)
run(TRUE "${GRAFT}" archive add "${A}" "${WORK}/r2" 2.0)
run(TRUE "${GRAFT}" archive add "${A}" "${WORK}/r3" 3.0)
run(FALSE "${GRAFT}" archive add "${A}" "${WORK}/r3" 3.0)

run(TRUE "${GRAFT}" archive list "${A}")
if(NOT last MATCHES "3 versions")
  message(FATAL_ERROR "list output unexpected:\n${last}")
endif()
run(TRUE "${GRAFT}" archive diff "${A}" 1.0 3.0)
if(NOT last MATCHES "- old.txt" OR NOT last MATCHES "\\+ new.txt" OR NOT last MATCHES "M app")
  message(FATAL_ERROR "diff output unexpected:\n${last}")
endif()

run(TRUE "${GRAFT}" archive extract "${A}" 2.0 "${WORK}/x2")
expect_same("${WORK}/r2/app" "${WORK}/x2/app")
run(FALSE "${GRAFT}" archive extract "${A}" 2.0 "${WORK}/x2")

run(TRUE "${GRAFT}" archive mkpatch -o "${WORK}/up.gfp" "${A}" 3.0)
run(TRUE "${GRAFT}" update "${WORK}/up.gfp" "${WORK}/x2")
expect_same("${WORK}/r3/app" "${WORK}/x2/app")
expect_same("${WORK}/r3/new.txt" "${WORK}/x2/new.txt")

run(TRUE "${GRAFT}" archive drop "${A}" 2.0)
run(TRUE "${GRAFT}" archive list "${A}")
if(NOT last MATCHES "2 versions" OR last MATCHES "2\\.0 ")
  message(FATAL_ERROR "drop didn't take:\n${last}")
endif()
run(TRUE "${GRAFT}" archive extract "${A}" 1.0 "${WORK}/x1")
expect_same("${WORK}/r1/app" "${WORK}/x1/app")
