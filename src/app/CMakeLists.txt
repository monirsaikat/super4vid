add_executable(Camtesia WIN32 main.cpp app.rc app.manifest)
target_include_directories(Camtesia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})   # app.rc: "assets/icon.ico"
target_link_libraries(Camtesia PRIVATE ct_options ct_platform ct_ui)
set_target_properties(Camtesia PROPERTIES FOLDER "app")
