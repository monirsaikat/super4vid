add_executable(dump_check main.cpp)
target_link_libraries(dump_check PRIVATE ct_options ct_platform dbghelp)
set_target_properties(dump_check PROPERTIES FOLDER "tools")
