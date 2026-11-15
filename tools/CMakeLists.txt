add_executable(graft_cli graft/main.cpp graft/masks.cpp)
set_target_properties(graft_cli PROPERTIES OUTPUT_NAME graft)
target_link_libraries(graft_cli PRIVATE graft::graft)
if(WIN32)
  target_link_libraries(graft_cli PRIVATE shell32)  # CommandLineToArgvW
endif()
graft_warnings(graft_cli)
