ct_add_test(ct_export_tests DEPS ct_export SOURCES
  preset_test.cpp)
target_include_directories(ct_export_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/common)
