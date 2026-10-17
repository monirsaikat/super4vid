# One source per line so parallel phases (P04 encode, P06 decode) merge cleanly.
ct_add_test(ct_media_tests DEPS ct_media SOURCES
  main.cpp
  encode_config_test.cpp
  encoder_caps_test.cpp)
