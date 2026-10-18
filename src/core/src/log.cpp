#include <ct/core/log.h>

#include <ct/core/stats.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Producers format a line on their own stack, then take an SRW lock only for the memcpy into a 1 MB ring.
// The "ct-log" thread drains the ring to the file 500 ms after the first pending line, when a Warn/Error is logged,
// when the ring is half full, and on Flush(). With nothing pending it sleeps without a timeout (P11 11.1: an idle
// app must not wake twice a second to find an empty ring); the first line after that wakes it (one SetEvent). No file IO, allocation or kernel call happens on the caller's thread, except SetEvent to wake
// the writer (Warn/Error and the half-full mark). When the ring is full the new line is dropped and counted
// ("log.dropped" in Stats). Files rotate at 10 MB; the newest 5 are kept.

namespace ct {

namespace {

constexpr size_t kRingBytes = 1u << 20;
constexpr DWORD kFlushIntervalMs = 500;
constexpr uint64_t kRotateBytes = 10ull << 20;
constexpr int kKeepFiles = 5;
constexpr size_t kMaxLine = 2048;
constexpr size_t kDirChars = 1024;

// Every member is constant-initialized: no dynamic initializer, usable from static constructors and destructors.
struct LogState {
    SRWLOCK ring_lock = SRWLOCK_INIT;         // serializes producers (the memcpy only)
    std::atomic<uint64_t> write{0};           // total bytes committed by producers
    std::atomic<uint64_t> read{0};            // total bytes consumed by the writer
    SRWLOCK io_lock = SRWLOCK_INIT;           // serializes consumers: ct-log thread, Flush, Init, Shutdown
    HANDLE file = nullptr;                    // nullptr = no file yet (or after Shutdown): lines stay in the ring
    uint64_t file_bytes = 0;
    std::atomic<HANDLE> wake{nullptr};        // auto-reset event, created by Init and never closed
    HANDLE thread = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> wake_pending{false};
    std::atomic<bool> idle{false};            // ct-log waits without a timeout; the next producer wakes it
    std::atomic<int> level{static_cast<int>(LogLevel::Info)};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> warnings{0};        // Warn + Error lines (WarningCount)
    wchar_t dir[kDirChars] = {};
    char path_utf8[kDirChars * 3] = {};
    char debug_chunk[4096 + 1] = {};          // OutputDebugStringA needs terminated text; used under io_lock
    char ring[kRingBytes] = {};
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

LogLevel ParseLevel(const char* s, LogLevel fallback) {
    if (_stricmp(s, "trace") == 0) return LogLevel::Trace;
    if (_stricmp(s, "debug") == 0) return LogLevel::Debug;
    if (_stricmp(s, "info") == 0) return LogLevel::Info;
    if (_stricmp(s, "warn") == 0) return LogLevel::Warn;
    if (_stricmp(s, "error") == 0) return LogLevel::Error;
    if (_stricmp(s, "off") == 0) return LogLevel::Off;
    return fallback;
}

// Per-thread cache of the formatted local date/time for the current second; the per-line cost is one
// GetSystemTimePreciseAsFileTime. The UTC->local bias is refreshed whenever the second changes.
struct TimeCache {
    int64_t utc_second = -1;
    int64_t bias_100ns = 0;
    char text[20];   // "YYYY-MM-DD HH:MM:SS"
};
thread_local TimeCache t_time;

size_t PutUInt(char* out, uint32_t v, int min_digits) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n < min_digits) tmp[n++] = '0';
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return static_cast<size_t>(n);
}

// "2026-10-17 12:03:44.123 [I] [capture ] desktop_duplicator.cpp:142 " -> returns its length (< 160 + file name).
size_t FormatPrefix(char* out, size_t cap, LogLevel level, const char* category, const char* file, int line) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t utc = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    TimeCache& tc = t_time;
    if (utc / 10'000'000 != tc.utc_second) {
        FILETIME local;
        FileTimeToLocalFileTime(&ft, &local);
        const int64_t l = static_cast<int64_t>((static_cast<uint64_t>(local.dwHighDateTime) << 32) | local.dwLowDateTime);
        tc.bias_100ns = l - utc;
        SYSTEMTIME st;
        FileTimeToSystemTime(&local, &st);
        std::snprintf(tc.text, sizeof(tc.text), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
        tc.utc_second = utc / 10'000'000;
    }
    const uint32_t ms = static_cast<uint32_t>(((utc + tc.bias_100ns) / 10'000) % 1000);
    const char* base = BaseName(file);
    const size_t base_len = std::min<size_t>(std::strlen(base), cap - 64);
    size_t n = 0;
    std::memcpy(out, tc.text, 19);
    n = 19;
    out[n++] = '.';
    n += PutUInt(out + n, ms, 3);
    out[n++] = ' ';
    out[n++] = '[';
    out[n++] = LevelLetter(level);
    out[n++] = ']';
    out[n++] = ' ';
    out[n++] = '[';
    size_t c = 0;
    if (category)
        for (; c < 8 && category[c]; ++c) out[n++] = category[c];
    for (; c < 8; ++c) out[n++] = ' ';
    out[n++] = ']';
    out[n++] = ' ';
    std::memcpy(out + n, base, base_len);
    n += base_len;
    out[n++] = ':';
    n += PutUInt(out + n, static_cast<uint32_t>(line < 0 ? 0 : line), 1);
    out[n++] = ' ';
    return n;
}

void Wake() {
    if (HANDLE ev = g_log.wake.load(std::memory_order_acquire)) SetEvent(ev);
}

// ---- consumer side (caller holds io_lock) --------------------------------------------------------------------

void WriteChunk(const char* data, size_t n) {
    if (g_log.file) {
        DWORD written = 0;
        WriteFile(g_log.file, data, static_cast<DWORD>(n), &written, nullptr);
        g_log.file_bytes += written;
    }
    if (IsDebuggerPresent()) {
        while (n > 0) {
            const size_t part = std::min(n, sizeof(g_log.debug_chunk) - 1);
            std::memcpy(g_log.debug_chunk, data, part);
            g_log.debug_chunk[part] = '\0';
            OutputDebugStringA(g_log.debug_chunk);
            data += part;
            n -= part;
        }
    }
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[40];
    swprintf(name, 40, L"camtesia-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return name;
}

// Keeps the newest (kKeepFiles - 1) logs, by creation time, so the file about to be created makes kKeepFiles.
// A log still open by a running instance (opened without FILE_SHARE_DELETE) fails DeleteFileW and is skipped.
void PruneLogs(const std::wstring& dir) {
    struct Item {
        std::wstring name;
        uint64_t created;
    };
    std::vector<Item> items;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\camtesia-*.log").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            items.push_back({fd.cFileName, (static_cast<uint64_t>(fd.ftCreationTime.dwHighDateTime) << 32) |
                                               fd.ftCreationTime.dwLowDateTime});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.created != b.created ? a.created < b.created : a.name < b.name;
    });
    const size_t keep = kKeepFiles - 1;
    for (size_t i = 0; i + keep < items.size(); ++i) DeleteFileW((dir + L"\\" + items[i].name).c_str());
}

