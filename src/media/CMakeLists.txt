ct_add_module(media
  SOURCES
    src/media_info.cpp
    src/video_decoder.cpp
    src/audio_decoder.cpp
  DEPS ct_platform ct_audio)
