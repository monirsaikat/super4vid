# Whole-app checks that run Camtesia.exe itself (cmake -P scripts; each launch gets its own CT_INSTANCE_SUFFIX).
# Registered from the root CMakeLists after tools/, because they use dump_check.

# P00 0.7: `--crash-test` leaves a minidump whose faulting stack resolves to the null write in main.cpp.
add_test(NAME camtesia_crash_dump
         COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:Camtesia> -DCHECK=$<TARGET_FILE:dump_check>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/crash_dump_test.cmake)

# P11 11.3 / P00 0.7: Debug exit is leak-free in every app mode: no CRT leak report and nothing from the D3D11 debug
# layer but the device itself. Release has neither check, so the script only verifies that each mode exits 0 there.
# Opens windows: skipped (exit 77) under CT_CI.
add_test(NAME camtesia_exit_leak_check
         COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:Camtesia> -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                 -DDEBUG_BUILD=$<CONFIG:Debug> -P ${CMAKE_CURRENT_SOURCE_DIR}/exit_leak_check.cmake)
set_tests_properties(camtesia_exit_leak_check PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
