ct_add_test(ct_render_tests DEPS ct_render ct_capture SOURCES
  render_fixture.cpp
  layer_math_test.cpp
  golden_test.cpp
  bench_test.cpp
  cache_test.cpp
  timeline_player_test.cpp)
