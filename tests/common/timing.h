#pragma once
// tests/common/timing.h
// docs/06: timing budgets are enforced only with CT_STRICT_TIMING=1 (acceptance runs on an idle machine); otherwise
// tests fail only on gross regressions so loaded machines and parallel builds stay green.

#include <cstdlib>

namespace ct::test {

inline bool StrictTiming() { return std::getenv("CT_STRICT_TIMING") != nullptr; }

// The budget under CT_STRICT_TIMING, else `gross` (a limit that only a real regression or a hang exceeds).
inline double TimingLimit(double budget, double gross) { return StrictTiming() ? budget : gross; }

} // namespace ct::test
