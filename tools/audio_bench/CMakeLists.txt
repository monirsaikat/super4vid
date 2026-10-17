add_executable(audio_bench main.cpp)
target_link_libraries(audio_bench PRIVATE ct_options ct_audio)
set_target_properties(audio_bench PROPERTIES FOLDER "tools")
