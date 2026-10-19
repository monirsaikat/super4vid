# The manifest's assemblyIdentity version comes from project(... VERSION), like version.h and app.rc.
configure_file(app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)
add_executable(super4vid WIN32 main.cpp app.rc ${CMAKE_CURRENT_BINARY_DIR}/app.manifest)
target_include_directories(super4vid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})   # app.rc: "assets/icon.ico"
target_link_libraries(super4vid PRIVATE ct_options ct_platform ct_ui ct_recorder)
set_target_properties(super4vid PROPERTIES FOLDER "app")

# P11 11.2: DLLs the recorder bar does not need before its first frame load on first use instead of at process start
# (the loader maps and initializes every static import before wWinMain). Media Foundation is entered only through
# MfRuntime::Ensure, which checks that mfplat.dll loads (N editions without the Media Feature Pack) before any MF call.
# Not delay-loaded: d3d11/dxgi/d3dcompiler (first frame), dbghelp (the crash filter must not run the loader).
target_link_options(super4vid PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mf.dll /DELAYLOAD:mfreadwrite.dll
                    /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:propsys.dll)
target_link_libraries(super4vid PRIVATE delayimp)
