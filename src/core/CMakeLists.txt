ct_add_module(core
  SOURCES src/types.cpp src/error.cpp src/log.cpp src/assert.cpp
          src/strings.cpp src/time.cpp src/containers.cpp
          src/thread.cpp src/async.cpp src/json.cpp
          src/fs.cpp src/config.cpp src/trace.cpp src/stats.cpp)
target_link_libraries(ct_core PUBLIC bcrypt)
