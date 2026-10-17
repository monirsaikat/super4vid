#include <ct/core/assert.h>

#include <ct/core/log.h>

#include <windows.h>
#include <intrin.h>

#include <atomic>

namespace ct {

namespace {
std::atomic<FatalHandler> g_fatal_handler{nullptr};
}

void SetFatalHandler(FatalHandler handler) {
    g_fatal_handler.store(handler, std::memory_order_release);
}

void AssertFailed(const char* expr, const char* file, int line) {
    Log::Write(LogLevel::Error, "assert", file, line, "assertion failed: %s", expr);
    Log::Flush();
}

void Fatal(const char* msg, const char* file, int line) {
    Log::Write(LogLevel::Error, "fatal", file, line, "FATAL: %s", msg ? msg : "(null)");
    Log::Flush();
    if (FatalHandler handler = g_fatal_handler.load(std::memory_order_acquire)) handler(msg);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

} // namespace ct
