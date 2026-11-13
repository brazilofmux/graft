add_executable(graft_cli graft/main.cpp)
set_target_properties(graft_cli PROPERTIES OUTPUT_NAME graft)
target_link_libraries(graft_cli PRIVATE graft::graft)
graft_warnings(graft_cli)
