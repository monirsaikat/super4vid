ct_add_module(render
  SOURCES src/render_context.cpp src/layer_math.cpp src/effects.cpp src/text_renderer.cpp src/cursor_renderer.cpp
          src/compositor.cpp
  DEPS ct_platform ct_project ct_events)
set(CT_RENDER_SHADERS
  shaders/fullscreen.hlsl:vs_5_0:vs_main
  shaders/fullscreen.hlsl:vs_5_0:vs_quad
  shaders/layer.hlsl:ps_5_0:ps_nv12
  shaders/layer.hlsl:ps_5_0:ps_rgba
  shaders/layer.hlsl:ps_5_0:ps_solid
  shaders/gradient.hlsl:ps_5_0:ps_main
  shaders/blur.hlsl:ps_5_0:ps_blur
  shaders/blur.hlsl:ps_5_0:ps_copy
  shaders/pixelate.hlsl:ps_5_0:ps_main
  shaders/cursor.hlsl:ps_5_0:ps_ring
  shaders/cursor.hlsl:ps_5_0:ps_disc
  shaders/cursor.hlsl:ps_5_0:ps_spotlight
  shaders/cursor.hlsl:ps_5_0:ps_shadow
  shaders/shadow_mask.hlsl:ps_5_0:ps_main
  shaders/composite.hlsl:ps_5_0:ps_shadow
  shaders/composite.hlsl:ps_5_0:ps_multiply
  shaders/transition.hlsl:ps_5_0:ps_main)
ct_compile_shaders(ct_render SHADERS ${CT_RENDER_SHADERS})
# Every shader includes common.hlsli; fxc's includes are not tracked by the generator, so list it as a dependency.
foreach(spec IN LISTS CT_RENDER_SHADERS)
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 file)
  list(GET parts 2 entry)
  get_filename_component(stem ${file} NAME_WE)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/${stem}_${entry}.h APPEND
                     DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.hlsli)
endforeach()
