ct_add_module(platform
  SOURCES src/window.cpp src/d3d_device.cpp src/crash_handler.cpp src/single_instance.cpp src/paths.cpp
          src/texture_pool.cpp src/gpu_timer.cpp src/image_io.cpp
          src/hooks_thread.cpp
          src/hotkeys.cpp
          src/tray_icon.cpp
          src/layered_window.cpp
          src/nv12_blit.cpp
  DEPS ct_core)
ct_compile_shaders(ct_platform SHADERS
  shaders/nv12_blit.hlsl:vs_5_0:vs_main
  shaders/nv12_blit.hlsl:ps_5_0:ps_main
  shaders/nv12_blit.hlsl:ps_5_0:ps_box)
