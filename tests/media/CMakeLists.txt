ct_add_test(ct_media_tests DEPS ct_media SOURCES
  main.cpp
  media_info_test.cpp
  video_decoder_test.cpp
  audio_decoder_test.cpp)

# Test-only helper: writes the P04-style pattern files to out/testmedia/ until tools/gen_test_media exists.
add_executable(ct_gen_test_media gen_pattern_main.cpp)
target_link_libraries(ct_gen_test_media PRIVATE ct_options ct_platform)
set_target_properties(ct_gen_test_media PROPERTIES FOLDER "tests")
