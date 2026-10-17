ct_add_module(core
  SOURCES src/types.cpp src/error.cpp src/log.cpp src/assert.cpp
          src/strings.cpp src/time.cpp src/containers.cpp)
target_link_libraries(ct_core PUBLIC bcrypt)
