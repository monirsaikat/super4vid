ct_add_module(recorder
  SOURCES src/session_clock.cpp src/keys.cpp src/typing.cpp src/event_recorder.cpp
          src/overlay_geometry.cpp src/region_overlay.cpp src/countdown_overlay.cpp src/recording_frame.cpp
          src/bundle_writer.cpp src/recording_session.cpp src/headless.cpp
  DEPS ct_capture ct_events ct_audio ct_media)
