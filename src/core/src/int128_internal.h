#pragma once
// Private 128-bit multiply/divide helpers shared by strings.cpp and time.cpp.
#include <intrin.h>

#include <cstdint>
#include <limits>

namespace ct::detail {

// floor(a * b / c) with a 128-bit intermediate. b >= 0, c > 0. Saturates instead of overflowing.
inline int64_t MulDivFloor(int64_t a, int64_t b, int64_t c) {
    const bool negative = a < 0;
    const uint64_t ua = negative ? 0ull - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
    uint64_t hi = 0;
    const uint64_t lo = _umul128(ua, static_cast<uint64_t>(b), &hi);
    const uint64_t uc = static_cast<uint64_t>(c);
    if (hi >= uc) return negative ? (std::numeric_limits<int64_t>::min)() : (std::numeric_limits<int64_t>::max)();
    uint64_t rem = 0;
    uint64_t q = _udiv128(hi, lo, uc, &rem);
    if (negative) {
        if (rem != 0) ++q;
        if (q > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return (std::numeric_limits<int64_t>::min)();
        return -static_cast<int64_t>(q);
    }
    if (q > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return (std::numeric_limits<int64_t>::max)();
    return static_cast<int64_t>(q);
}

// Exact floor(x / c) for c > 0.
inline int64_t FloorDiv(int64_t x, int64_t c) {
    int64_t q = x / c;
    if ((x % c != 0) && (x < 0)) --q;
    return q;
}

// Frame n starts at floor(n * 1e6 * den / num). Returns the largest n whose start is <= t.
inline int64_t FrameTimeUsImpl(int64_t index, int num, int den) {
    if (num <= 0 || den <= 0) return 0;
    return MulDivFloor(index, 1'000'000ll * den, num);
}

inline int64_t FrameIndexAtImpl(int64_t t_us, int num, int den) {
    if (num <= 0 || den <= 0) return 0;
    // start(n) <= t  <=>  floor(n*D/N) <= t  <=>  n*D < (t+1)*N  <=>  n <= floor(((t+1)*N - 1) / D) = ceil((t+1)*N / D) - 1
    if (t_us == (std::numeric_limits<int64_t>::max)()) return MulDivFloor(t_us, num, 1'000'000ll * den);
    return -MulDivFloor(-(t_us + 1), num, 1'000'000ll * den) - 1;
}

} // namespace ct::detail
