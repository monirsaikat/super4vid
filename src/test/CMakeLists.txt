# Test harness used by every tests/<module> executable (see docs/06 section 1).
ct_add_module(test
  SOURCES src/ct_test.cpp src/ct_test_main.cpp
  DEPS ct_core)
