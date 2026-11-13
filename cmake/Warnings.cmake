# graft_warnings(<target>): the project's warning set, applied to our own code only.
# Size and offset arithmetic is the heart of a patch format, so implicit
# narrowing and signedness conversions are warnings here.
function(graft_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(GRAFT_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wcast-qual -Wformat=2 -Wundef -Wnull-dereference -Wimplicit-fallthrough)
    if(GRAFT_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
