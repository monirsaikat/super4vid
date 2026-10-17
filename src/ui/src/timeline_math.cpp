// Timeline geometry helpers.
#include <ct/ui/timeline_math.h>

#include <ct/core/time.h>

#include <cmath>
#include <cstdio>

namespace ct::ui {

TimeUs TimelineView::T(float x) const {
    return scroll_us + static_cast<TimeUs>(std::llround(static_cast<double>(x - origin_x) * 1e6 / pps));
}

TimeUs TimelineView::PxToUs(float px) const { return static_cast<TimeUs>(std::llround(static_cast<double>(px) * 1e6 / pps)); }

TimeUs ZoomScroll(TimeUs scroll_us, TimeUs t_anchor, double old_pps, double new_pps) {
    if (new_pps <= 0) return scroll_us;
    const double d = static_cast<double>(t_anchor - scroll_us) * (1.0 - old_pps / new_pps);
    return scroll_us + static_cast<TimeUs>(std::llround(d));
}

double ClampPps(double pps) { return pps < kMinPps ? kMinPps : pps > kMaxPps ? kMaxPps : pps; }

double FitPps(TimeUs duration, float width) {
    if (duration <= 0 || width <= 1) return 100;
    return ClampPps(static_cast<double>(width) * 0.96 / (static_cast<double>(duration) / 1e6));
}

TickUnit PickTickUnit(double pps, int fps_num, int fps_den, float min_px) {
    const TimeUs frame = (fps_num > 0 && fps_den > 0) ? FrameTimeUs(1, fps_num, fps_den) : 16'667;
    const TimeUs units[] = {frame, 100'000, 500'000, 1'000'000, 5'000'000, 10'000'000, 30'000'000, 60'000'000, 300'000'000};
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); ++i) {
        const double px = static_cast<double>(units[i]) * pps / 1e6;
        if (px >= min_px) return TickUnit{units[i], i == 0 ? 0 : 5, i == 0};
    }
    return TickUnit{300'000'000, 5, false};
}

const char* FormatTick(char* buf, unsigned n, TimeUs t, const TickUnit& u, int fps_num, int fps_den) {
    if (t < 0) t = 0;
    const long long s = static_cast<long long>(t / kUsPerSecond);
    const TimeUs rem = t - static_cast<TimeUs>(s) * kUsPerSecond;
    if (u.frames) {
        const int num = fps_num > 0 ? fps_num : 30, den = fps_den > 0 ? fps_den : 1;
        const long long f = static_cast<long long>(FrameIndexAt(rem, num, den));
        std::snprintf(buf, n, "%lld:%02lld:%02lldf", s / 60, s % 60, f);
    } else if (u.major_us < kUsPerSecond) {
        std::snprintf(buf, n, "%lld:%02lld.%02lld", s / 60, s % 60, static_cast<long long>(rem / 10'000));
    } else if (s >= 3600) {
        std::snprintf(buf, n, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    } else {
        std::snprintf(buf, n, "%lld:%02lld", s / 60, s % 60);
    }
    return buf;
}

} // namespace ct::ui
