# The manifest's assemblyIdentity version comes from project(... VERSION), like version.h and app.rc.
configure_file(app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)
add_executable(Camtesia WIN32 main.cpp app.rc ${CMAKE_CURRENT_BINARY_DIR}/app.manifest)
target_include_directories(Camtesia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})   # app.rc: "assets/icon.ico"
target_link_libraries(Camtesia PRIVATE ct_options ct_platform ct_ui ct_recorder)
set_target_properties(Camtesia PROPERTIES FOLDER "app")
