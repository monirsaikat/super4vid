ct_add_test(ct_project_tests DEPS ct_project SOURCES
  types_test.cpp
  serialize_test.cpp
  ops_test.cpp
  fuzz_test.cpp
  undo_test.cpp)
