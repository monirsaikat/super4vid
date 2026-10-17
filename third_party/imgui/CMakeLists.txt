add_library(imgui STATIC
  imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp
  backends/imgui_impl_win32.cpp backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/backends ${CMAKE_SOURCE_DIR}/src/ui/include
  ${CMAKE_SOURCE_DIR}/src/core/include)   # ct_imconfig.h -> <ct/core/assert.h>
target_compile_options(imgui PRIVATE /W3 /utf-8 /EHs-c- /GR- /MP /Zi /FS $<$<CONFIG:Debug>:/Od> $<$<CONFIG:Release>:/O2 /Gy /Gw>)
target_compile_definitions(imgui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DEFINE_MATH_OPERATORS IMGUI_USER_CONFIG="ct/ui/ct_imconfig.h"
  _HAS_EXCEPTIONS=0 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  $<$<CONFIG:Release>:IMGUI_DISABLE_DEMO_WINDOWS>)
# IM_ASSERT -> CT_ASSERT -> ct::AssertFailed lives in ct_core. LINK_ONLY so ct_core's /W4 /WX usage
# requirements (from ct_options) do not leak into the third-party compile.
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi $<LINK_ONLY:ct_core>)
set_target_properties(imgui PROPERTIES FOLDER "third_party")
