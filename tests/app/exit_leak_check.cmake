# ctest super4vid_exit_leak_check (P11 11.3, P00 0.7): every app mode that ends on its own is started, must exit 0,
# and in Debug must print neither the CRT leak report ("Detected memory leaks!", _CRTDBG_LEAK_CHECK_DF) nor the
# D3D11 debug-layer summary line ("d3d11 debug: ..." on stderr: errors, warnings or live objects besides the device).
# -DAPP=<super4vid.exe> -DSOURCE_DIR=<repo> -DDEBUG_BUILD=0|1
if(DEFINED ENV{CT_CI})
  message(STATUS "CT_CI set: skipped (opens windows, records the screen)")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    cmake_language(EXIT 77)
  endif()
  return()
endif()

file(TO_CMAKE_PATH "$ENV{TEMP}/super4vid_exit_check_$ENV{USERNAME}" tmp)
file(REMOVE_RECURSE "${tmp}")
file(MAKE_DIRECTORY "${tmp}")
set(media "${SOURCE_DIR}/tests/data/pattern_1080p60_5s.mp4")

# "name|arg,arg,..." (commas, not semicolons: a ; would split the entry itself)
set(modes
  "bar-time-to-window|--time-to-window"
  "bar-mic-popover|--screenshot,${tmp}/mic.png,--bar-open,mic"
  "bar-settings|--screenshot,${tmp}/settings.png,--bar-settings"
  "bar-record|--screenshot,${tmp}/rec.png,--bar-record"
  "settings-window|--settings-demo,--screenshot,${tmp}/settings_demo.png"
  "editor-media|--editor,${media},--seek,1,--screenshot,${tmp}/editor.png,--frames,40"
  "editor-demo|--editor-demo,--screenshot,${tmp}/editor_demo.png"
  "headless-record|--record,--seconds,2,--out,${tmp}/headless")

set(failed "")
set(i 0)
foreach(mode IN LISTS modes)
  string(FIND "${mode}" "|" bar)
  string(SUBSTRING "${mode}" 0 ${bar} name)
  math(EXPR bar "${bar} + 1")
  string(SUBSTRING "${mode}" ${bar} -1 parts)
  string(REPLACE "," ";" parts "${parts}")
  math(EXPR i "${i} + 1")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env CT_INSTANCE_SUFFIX=exitcheck-${i} ${APP} ${parts}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
  set(bad "")
  if(NOT rc EQUAL 0)
    set(bad "exit ${rc}")
  endif()
  if(DEBUG_BUILD)
    if(err MATCHES "Detected memory leaks")
      set(bad "${bad} CRT leaks")
    endif()
    if(err MATCHES "d3d11 debug:")
      set(bad "${bad} D3D11 debug layer")
    endif()
  endif()
  if(bad)
    message(STATUS "[FAIL] ${name}: ${bad}\n${err}")
    list(APPEND failed ${name})
  else()
    message(STATUS "[PASS] ${name}")
  endif()
endforeach()
file(REMOVE_RECURSE "${tmp}")
if(failed)
  message(FATAL_ERROR "modes failing the exit check: ${failed}")
endif()
