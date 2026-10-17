#pragma once
// ct/test/ct_test.h
// Minimal test harness (no third-party framework). Tests register themselves through static objects that link
// into a list; ct::test::RunAll() runs them. One test exe per module, each registered with ctest.
// Thread model: tests run sequentially on the main thread; CT_CHECK* may only be used from that thread.
//
//   CT_TEST(name) { ... }               unit test, always runs
//   CT_TEST_DEVICE(name) { ... }        needs GPU/audio/webcam: SKIPPED when CT_CI is set (or call CT_SKIP)
//   CT_TEST_GOLDEN(name) { ... }        compares an image with tests/data/golden/<name>.png (stubbed until P08)
//   CT_BENCH(name, N) { ... }           runs only with --bench; loop CT_BENCH_N times; prints ops/s and ns/op
//
// Runner flags: --filter <substr> --list --bench --repeat N --update-golden --verbose. Exit code = failures.
// If a test exe defines no main(), ct_test provides one that calls RunAll().

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace ct::test {

enum class Kind : uint8_t { Unit, Device, Golden, Bench };

using TestFn = void (*)();
using BenchFn = void (*)(int64_t n);

struct TestCase {
    const char* name;
    const char* file;
    int line;
    Kind kind;
    TestFn fn;
    BenchFn bench_fn;
    int64_t bench_n;
    TestCase* next;
};

// Appends to the global list; called from the static registrar objects the macros create.
struct Registrar {
    Registrar(TestCase* test_case);
};

int RunAll(int argc, char** argv);

// Results reporting for the macros below.
void ReportFailure(const char* file, int line, const char* expr, const std::string& detail);
void ReportSkip(const char* reason);
bool UpdateGolden();          // --update-golden was passed
bool Verbose();               // --verbose was passed
const char* CurrentTestName();

// Golden image comparison (RGBA8, tightly packed). Until P08 this marks the test SKIPPED.
// Returns false when the test must stop (failure or skip).
bool CheckGolden(const char* file, int line, const void* rgba, int width, int height);

// Value formatting for CT_CHECK_EQ / CT_CHECK_NEAR diagnostics.
inline std::string ToString(const std::string& v) { return "\"" + v + "\""; }
inline std::string ToString(std::string_view v) { return "\"" + std::string(v) + "\""; }
inline std::string ToString(const char* v) { return v ? "\"" + std::string(v) + "\"" : std::string("(null)"); }
inline std::string ToString(char* v) { return ToString(static_cast<const char*>(v)); }
inline std::string ToString(bool v) { return v ? "true" : "false"; }
inline std::string ToString(std::nullptr_t) { return "nullptr"; }
template <typename T>
std::string ToString(const T& v) {
    char buf[64];
    if constexpr (std::is_enum_v<T>) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(static_cast<std::underlying_type_t<T>>(v)));
    } else if constexpr (std::is_floating_point_v<T>) {
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<T>) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    } else if constexpr (std::is_pointer_v<T>) {
        std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(v));
    } else {
        std::snprintf(buf, sizeof(buf), "<%zu-byte value>", sizeof(T));
    }
    return std::string(buf);
}

std::string FormatPair(const std::string& left, const std::string& right);
std::string HrDetail(long hr);
bool IsCi();                  // CT_CI environment variable is set
// A function (not an inline `hr < 0`) so constant arguments do not trigger C4127 at the call site.
bool HrFailed(long hr);

} // namespace ct::test

#define CT_TEST_REGISTER_(name, kind)                                                                     \
    static void ct_test_fn_##name();                                                                      \
    static ::ct::test::TestCase ct_test_case_##name{#name, __FILE__, __LINE__, kind, &ct_test_fn_##name,   \
                                                     nullptr, 0, nullptr};                                \
    static ::ct::test::Registrar ct_test_reg_##name(&ct_test_case_##name);                                \
    static void ct_test_fn_##name()

#define CT_TEST(name)        CT_TEST_REGISTER_(name, ::ct::test::Kind::Unit)
#define CT_TEST_DEVICE(name) CT_TEST_REGISTER_(name, ::ct::test::Kind::Device)
#define CT_TEST_GOLDEN(name) CT_TEST_REGISTER_(name, ::ct::test::Kind::Golden)

#define CT_BENCH(name, n)                                                                                 \
    static void ct_bench_fn_##name(int64_t ct_bench_n);                                                   \
    static ::ct::test::TestCase ct_test_case_##name{#name, __FILE__, __LINE__, ::ct::test::Kind::Bench,     \
                                                     nullptr, &ct_bench_fn_##name, (n), nullptr};         \
    static ::ct::test::Registrar ct_test_reg_##name(&ct_test_case_##name);                                \
    static void ct_bench_fn_##name(int64_t ct_bench_n)
#define CT_BENCH_N ct_bench_n

#define CT_CHECK(expr)                                                                                    \
    do {                                                                                                  \
        if (!(expr)) ::ct::test::ReportFailure(__FILE__, __LINE__, #expr, std::string());                 \
    } while (0)

#define CT_CHECK_EQ(a, b)                                                                                 \
    do {                                                                                                  \
        const auto& ct_l_ = (a);                                                                          \
        const auto& ct_r_ = (b);                                                                          \
        if (!(ct_l_ == ct_r_))                                                                            \
            ::ct::test::ReportFailure(__FILE__, __LINE__, #a " == " #b,                                   \
                                      ::ct::test::FormatPair(::ct::test::ToString(ct_l_), ::ct::test::ToString(ct_r_))); \
    } while (0)

#define CT_CHECK_NEAR(a, b, eps)                                                                          \
    do {                                                                                                  \
        const double ct_l_ = static_cast<double>(a);                                                      \
        const double ct_r_ = static_cast<double>(b);                                                      \
        const double ct_d_ = ct_l_ > ct_r_ ? ct_l_ - ct_r_ : ct_r_ - ct_l_;                               \
        if (!(ct_d_ <= static_cast<double>(eps)))                                                         \
            ::ct::test::ReportFailure(__FILE__, __LINE__, "|" #a " - " #b "| <= " #eps,                   \
                                      ::ct::test::FormatPair(::ct::test::ToString(ct_l_), ::ct::test::ToString(ct_r_))); \
    } while (0)

#define CT_CHECK_HR(hr_expr)                                                                              \
    do {                                                                                                  \
        const long ct_hr_ = static_cast<long>(hr_expr);                                                   \
        if (::ct::test::HrFailed(ct_hr_)) ::ct::test::ReportFailure(__FILE__, __LINE__, #hr_expr, ::ct::test::HrDetail(ct_hr_)); \
    } while (0)

// Aborts the current test (returns from the test function) when expr is false.
#define CT_REQUIRE(expr)                                                                                  \
    do {                                                                                                  \
        if (!(expr)) {                                                                                    \
            ::ct::test::ReportFailure(__FILE__, __LINE__, #expr, std::string("(required)"));              \
            return;                                                                                       \
        }                                                                                                 \
    } while (0)

#define CT_SKIP(reason)                                                                                   \
    do {                                                                                                  \
        ::ct::test::ReportSkip(reason);                                                                   \
        return;                                                                                           \
    } while (0)

#define CT_FAIL(msg)                                                                                      \
    do {                                                                                                  \
        ::ct::test::ReportFailure(__FILE__, __LINE__, "CT_FAIL", std::string(msg));                       \
    } while (0)

#define CT_GOLDEN_IMAGE(rgba, width, height)                                                              \
    do {                                                                                                  \
        if (!::ct::test::CheckGolden(__FILE__, __LINE__, (rgba), (width), (height))) return;              \
    } while (0)
