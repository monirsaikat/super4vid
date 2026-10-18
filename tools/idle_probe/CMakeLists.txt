add_executable(idle_probe main.cpp)
target_link_libraries(idle_probe PRIVATE ct_options ct_platform psapi)
set_target_properties(idle_probe PROPERTIES FOLDER "tools")
