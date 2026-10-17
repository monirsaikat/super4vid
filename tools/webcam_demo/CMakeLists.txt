add_executable(webcam_demo main.cpp)
target_link_libraries(webcam_demo PRIVATE ct_options ct_capture)
set_target_properties(webcam_demo PROPERTIES FOLDER "tools")
