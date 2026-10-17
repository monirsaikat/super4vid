add_executable(gif_demo main.cpp)
target_link_libraries(gif_demo PRIVATE ct_options ct_export)
target_include_directories(gif_demo PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
set_target_properties(gif_demo PROPERTIES FOLDER "tools")
