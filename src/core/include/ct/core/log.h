#pragma once
// ct/core/log.h
// Process-wide logger: UTF-8 lines to %LOCALAPPDATA%\Camtesia\logs\camtesia-<stamp>.log and to the debugger.
// Thread model: Write() may be called from any thread; Init/SetLevel/Shutdown from the main thread only.
// Lifetime: lines written before Init() are buffered and land in the file once it opens.
// P00 implementation: SRWLOCK + 64 KB buffer. P01 swaps the internals for a lock-free ring; this header stays.

#include <cstdint>

namespace ct {

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Off };

struct Log {
    static void Init(const char* log_dir_utf8);        // opens <log_dir>\camtesia-<stamp>.log, rotates (keep 5)
    static void SetLevel(LogLevel);                    // default Info; CT_LOG env var: trace|debug|info|warn|error
    static LogLevel Level();
    static void Write(LogLevel, const char* category, const char* file, int line, const char* fmt, ...);
    static void Tick();                                // flush if > 500 ms since last flush; UI loop calls it
    static void Flush();
    static void Shutdown();
    static const char* FilePathUtf8();                 // empty string before Init()
};

} // namespace ct

#ifndef CT_LOG_CATEGORY
#define CT_LOG_CATEGORY "app"   // each module redefines before including: #define CT_LOG_CATEGORY "capture"
#endif
#define CT_LOG_TRACE(...) ::ct::Log::Write(::ct::LogLevel::Trace, CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
#define CT_LOG_DEBUG(...) ::ct::Log::Write(::ct::LogLevel::Debug, CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
#define CT_LOG_INFO(...)  ::ct::Log::Write(::ct::LogLevel::Info,  CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
#define CT_LOG_WARN(...)  ::ct::Log::Write(::ct::LogLevel::Warn,  CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
#define CT_LOG_ERROR(...) ::ct::Log::Write(::ct::LogLevel::Error, CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
