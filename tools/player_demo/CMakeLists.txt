add_executable(player_demo main.cpp)
target_link_libraries(player_demo PRIVATE ct_options ct_playback)
set_target_properties(player_demo PROPERTIES FOLDER "tools")
