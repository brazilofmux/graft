add_executable(graft_tests test_main.cpp test_hash.cpp test_status.cpp)
target_link_libraries(graft_tests PRIVATE graft::graft)
target_include_directories(graft_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
graft_warnings(graft_tests)
add_test(NAME unit COMMAND graft_tests)

if(GRAFT_TOOLS)
  # CLI smoke tests.
  add_test(NAME cli.version COMMAND graft_cli version)
  set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "^graft ${PROJECT_VERSION} \\(zstd ")

  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/empty.bin "")
  add_test(NAME cli.hash.empty COMMAND graft_cli hash ${CMAKE_CURRENT_BINARY_DIR}/empty.bin)
  set_tests_properties(cli.hash.empty PROPERTIES
    PASS_REGULAR_EXPRESSION "^af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262  ")

  add_test(NAME cli.hash.missing COMMAND graft_cli hash ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist)
  set_tests_properties(cli.hash.missing PROPERTIES WILL_FAIL TRUE)

  add_test(NAME cli.unknown COMMAND graft_cli frobnicate)
  set_tests_properties(cli.unknown PROPERTIES WILL_FAIL TRUE)
endif()
