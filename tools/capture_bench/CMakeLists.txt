add_executable(capture_bench main.cpp)
target_link_libraries(capture_bench PRIVATE ct_options ct_capture ct_media)
set_target_properties(capture_bench PROPERTIES FOLDER "tools")
