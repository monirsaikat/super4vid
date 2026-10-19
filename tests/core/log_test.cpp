// ct/core/{log,stats,trace}.h: ring logger (caller cost, file contents, rotation), counters/timings, ETW spans.
#define CT_LOG_CATEGORY "logtest"
#include <ct/core/fs.h>
#include <cstdlib>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/core/trace.h>
#include <ct/test/ct_test.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Counts lines containing `marker` in every super4vid-*.log of `dir`.
size_t CountLines(const std::string& dir, const char* marker, size_t* files = nullptr) {
    std::vector<ct::fs::Entry> entries;
    ct::fs::List(dir, entries);
    size_t count = 0, logs = 0;
    for (const auto& e : entries) {
        if (!ct::StartsWith(e.name, "super4vid-") || !ct::EndsWith(e.name, ".log")) continue;
        ++logs;
        std::string text;
        ct::fs::ReadAll(ct::PathJoin(dir, e.name), text);
        for (size_t pos = 0; (pos = text.find(marker, pos)) != std::string::npos; pos += std::strlen(marker)) ++count;
    }
    if (files) *files = logs;
    return count;
}

// The harness runs with level Warn and no log file; each logging test gets its own directory and restores that.
struct LogSession {
    std::string dir = ct::fs::TempPath("");
    ct::LogLevel saved = ct::Log::Level();
    explicit LogSession(ct::LogLevel level) {
        ct::fs::CreateDirectories(dir);
        ct::Log::SetLevel(level);
        ct::Log::Init(dir.c_str());
    }
    ~LogSession() {
        ct::Log::Shutdown();
        ct::Log::SetLevel(saved);
        ct::fs::DeleteTree(dir);
    }
};

} // namespace

