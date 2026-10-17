ct_add_test(ct_recorder_tests DEPS ct_recorder SOURCES
  main.cpp
  session_clock_test.cpp
  keys_test.cpp
  typing_test.cpp
  event_recorder_test.cpp
  hooks_thread_test.cpp
  tray_hotkeys_test.cpp)
