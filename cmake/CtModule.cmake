# ct_add_module(<name> SOURCES ... DEPS ...)  -> static lib ct_<name>, public include dir include/
function(ct_add_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
  add_library(ct_${name} STATIC ${M_SOURCES})
  target_include_directories(ct_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(ct_${name} PUBLIC ct_options ct_winlibs ${M_DEPS})
  set_target_properties(ct_${name} PROPERTIES FOLDER "modules")
endfunction()

# ct_add_test(<exe> SOURCES ... DEPS ...)  -> registers with ctest, runs from tests/ so tests/data is reachable
function(ct_add_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ct_options ct_test ${T_DEPS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
  set_target_properties(${name} PROPERTIES FOLDER "tests")
endfunction()

# ct_compile_shaders(<target> SHADERS file.hlsl:profile:entry ...)
# Produces <build>/shaders/<stem>_<entry>.h with a byte array named g_<stem>_<entry>.
function(ct_compile_shaders target)
  cmake_parse_arguments(S "" "" "SHADERS" ${ARGN})
  find_program(FXC_EXE fxc REQUIRED)     # on PATH after vcvars64
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${out_dir})
  foreach(spec IN LISTS S_SHADERS)
    string(REPLACE ":" ";" parts ${spec})
    list(GET parts 0 file)
    list(GET parts 1 profile)
    list(GET parts 2 entry)
    get_filename_component(stem ${file} NAME_WE)
    set(out ${out_dir}/${stem}_${entry}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND ${FXC_EXE} /nologo /T ${profile} /E ${entry} /WX
              "$<IF:$<CONFIG:Debug>,/Zi;/Od,/O3>" /Fh ${out} /Vn g_${stem}_${entry}
              ${CMAKE_CURRENT_SOURCE_DIR}/${file}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file}
      COMMAND_EXPAND_LISTS COMMENT "fxc ${file} (${entry})")
    target_sources(${target} PRIVATE ${out})
  endforeach()
  target_include_directories(${target} PRIVATE ${out_dir})
endfunction()
