add_executable(gen_test_media main.cpp)
target_link_libraries(gen_test_media PRIVATE ct_options ct_media)
set_target_properties(gen_test_media PROPERTIES FOLDER "tools")
