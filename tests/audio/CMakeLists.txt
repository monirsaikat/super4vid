ct_add_test(ct_audio_tests DEPS ct_audio SOURCES
  devices_test.cpp
  stream_aligner_test.cpp
  meter_test.cpp
  wasapi_capture_test.cpp)
