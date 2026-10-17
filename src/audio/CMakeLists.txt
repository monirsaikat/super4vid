ct_add_module(audio
  SOURCES src/devices.cpp src/stream_aligner.cpp src/meter.cpp src/dsp.cpp src/dsp_avx2.cpp
          src/capture_engine.cpp src/wasapi_capture.cpp src/wasapi_loopback.cpp src/wasapi_render.cpp
          src/resampler.cpp src/wav.cpp
  DEPS ct_platform)
# The AVX2 kernels are the only code built for AVX2; dsp::Init() calls them only after a runtime CPU check.
set_source_files_properties(src/dsp_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
