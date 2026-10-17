# One source per line so parallel phases (P04 encode, P06 decode) merge cleanly.
ct_add_module(media
  SOURCES
    src/mf_runtime.cpp
    src/encode_config.cpp
    src/encoder_caps.cpp
    src/mp4_writer.cpp
    src/probe.cpp
  DEPS
    ct_capture
    ct_audio)
