ct_add_module(export
  SOURCES src/preset.cpp src/gif_encoder.cpp src/gif_quantize.cpp src/gif_lzw.cpp
          src/scaler.cpp
          src/export_job.cpp
  DEPS ct_core ct_platform
       ct_render
       ct_media
       ct_capture)
ct_compile_shaders(ct_export SHADERS
  shaders/scale.hlsl:vs_5_0:vs_main
  shaders/scale.hlsl:ps_5_0:ps_main)
