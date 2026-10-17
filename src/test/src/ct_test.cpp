#include <ct/test/ct_test.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include <windows.h>
#include <crtdbg.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ct::test {

namespace {

TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;

struct RunState {
    const TestCase* current = nullptr;
    int failures_in_test = 0;
    bool skipped = false;
    char skip_reason[256] = {};
    bool update_golden = false;
    bool verbose = false;
};
RunState g_run;

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double QpcToMs(int64_t ticks) {
    static int64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Unit:   return "unit";
    case Kind::Device: return "device";
    case Kind::Golden: return "golden";
    case Kind::Bench:  return "bench";
    }
    return "?";
}

// Runs one test; returns true when it failed.
bool RunOne(const TestCase& tc) {
    g_run.current = &tc;
    g_run.failures_in_test = 0;
    g_run.skipped = false;
    g_run.skip_reason[0] = '\0';

    if ((tc.kind == Kind::Device || tc.kind == Kind::Golden) && IsCi()) {
        std::printf("[SKIP] %s: CT_CI is set (needs hardware)\n", tc.name);
        return false;
    }

    const int64_t start = QpcNow();
    if (tc.kind == Kind::Bench) {
        tc.bench_fn(tc.bench_n);
    } else {
        tc.fn();
    }
    const double ms = QpcToMs(QpcNow() - start);

    if (g_run.failures_in_test > 0) {
        std::printf("[FAIL] %s (%.1f ms)\n", tc.name, ms);
        return true;
    }
    if (g_run.skipped) {
        std::printf("[SKIP] %s: %s\n", tc.name, g_run.skip_reason);
        return false;
    }
    if (tc.kind == Kind::Bench) {
        const double ns_per_op = tc.bench_n > 0 ? ms * 1e6 / static_cast<double>(tc.bench_n) : 0.0;
        const double ops_per_s = ms > 0 ? static_cast<double>(tc.bench_n) * 1000.0 / ms : 0.0;
        std::printf("[BENCH] %s: %lld ops in %.1f ms, %.3f M ops/s, %.2f ns/op\n", tc.name,
                    static_cast<long long>(tc.bench_n), ms, ops_per_s / 1e6, ns_per_op);
        return false;
    }
    std::printf("[PASS] %s (%.1f ms)\n", tc.name, ms);
    return false;
}

void PrintUsage(const char* exe) {
    std::printf("usage: %s [--filter <substr>] [--list] [--bench] [--repeat N] [--update-golden] [--verbose]\n",
                exe ? exe : "test");
}

} // namespace

Registrar::Registrar(TestCase* test_case) {
    test_case->next = nullptr;
    if (g_tail) g_tail->next = test_case; else g_head = test_case;
    g_tail = test_case;
}

void ReportFailure(const char* file, int line, const char* expr, const std::string& detail) {
    ++g_run.failures_in_test;
    if (detail.empty())
        std::printf("[FAIL] %s(%d): %s\n", file, line, expr);
    else
        std::printf("[FAIL] %s(%d): %s  %s\n", file, line, expr, detail.c_str());
    std::fflush(stdout);
}

void ReportSkip(const char* reason) {
    g_run.skipped = true;
    std::snprintf(g_run.skip_reason, sizeof(g_run.skip_reason), "%s", reason ? reason : "");
}

bool UpdateGolden() { return g_run.update_golden; }
bool Verbose() { return g_run.verbose; }
const char* CurrentTestName() { return g_run.current ? g_run.current->name : ""; }

bool CheckGolden(const char* file, int line, const void* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) {
        ReportFailure(file, line, "CT_GOLDEN_IMAGE", "empty image");
        return false;
    }
    ReportSkip("golden image comparison is implemented in P08");
    return false;
}

std::string FormatPair(const std::string& left, const std::string& right) {
    return "left=" + left + " right=" + right;
}

std::string HrDetail(long hr) {
    return "hr=" + ::ct::HrToString(hr);
}

bool HrFailed(long hr) { return hr < 0; }

bool IsCi() {
    char buf[8];
    return GetEnvironmentVariableA("CT_CI", buf, sizeof(buf)) > 0;
}

int RunAll(int argc, char** argv) {
    // Never block an unattended run on a CRT assert dialog or the Windows error-reporting UI.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* filter = nullptr;
    bool list = false;
    bool bench = false;
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(a, "--list") == 0) list = true;
        else if (std::strcmp(a, "--bench") == 0) bench = true;
        else if (std::strcmp(a, "--repeat") == 0 && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(a, "--update-golden") == 0) g_run.update_golden = true;
        else if (std::strcmp(a, "--verbose") == 0) g_run.verbose = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { PrintUsage(argv[0]); return 0; }
        else { std::printf("unknown argument: %s\n", a); PrintUsage(argv[0]); return 1; }
    }
    ::ct::Log::SetLevel(g_run.verbose ? LogLevel::Trace : LogLevel::Warn);

    if (list) {
        for (const TestCase* tc = g_head; tc; tc = tc->next)
            if (!filter || std::strstr(tc->name, filter)) std::printf("%-7s %s\n", KindName(tc->kind), tc->name);
        return 0;
    }

    int failures = 0, passed = 0, ran = 0;
    const int64_t start = QpcNow();
    for (int r = 0; r < repeat; ++r) {
        for (const TestCase* tc = g_head; tc; tc = tc->next) {
            if (filter && !std::strstr(tc->name, filter)) continue;
            if ((tc->kind == Kind::Bench) != bench) continue;   // --bench runs benches only
            ++ran;
            if (RunOne(*tc)) ++failures; else ++passed;
        }
    }
    std::printf("%d test(s), %d passed or skipped, %d failed, %.1f ms\n", ran, passed, failures,
                QpcToMs(QpcNow() - start));
    return failures;
}

} // namespace ct::test
