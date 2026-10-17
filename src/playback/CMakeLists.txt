ct_add_module(playback
  SOURCES
    src/frame_cache.cpp
    src/media_cache.cpp
    src/audio_engine.cpp
    src/playback_clock.cpp
    src/player.cpp
  DEPS ct_media)