// Prunes old logs and opens a fresh file; the current file (if any) is closed only once that succeeded, so a failed
// rotation keeps logging into the old file and the next drain retries. Caller holds io_lock.
bool OpenNewFileLocked() {
    const std::wstring dir = g_log.dir;
    if (dir.empty()) return false;
    PruneLogs(dir);
    const std::wstring base = dir + L"\\" + Stamp();
    std::wstring path = base + L".log";
    HANDLE file = INVALID_HANDLE_VALUE;
    // Same second (rotation, or a second instance about to exit): add -2, -3, ...
    for (int n = 1; n <= 20 && file == INVALID_HANDLE_VALUE; ++n) {
        if (n > 1) path = base + L"-" + std::to_wstring(n) + L".log";
        // No FILE_SHARE_DELETE: another instance's PruneLogs must not be able to delete the live log.
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    if (g_log.file) CloseHandle(g_log.file);
    g_log.file = file;
    g_log.file_bytes = 0;
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, g_log.path_utf8, sizeof(g_log.path_utf8), nullptr, nullptr);
    return true;
}

void DrainLocked() {
    if (!g_log.file) return;   // keep buffering until a file exists
    const uint64_t r = g_log.read.load(std::memory_order_relaxed);
    const uint64_t w = g_log.write.load(std::memory_order_acquire);
    g_log.wake_pending.store(false, std::memory_order_relaxed);
    if (w != r) {
        // Producers only append beyond `w` and never into [r, w) until `read` moves, so no lock is needed here.
        const size_t n = static_cast<size_t>(w - r);
        const size_t pos = static_cast<size_t>(r % kRingBytes);
        const size_t first = std::min(n, kRingBytes - pos);
        WriteChunk(g_log.ring + pos, first);
        if (n > first) WriteChunk(g_log.ring, n - first);
        g_log.read.store(w, std::memory_order_release);
    }
    if (g_log.file_bytes >= kRotateBytes) OpenNewFileLocked();
}