CT_TEST(log_line_format_and_warn_wakeup) {
    LogSession session(ct::LogLevel::Info);
    CT_REQUIRE(ct::StartsWith(ct::Log::FilePathUtf8(), session.dir.c_str()));
    CT_LOG_DEBUG("filtered out %d", 1);
    CT_LOG_INFO("hello %s %d", "world", 42);
    CT_LOG_WARN("warned \xC3\xA9");
    // A Warn wakes the ct-log thread: the line reaches the file well before the 500 ms period, without Flush().
    const int64_t start = ct::Clock::NowUs();
    while (CountLines(session.dir, "warned") == 0 && ct::Clock::NowUs() - start < 400'000) Sleep(5);
    const int64_t waited_ms = (ct::Clock::NowUs() - start) / 1000;
    CT_CHECK(CountLines(session.dir, "warned \xC3\xA9") == 1);
    CT_CHECK(waited_ms < 300);
    std::string text;
    CT_REQUIRE(ct::fs::ReadAll(ct::Log::FilePathUtf8(), text));
    CT_CHECK(text.find("filtered out") == std::string::npos);
    const size_t hello = text.find("hello world 42\r\n");
    CT_REQUIRE(hello != std::string::npos);
    const size_t line_start = text.rfind('\n', hello) == std::string::npos ? 0 : text.rfind('\n', hello) + 1;
    const std::string line = text.substr(line_start, hello - line_start);
    // "2026-10-17 12:03:44.123 [I] [logtest ] log_test.cpp:57 "
    CT_CHECK(line.size() > 24 && line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' &&
             line[19] == '.');
    CT_CHECK(line.find(" [I] [logtest ] log_test.cpp:") == 23);
    CT_CHECK(text.find("[W] [logtest ]") != std::string::npos);
    ct::Log::Tick();   // still exists, does nothing
}

// P11 11.1: with an empty ring the ct-log thread waits without a timeout; an Info line written after an idle
// period still reaches the file through the 500 ms drain (no Warn, no Flush), every time.
CT_TEST(log_info_after_idle_reaches_file) {
    LogSession session(ct::LogLevel::Info);
    for (int round = 0; round < 3; ++round) {
        Sleep(700);   // let the writer drain and go idle
        char marker[32];
        std::snprintf(marker, sizeof(marker), "idle-line-%d", round);
        CT_LOG_INFO("%s", marker);
        const int64_t start = ct::Clock::NowUs();
        while (CountLines(session.dir, marker) == 0 && ct::Clock::NowUs() - start < 3'000'000) Sleep(10);
        CT_CHECK_EQ(CountLines(session.dir, marker), static_cast<size_t>(1));
        const int64_t waited_ms = (ct::Clock::NowUs() - start) / 1000;
        CT_CHECK(waited_ms >= 300);    // batched, not written per line
        CT_CHECK(waited_ms < 2500);    // gross: the drain period is 500 ms
    }
}

CT_TEST(log_100k_lines_from_time_critical_thread) {
    LogSession session(ct::LogLevel::Info);
    constexpr int kLines = 100'000;
    double caller_ms = 0;
    ct::Thread t;
    ct::Thread::Options opt;
    opt.name = "ct-test-critical";
    opt.priority = THREAD_PRIORITY_TIME_CRITICAL;
    opt.com_mta = false;
    const uint64_t dropped_before = ct::Stats::C("log.dropped").Get();
    CT_REQUIRE(t.Start(opt, [&] {
        ct::Stopwatch sw;
        sw.Start();
        for (int i = 0; i < kLines; ++i) CT_LOG_INFO("critical-line %d payload %s", i, "0123456789abcdef");
        caller_ms = sw.ElapsedMs();
    }));
    CT_REQUIRE(t.Join(10000));
    std::printf("    100,000 lines from a TIME_CRITICAL thread: %.1f ms on the caller (%.0f ns/line)\n", caller_ms,
                caller_ms * 1e6 / kLines);
#if defined(_DEBUG)
    CT_CHECK(caller_ms < 400.0);   // /Od /RTC1 and the debug CRT's vsnprintf; the 50 ms budget is a Release number
#else
    // The 50 ms budget is measured on an idle machine (25-27 ms there; P01 numbers in PROGRESS.md).
    // Under parallel builds/benches it reached 37-224 ms, so warn above budget and fail only on a gross regression.
    if (caller_ms >= 50.0) std::printf("    [WARN] above the 50 ms budget (machine under load?)\n");
    if (std::getenv("CT_STRICT_TIMING") != nullptr) CT_CHECK(caller_ms < 50.0);   // acceptance run on an idle machine
    else CT_CHECK(caller_ms < 2000.0);                                             // gross regression only
#endif

    // Every line is either in the files or counted as dropped (the ct-log thread publishes the counter).
    size_t written = 0, files = 0;
    uint64_t dropped = 0;
    const int64_t start = ct::Clock::NowUs();
    do {
        ct::Log::Flush();
        written = CountLines(session.dir, "critical-line", &files);
        dropped = ct::Stats::C("log.dropped").Get() - dropped_before;
        if (written + dropped == kLines) break;
        Sleep(50);
    } while (ct::Clock::NowUs() - start < 3'000'000);
    std::printf("    written %zu, dropped %llu, files %zu\n", written, static_cast<unsigned long long>(dropped), files);
    CT_CHECK_EQ(written + dropped, size_t(kLines));
    CT_CHECK(written > 0);
}

CT_TEST(log_rotation_keeps_five) {
    LogSession session(ct::LogLevel::Info);
    // ~66 MB of lines: rotation at 10 MB must leave at most 5 files. Paced so the ring never fills.
    const std::string payload(1000, 'x');
    for (int i = 0; i < 66'000; ++i) {
        CT_LOG_INFO("rot %d %s", i, payload.c_str());
        if (i % 400 == 399) ct::Log::Flush();
    }
    ct::Log::Flush();
    size_t files = 0;
    const size_t lines = CountLines(session.dir, "rot ", &files);
    CT_CHECK(files >= 2 && files <= 5);
    CT_CHECK(lines > 30'000);   // older files were pruned; the newest ones are complete
    std::vector<ct::fs::Entry> entries;
    ct::fs::List(session.dir, entries);
    bool sizes_ok = true;
    for (const auto& e : entries)
        if (e.size > (11u << 20)) sizes_ok = false;   // rotation happens after a drain crosses 10 MB (drains <= 1 MB)
    CT_CHECK(sizes_ok);
}

// Review fix: another instance's pruning (DeleteFileW) must not be able to remove the live log.
CT_TEST(log_live_file_cannot_be_deleted) {
    LogSession session(ct::LogLevel::Info);
    const std::wstring path = ct::Utf8ToWide(ct::Log::FilePathUtf8());
    CT_REQUIRE(!path.empty());
    CT_CHECK(!DeleteFileW(path.c_str()));
    CT_CHECK_EQ(GetLastError(), DWORD(ERROR_SHARING_VIOLATION));
    CT_LOG_WARN("still here");
    ct::Log::Flush();
    CT_CHECK_EQ(CountLines(session.dir, "still here"), size_t(1));
}

// Review fix: when rotation cannot create the next file, logging continues into the current one (it used to close
// it first and then drop every later line). Read-only files occupy every name rotation may try in the next 30 s.
CT_TEST(log_failed_rotation_keeps_old_file) {
    LogSession session(ct::LogLevel::Info);
    struct Blockers {
        std::vector<std::wstring> paths;
        ~Blockers() {
            for (const auto& p : paths) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        }
    } blockers;
    const std::wstring dir = ct::Utf8ToWide(session.dir);
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    for (int s = 0; s <= 30; ++s) {
        const uint64_t t = now + static_cast<uint64_t>(s) * 10'000'000ull;
        FILETIME utc{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)}, local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&utc, &local);
        FileTimeToSystemTime(&local, &st);
        wchar_t base[64];
        swprintf(base, 64, L"super4vid-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        for (int n = 1; n <= 20; ++n) {
            std::wstring path = dir + L"\\" + base + (n > 1 ? L"-" + std::to_wstring(n) : std::wstring()) + L".log";
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_READONLY, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;   // the live log already has this name
            CloseHandle(h);
            blockers.paths.push_back(path);
        }
    }
    const std::string payload(1000, 'x');
    for (int i = 0; i < 11'000; ++i) {   // ~11 MB: crosses the 10 MB rotation mark
        CT_LOG_INFO("rot %d %s", i, payload.c_str());
        if (i % 400 == 399) ct::Log::Flush();
    }
    ct::Log::Flush();
    CT_LOG_INFO("after failed rotation");
    ct::Log::Flush();
    CT_CHECK_EQ(CountLines(session.dir, "after failed rotation"), size_t(1));
    CT_CHECK_EQ(CountLines(session.dir, "rot "), size_t(11'000));
}

CT_TEST(stats_counters_gauges_timings) {
    ct::Stats::Counter& c = ct::Stats::C("test.frames");
    CT_CHECK_EQ(&c, &ct::Stats::C("test.frames"));   // interned
    const uint64_t base = c.Get();
    c.Add();
    c.Add(4);
    CT_CHECK_EQ(c.Get(), base + 5);
    ct::Stats::G("test.fps").Set(59.94);
    CT_CHECK_EQ(ct::Stats::G("test.fps").Get(), 59.94);
    ct::Stats::Timing& t = ct::Stats::T("test.convert.ms");
    t.Reset();
    t.AddUs(400);
    t.AddUs(1300);
    t.AddUs(100);
    CT_CHECK_NEAR(t.AvgMs(), 0.6, 1e-9);
    CT_CHECK_NEAR(t.MaxMs(), 1.3, 1e-9);
    CT_CHECK_EQ(t.Count(), uint64_t(3));
    // Same name, different kinds: separate entries.
    CT_CHECK(static_cast<void*>(&ct::Stats::C("test.same")) != static_cast<void*>(&ct::Stats::T("test.same")));
    std::string dump;
    ct::Stats::Dump(dump);
    CT_CHECK(dump.find(ct::Format("test.frames %llu", static_cast<unsigned long long>(base + 5))) != std::string::npos);
    CT_CHECK(dump.find("test.fps 59.94") != std::string::npos);
    CT_CHECK(dump.find("test.convert.ms avg 0.60 max 1.30") != std::string::npos);
    CT_CHECK(dump.find("  ") != std::string::npos);
    CT_CHECK(dump.find("test.convert.ms") < dump.find("test.fps"));   // sorted by name
    ct::Stats::ResetTimings();
    CT_CHECK_EQ(t.Count(), uint64_t(0));
    CT_CHECK_EQ(t.AvgMs(), 0.0);

    // Concurrent adds from 4 threads.
    ct::Stats::Counter& hot = ct::Stats::C("test.hot");
    const uint64_t hot_base = hot.Get();
    ct::Thread workers[4];
    for (ct::Thread& w : workers)
        CT_REQUIRE(w.Start({}, [&] {
            for (int i = 0; i < 100'000; ++i) hot.Add();
        }));
    for (ct::Thread& w : workers) CT_CHECK(w.Join(10000));
    CT_CHECK_EQ(hot.Get() - hot_base, uint64_t(400'000));
}

CT_TEST(trace_scope_feeds_stats) {
    ct::Trace::Register();
    ct::Trace::Register();   // idempotent
    ct::Trace::Event("test.event", 7);
    ct::Stats::T("test.scope").Reset();
    {
        CT_TRACE_SCOPE("test.scope");   // timings off: not recorded
    }
    CT_CHECK_EQ(ct::Stats::T("test.scope").Count(), uint64_t(0));
    ct::Stats::SetScopeTimings(true);
    for (int i = 0; i < 3; ++i) {
        CT_TRACE_SCOPE("test.scope");
        Sleep(2);
    }
    ct::Stats::SetScopeTimings(false);
    CT_CHECK_EQ(ct::Stats::T("test.scope").Count(), uint64_t(3));
    CT_CHECK(ct::Stats::T("test.scope").MaxMs() >= 1.0);
    ct::Trace::Unregister();
    ct::Trace::Unregister();
    ct::Trace::Event("after.unregister");   // dropped, no crash
    CT_CHECK(!ct::Trace::Enabled());
}
