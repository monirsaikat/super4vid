ct_add_module(capture
  SOURCES src/display.cpp src/desktop_duplicator.cpp src/color_convert.cpp
  DEPS ct_platform)
ct_compile_shaders(ct_capture SHADERS
  shaders/fullscreen.hlsl:vs_5_0:vs_main
  shaders/bgra_to_nv12.hlsl:ps_5_0:ps_luma
  shaders/bgra_to_nv12.hlsl:ps_5_0:ps_chroma
  shaders/rgba16f_to_nv12.hlsl:ps_5_0:ps_luma
  shaders/rgba16f_to_nv12.hlsl:ps_5_0:ps_chroma)
# rgba16f_to_nv12.hlsl includes bgra_to_nv12.hlsl; rebuild its headers when the include changes.
foreach(entry ps_luma ps_chroma)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/rgba16f_to_nv12_${entry}.h APPEND
                     DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bgra_to_nv12.hlsl)
endforeach()
