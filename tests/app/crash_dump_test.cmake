# ctest super4vid_crash_dump (P00 0.7): run `super4vid.exe --crash-test`, expect a new crash-*.dmp in
# %LOCALAPPDATA%\super4vid\crashes whose faulting stack (dump_check) contains main.cpp, then delete that dump.
# -DAPP=<super4vid.exe> -DCHECK=<dump_check.exe>
file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/super4vid/crashes" dir)
file(GLOB before "${dir}/crash-*.dmp")
# Own instance name: a running super4vid (another worktree, the user's) must not receive the command line instead.
execute_process(COMMAND ${CMAKE_COMMAND} -E env CT_INSTANCE_SUFFIX=crash-test-ctest ${APP} --crash-test
                RESULT_VARIABLE rc TIMEOUT 60)
message(STATUS "super4vid --crash-test exit: ${rc}")
file(GLOB after "${dir}/crash-*.dmp")
if(before)
  list(REMOVE_ITEM after ${before})
endif()
list(LENGTH after n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "expected one new dump in ${dir}, found ${n}: ${after}")
endif()
file(SIZE "${after}" bytes)
message(STATUS "dump: ${after} (${bytes} bytes)")
execute_process(COMMAND ${CHECK} ${after} --expect main.cpp RESULT_VARIABLE check_rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}${err}")
file(REMOVE "${after}")
if(NOT check_rc EQUAL 0)
  message(FATAL_ERROR "dump_check failed (${check_rc})")
endif()
