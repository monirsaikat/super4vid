ct_add_module(platform
  SOURCES src/window.cpp src/d3d_device.cpp src/crash_handler.cpp src/single_instance.cpp src/paths.cpp
          src/texture_pool.cpp src/gpu_timer.cpp src/image_io.cpp
          src/hooks_thread.cpp
          src/hotkeys.cpp
          src/tray_icon.cpp
          src/layered_window.cpp
  DEPS ct_core)
