ct_add_module(capture
  SOURCES src/display.cpp src/desktop_duplicator.cpp src/color_convert.cpp src/cursor_tracker.cpp
          src/screen_capturer.cpp
          src/webcam_source.cpp
          src/webcam_synthetic.cpp
  DEPS ct_platform)
ct_compile_shaders(ct_capture SHADERS
  shaders/fullscreen.hlsl:vs_5_0:vs_main
  shaders/bgra_to_nv12.hlsl:ps_5_0:ps_luma
  shaders/bgra_to_nv12.hlsl:ps_5_0:ps_chroma
  shaders/bgra_to_nv12_cs.hlsl:cs_5_0:cs_main
  shaders/rgba16f_to_nv12.hlsl:ps_5_0:ps_luma
  shaders/rgba16f_to_nv12.hlsl:ps_5_0:ps_chroma
  shaders/rgba16f_to_nv12_cs.hlsl:cs_5_0:cs_main)
# The shaders include nv12_common.hlsli (and the HDR variants include the SDR file); fxc's includes are not tracked
# by the generator, so list them as extra dependencies.
foreach(out bgra_to_nv12_ps_luma bgra_to_nv12_ps_chroma bgra_to_nv12_cs_cs_main
            rgba16f_to_nv12_ps_luma rgba16f_to_nv12_ps_chroma rgba16f_to_nv12_cs_cs_main)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/${out}.h APPEND
                     DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/nv12_common.hlsli
                             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bgra_to_nv12.hlsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bgra_to_nv12_cs.hlsl)
endforeach()
