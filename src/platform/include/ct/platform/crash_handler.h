#pragma once
// ct/platform/crash_handler.h
// Writes a minidump to Paths::CrashDir() on any unhandled SEH exception, invalid CRT parameter, pure virtual call,
// std::terminate, abort() or CT_FATAL, runs the registered emergency callbacks, flushes the log and terminates.
// Thread model: Install() once, early in wWinMain, on the main thread. The handlers run on the crashing thread
// (a helper thread writes the dump for stack overflows).
// Lifetime: process-wide; there is no uninstall.

namespace ct::platform {

struct CrashHandler {
    static void Install();                                         // SetUnhandledExceptionFilter + _set_invalid_parameter_handler + _set_purecall_handler + set_terminate
    static void SetDumpDir(const char* utf8);
    using EmergencyCallback = void (*)(void* user);
    static void RegisterEmergency(EmergencyCallback, void* user);  // called inside the filter, max 8, must not allocate (P05 registers "finalize the recording")
    [[noreturn]] static void WriteDumpAndExit(const char* reason);
    static const wchar_t* LastDumpPath();                          // empty until a dump was written (tests)
};

} // namespace ct::platform
