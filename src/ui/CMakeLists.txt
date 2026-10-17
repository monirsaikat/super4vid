# P00: header-only target that exposes ct_imconfig.h. P09 turns this into ct_add_module(ui ...) with the same name.
add_library(ct_ui INTERFACE)
target_include_directories(ct_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ct_ui INTERFACE ct_core imgui)
