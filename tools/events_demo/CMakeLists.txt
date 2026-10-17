add_executable(events_demo main.cpp)
target_link_libraries(events_demo PRIVATE ct_options ct_recorder)
set_target_properties(events_demo PROPERTIES FOLDER "tools")
