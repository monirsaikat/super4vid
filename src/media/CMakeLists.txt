# One source per line so parallel phases merge cleanly.
ct_add_module(media
  SOURCES
    src/mf_runtime.cpp
    src/encode_config.cpp
    src/encoder_caps.cpp
    src/mp4_writer.cpp
    src/probe.cpp
    src/media_info.cpp
    src/video_decoder.cpp
    src/audio_decoder.cpp
    src/peaks.cpp
    src/image_io.cpp
  DEPS
    ct_platform
    ct_capture
    ct_audio)
