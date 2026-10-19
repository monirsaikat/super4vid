#pragma once
// ct/core/log.h
// Process-wide logger: UTF-8 lines to %LOCALAPPDATA%\super4vid\logs\super4vid-<stamp>.log and to the debugger.
// Thread model: Write() may be called from any thread, including capture/audio threads: it formats on the caller's
// stack and only memcpy's into a 1 MB ring under an SRW lock (no file IO); a "ct-log" thread writes the file every
// 500 ms, on Warn/Error, and on Flush(). A full ring drops the new line (counted as Stats "log.dropped").
// Init/SetLevel/Shutdown from the main thread only. Files rotate at 10 MB; the newest 5 are kept.
// Lifetime: lines written before Init() are buffered (up to 1 MB) and land in the file once it opens.

#include <cstdint>

namespace ct {

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Off };

struct Log {
    static void Init(const char* log_dir_utf8);        // opens <log_dir>\super4vid-<stamp>.log, rotates (keep 5)
    static void SetLevel(LogLevel);                    // default Info; CT_LOG env var: trace|debug|info|warn|error
    static LogLevel Level();
    static void Write(LogLevel, const char* category, const char* file, int line, const char* fmt, ...);
    static void Tick();                                // no-op since P01 (the ct-log thread flushes); kept for API stability
    static void Flush();                               // synchronous: drains the ring and FlushFileBuffers (bounded wait)
    static void Shutdown();
    static const char* FilePathUtf8();                 // empty string before Init()
    // Warn + Error lines written since process start (passing the level filter; the editor's status-bar count).
    static uint64_t WarningCount();
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
