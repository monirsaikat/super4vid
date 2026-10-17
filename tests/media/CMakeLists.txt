# One source per line so parallel phases (P04 encode, P06 decode) merge cleanly.
ct_add_test(ct_media_tests DEPS ct_media SOURCES
  main.cpp
  media_test_util.cpp
  encode_config_test.cpp
  encoder_caps_test.cpp
  mp4_writer_test.cpp
  texture_reuse_test.cpp
  test_data_test.cpp
  errors_test.cpp)
