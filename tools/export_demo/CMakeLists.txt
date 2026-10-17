add_executable(export_demo main.cpp)
target_link_libraries(export_demo PRIVATE ct_options ct_export)
target_include_directories(export_demo PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
set_target_properties(export_demo PROPERTIES FOLDER "tools")