DWORD WINAPI LogThreadProc(void*) {
    SetThreadDescription(GetCurrentThread(), L"ct-log");
    Stats::Counter& dropped = Stats::C("log.dropped");
    while (!g_log.stop.load(std::memory_order_acquire)) {
        // Nothing buffered: sleep until a producer wakes us. `idle` is published before re-checking the ring (and the
        // producer commits before reading `idle`, both seq_cst), so a line written meanwhile is never left waiting.
        DWORD timeout = kFlushIntervalMs;
        if (g_log.write.load(std::memory_order_seq_cst) == g_log.read.load(std::memory_order_relaxed)) {
            g_log.idle.store(true, std::memory_order_seq_cst);
            if (g_log.write.load(std::memory_order_seq_cst) == g_log.read.load(std::memory_order_relaxed)) timeout = INFINITE;
            else g_log.idle.store(false, std::memory_order_relaxed);
        }
        const DWORD r = WaitForSingleObject(g_log.wake.load(std::memory_order_acquire), timeout);
        g_log.idle.store(false, std::memory_order_relaxed);
        // Woken from idle by an ordinary line: batch it with what follows for up to 500 ms, like before.
        if (r == WAIT_OBJECT_0 && timeout == INFINITE && !g_log.wake_pending.load(std::memory_order_relaxed) &&
            !g_log.stop.load(std::memory_order_acquire))
            continue;
        AcquireSRWLockExclusive(&g_log.io_lock);
        DrainLocked();
        ReleaseSRWLockExclusive(&g_log.io_lock);
        dropped.v.store(g_log.dropped.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return 0;
}

// Bounded wait: the crash handler flushes, and the crashing thread may be the one holding the lock.
bool TryLockIo() {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (TryAcquireSRWLockExclusive(&g_log.io_lock)) return true;
        Sleep(1);
    }
    return false;
}

} // namespace

void Log::Init(const char* log_dir_utf8) {
    char env[16] = {};
    if (GetEnvironmentVariableA("CT_LOG", env, sizeof(env)) > 0) SetLevel(ParseLevel(env, Level()));

    wchar_t dir[kDirChars] = {};
    if (!log_dir_utf8 || !*log_dir_utf8) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, log_dir_utf8, -1, dir, static_cast<int>(kDirChars));
    if (n <= 1) return;
    size_t len = static_cast<size_t>(n - 1);
    while (len > 0 && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) dir[--len] = L'\0';
    if (len == 0) return;
    CreateDirectoryW(dir, nullptr);

    if (!g_log.wake.load(std::memory_order_acquire))
        g_log.wake.store(CreateEventW(nullptr, FALSE, FALSE, nullptr), std::memory_order_release);

    AcquireSRWLockExclusive(&g_log.io_lock);
    wcsncpy_s(g_log.dir, dir, _TRUNCATE);
    const bool opened = OpenNewFileLocked();
    if (opened) DrainLocked();   // lines logged before Init
    ReleaseSRWLockExclusive(&g_log.io_lock);
    if (!opened) return;

    if (!g_log.thread) {
        g_log.stop.store(false, std::memory_order_release);
        g_log.thread = CreateThread(nullptr, 0, &LogThreadProc, nullptr, 0, nullptr);
    }
}

