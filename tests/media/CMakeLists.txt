# One source per line so parallel phases merge cleanly.
ct_add_test(ct_media_tests DEPS ct_media SOURCES
  main.cpp
  media_test_util.cpp
  encode_config_test.cpp
  encoder_caps_test.cpp
  mp4_writer_test.cpp
  texture_reuse_test.cpp
  test_data_test.cpp
  errors_test.cpp
  media_info_test.cpp
  video_decoder_test.cpp
  audio_decoder_test.cpp
  nv12_blit_test.cpp
  image_io_peaks_test.cpp)

# Test-only helper (P06): writes pattern files to out/testmedia/. tools/gen_test_media (P04) is the canonical generator.
add_executable(ct_gen_test_media gen_pattern_main.cpp)
target_link_libraries(ct_gen_test_media PRIVATE ct_options ct_platform)
set_target_properties(ct_gen_test_media PROPERTIES FOLDER "tests")
