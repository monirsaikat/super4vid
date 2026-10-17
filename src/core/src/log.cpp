#include <ct/core/log.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// P00 logger: one SRWLOCK-protected 64 KB buffer written with WriteFile. Lines written before Init() stay
// buffered (up to the buffer size) and reach the file when it opens. P01 replaces the internals.

namespace ct {

namespace {

constexpr size_t kBufferBytes = 64 * 1024;
constexpr size_t kFlushThreshold = 48 * 1024;
constexpr ULONGLONG kFlushIntervalMs = 500;
constexpr int kKeepFiles = 5;
constexpr size_t kMaxLine = 2048;

struct LogState {
    SRWLOCK lock = SRWLOCK_INIT;
    char buffer[kBufferBytes];
    size_t used = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool opened = false;                 // Init() succeeded at least once; later writes go straight to the file
    ULONGLONG last_flush_ms = 0;
    std::atomic<int> level{static_cast<int>(LogLevel::Info)};
    char path_utf8[MAX_PATH * 3] = {};
};

LogState g_log;

char LevelLetter(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return 'T';
    case LogLevel::Debug: return 'D';
    case LogLevel::Info:  return 'I';
    case LogLevel::Warn:  return 'W';
    case LogLevel::Error: return 'E';
    default:              return '?';
    }
}

const char* BaseName(const char* path) {
    if (!path) return "?";
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

// Caller holds the lock.
void FlushLocked() {
    if (g_log.file != INVALID_HANDLE_VALUE && g_log.used > 0) {
        DWORD written = 0;
        WriteFile(g_log.file, g_log.buffer, static_cast<DWORD>(g_log.used), &written, nullptr);
        g_log.used = 0;
    } else if (g_log.opened) {
        g_log.used = 0;   // file closed after Shutdown(): nowhere to write, drop
    }
    g_log.last_flush_ms = GetTickCount64();
}

std::wstring Utf8ToWideLocal(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

LogLevel ParseLevel(const char* s, LogLevel fallback) {
    if (_stricmp(s, "trace") == 0) return LogLevel::Trace;
    if (_stricmp(s, "debug") == 0) return LogLevel::Debug;
    if (_stricmp(s, "info") == 0) return LogLevel::Info;
    if (_stricmp(s, "warn") == 0) return LogLevel::Warn;
    if (_stricmp(s, "error") == 0) return LogLevel::Error;
    if (_stricmp(s, "off") == 0) return LogLevel::Off;
    return fallback;
}

// Keeps the newest (kKeepFiles - 1) logs so the file about to be created makes kKeepFiles.
void RotateLogs(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\camtesia-*.log").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(names.begin(), names.end());   // the timestamp in the name sorts chronologically
    const size_t keep = kKeepFiles - 1;
    for (size_t i = 0; i + keep < names.size(); ++i) DeleteFileW((dir + L"\\" + names[i]).c_str());
}

} // namespace

void Log::Init(const char* log_dir_utf8) {
    char env[16] = {};
    if (GetEnvironmentVariableA("CT_LOG", env, sizeof(env)) > 0) SetLevel(ParseLevel(env, Level()));

    std::wstring dir = Utf8ToWideLocal(log_dir_utf8);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    RotateLogs(dir);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"camtesia-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    std::wstring path = dir + L"\\" + name + L".log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // Two processes in the same second (e.g. a second instance that is about to exit): add the pid.
        wchar_t suffix[32];
        swprintf(suffix, 32, L"-%lu.log", GetCurrentProcessId());
        path = dir + L"\\" + name + suffix;
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) return;

    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file != INVALID_HANDLE_VALUE) CloseHandle(g_log.file);
    g_log.file = file;
    g_log.opened = true;
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, g_log.path_utf8, sizeof(g_log.path_utf8), nullptr, nullptr);
    FlushLocked();
    ReleaseSRWLockExclusive(&g_log.lock);
}

void Log::SetLevel(LogLevel level) {
    g_log.level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Log::Level() {
    return static_cast<LogLevel>(g_log.level.load(std::memory_order_relaxed));
}

void Log::Write(LogLevel level, const char* category, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) < g_log.level.load(std::memory_order_relaxed) || level == LogLevel::Off) return;

    char text[kMaxLine];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%c] [%-8.8s] %s:%d ", st.wYear,
                          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, LevelLetter(level),
                          category ? category : "", BaseName(file), line);
    if (n < 0) return;
    size_t len = std::min(static_cast<size_t>(n), sizeof(text) - 1);
    va_list args;
    va_start(args, fmt);
    const int m = std::vsnprintf(text + len, sizeof(text) - len, fmt ? fmt : "", args);
    va_end(args);
    if (m > 0) len = std::min(len + static_cast<size_t>(m), sizeof(text) - 3);   // leave room for "\r\n"
    text[len++] = '\r';
    text[len++] = '\n';
    text[len] = '\0';

    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.used + len > kBufferBytes) {
        FlushLocked();
        if (g_log.used + len > kBufferBytes) g_log.used = 0;   // not opened yet and full: drop the oldest lines
    }
    std::memcpy(g_log.buffer + g_log.used, text, len);
    g_log.used += len;
    if (g_log.used >= kFlushThreshold || level >= LogLevel::Warn) FlushLocked();
    ReleaseSRWLockExclusive(&g_log.lock);

    if (IsDebuggerPresent()) OutputDebugStringA(text);
}

void Log::Tick() {
    if (GetTickCount64() - g_log.last_flush_ms < kFlushIntervalMs) return;
    if (!TryAcquireSRWLockExclusive(&g_log.lock)) return;   // someone is writing; try next tick
    FlushLocked();
    ReleaseSRWLockExclusive(&g_log.lock);
}

void Log::Flush() {
    // Bounded wait: the crash handler calls this, and the crashing thread may be the one holding the lock.
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (TryAcquireSRWLockExclusive(&g_log.lock)) {
            FlushLocked();
            if (g_log.file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log.file);
            ReleaseSRWLockExclusive(&g_log.lock);
            return;
        }
        Sleep(1);
    }
}

void Log::Shutdown() {
    AcquireSRWLockExclusive(&g_log.lock);
    FlushLocked();
    if (g_log.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

const char* Log::FilePathUtf8() {
    return g_log.path_utf8;
}

} // namespace ct