void Log::SetLevel(LogLevel level) { g_log.level.store(static_cast<int>(level), std::memory_order_relaxed); }

LogLevel Log::Level() { return static_cast<LogLevel>(g_log.level.load(std::memory_order_relaxed)); }

void Log::Write(LogLevel level, const char* category, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) < g_log.level.load(std::memory_order_relaxed) || level >= LogLevel::Off) return;
    if (level >= LogLevel::Warn) g_log.warnings.fetch_add(1, std::memory_order_relaxed);

    char text[kMaxLine];
    size_t len = FormatPrefix(text, sizeof(text), level, category, file, line);
    va_list args;
    va_start(args, fmt);
    const int m = std::vsnprintf(text + len, sizeof(text) - len, fmt ? fmt : "", args);
    va_end(args);
    if (m > 0) len = std::min(len + static_cast<size_t>(m), sizeof(text) - 3);   // truncated lines keep "\r\n"
    text[len++] = '\r';
    text[len++] = '\n';

    AcquireSRWLockExclusive(&g_log.ring_lock);
    const uint64_t w = g_log.write.load(std::memory_order_relaxed);
    const uint64_t used = w - g_log.read.load(std::memory_order_acquire);
    if (kRingBytes - used < len) {
        ReleaseSRWLockExclusive(&g_log.ring_lock);
        g_log.dropped.fetch_add(1, std::memory_order_relaxed);
        if (!g_log.wake_pending.exchange(true, std::memory_order_relaxed)) Wake();
        return;
    }
    const size_t pos = static_cast<size_t>(w % kRingBytes);
    const size_t first = std::min(len, kRingBytes - pos);
    std::memcpy(g_log.ring + pos, text, first);
    if (len > first) std::memcpy(g_log.ring, text + first, len - first);
    g_log.write.store(w + len, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_log.ring_lock);

    if (level >= LogLevel::Warn) {
        g_log.wake_pending.store(true, std::memory_order_relaxed);
        Wake();
    } else if (used + len > kRingBytes / 2 && !g_log.wake_pending.exchange(true, std::memory_order_relaxed)) {
        Wake();
    } else {
        // The writer sleeps without a timeout while the ring is empty: the first line after that wakes it once.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (g_log.idle.load(std::memory_order_relaxed) && g_log.idle.exchange(false, std::memory_order_relaxed)) Wake();
    }
}

void Log::Tick() {}   // kept for API stability; the ct-log thread flushes on its own

void Log::Flush() {
    if (!TryLockIo()) return;
    DrainLocked();
    if (g_log.file) FlushFileBuffers(g_log.file);
    ReleaseSRWLockExclusive(&g_log.io_lock);
}

void Log::Shutdown() {
    if (g_log.thread) {
        g_log.stop.store(true, std::memory_order_release);
        Wake();
        WaitForSingleObject(g_log.thread, 2000);
        CloseHandle(g_log.thread);
        g_log.thread = nullptr;
    }
    AcquireSRWLockExclusive(&g_log.io_lock);
    DrainLocked();
    if (g_log.file) {
        FlushFileBuffers(g_log.file);
        CloseHandle(g_log.file);
        g_log.file = nullptr;
    }
    ReleaseSRWLockExclusive(&g_log.io_lock);
}

const char* Log::FilePathUtf8() { return g_log.path_utf8; }

uint64_t Log::WarningCount() { return g_log.warnings.load(std::memory_order_relaxed); }

} // namespace ct
