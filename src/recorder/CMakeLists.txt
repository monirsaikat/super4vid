ct_add_module(recorder
  SOURCES src/session_clock.cpp src/keys.cpp src/typing.cpp src/event_recorder.cpp
  DEPS ct_capture ct_events)
