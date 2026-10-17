add_executable(avsync_source main.cpp)
target_link_libraries(avsync_source PRIVATE ct_options ct_media)
set_target_properties(avsync_source PROPERTIES FOLDER "tools")
