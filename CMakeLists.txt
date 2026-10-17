cmake_minimum_required(VERSION 3.25)
project(Camtesia VERSION 0.1.0 LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Camtesia builds with MSVC only. See docs/01-toolchain-and-build.md")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT, /MTd
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)                        # /GL + /LTCG
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
enable_testing()

# Replace CMake's default MSVC flags so nothing conflicts with ours.
set(CMAKE_CXX_FLAGS         "/DWIN32 /D_WINDOWS")
set(CMAKE_CXX_FLAGS_DEBUG   "")
set(CMAKE_CXX_FLAGS_RELEASE "/DNDEBUG")
set(CMAKE_C_FLAGS           "/DWIN32 /D_WINDOWS")
set(CMAKE_C_FLAGS_DEBUG     "")
set(CMAKE_C_FLAGS_RELEASE   "/DNDEBUG")

add_library(ct_options INTERFACE)
target_compile_options(ct_options INTERFACE
  /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /Zc:throwingNew
  /utf-8 /EHs-c- /GR- /MP /Zi /FS /diagnostics:caret /guard:cf /sdl-
  $<$<CONFIG:Debug>:/Od /RTC1>
  $<$<CONFIG:Release>:/O2 /Ob3 /Oi /Ot /Gy /Gw>)
target_compile_definitions(ct_options INTERFACE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX STRICT _CRT_SECURE_NO_WARNINGS
  _HAS_EXCEPTIONS=0 _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008
  CT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  CT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  CT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  $<$<CONFIG:Debug>:CT_DEBUG=1>)
target_link_options(ct_options INTERFACE
  /DEBUG:FULL /INCREMENTAL:NO /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
  $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

# Version stamping: generated/ct/version.h (CT_VERSION_STRING, CT_GIT_HASH, CT_BUILD_DATE).
execute_process(COMMAND git rev-parse --short=7 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE CT_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CT_GIT_HASH)
  set(CT_GIT_HASH "nogit")
endif()
string(TIMESTAMP CT_BUILD_DATE "%Y-%m-%d")
configure_file(cmake/version.h.in ${CMAKE_BINARY_DIR}/generated/ct/version.h)
target_include_directories(ct_options INTERFACE ${CMAKE_BINARY_DIR}/generated)

add_library(ct_winlibs INTERFACE)
target_link_libraries(ct_winlibs INTERFACE
  d3d11 dxgi dxguid d3dcompiler d2d1 dwrite windowscodecs
  mfplat mf mfreadwrite mfuuid wmcodecdspuuid
  ole32 oleaut32 uuid user32 gdi32 shell32 shcore shlwapi dwmapi avrt winmm
  comctl32 propsys dbghelp advapi32 version)

include(cmake/CtModule.cmake)

# One line per module, in dependency order. Phases append their module here and nothing else.
add_subdirectory(third_party/imgui)
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/audio)
add_subdirectory(src/capture)
add_subdirectory(src/test)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(tests)
add_subdirectory(tools/audio_bench)
add_subdirectory(tools/capture_bench)
