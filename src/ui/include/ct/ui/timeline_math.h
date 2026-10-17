#pragma once
// ct/ui/timeline_math.h
// Pure timeline geometry (docs/phases/P09 9.4): time <-> x mapping, zoom about a point, ruler tick selection.
// Thread model: pure functions.

#include <ct/core/types.h>

namespace ct::ui {

constexpr double kMinPps = 2, kMaxPps = 2000;

struct TimelineView {
    float origin_x = 0;   // screen x of the lanes' left edge
    float width = 0;      // lanes width in px
    double pps = 100;     // pixels per second (already DPI-scaled when used for screen px)
    TimeUs scroll_us = 0; // time at origin_x

    // x(t) = origin_x + (t - scroll_us) * pps / 1e6
    float X(TimeUs t) const { return origin_x + static_cast<float>(static_cast<double>(t - scroll_us) * pps / 1e6); }
    // t(x), rounded to the nearest microsecond
    TimeUs T(float x) const;
    TimeUs VisibleStart() const { return scroll_us; }
    TimeUs VisibleEnd() const { return T(origin_x + width); }
    TimeUs PxToUs(float px) const;
};

// New scroll so that time `t_anchor` stays under the same x after changing pps from old_pps to new_pps:
// scroll += (t_anchor - scroll) * (1 - old_pps / new_pps).
TimeUs ZoomScroll(TimeUs scroll_us, TimeUs t_anchor, double old_pps, double new_pps);
double ClampPps(double pps);
// pps that shows [0, duration] in `width` px (with a 4 % margin); kMaxPps for an empty project.
double FitPps(TimeUs duration, float width);

struct TickUnit {
    TimeUs major_us;   // spacing of labelled ticks
    int minor_div;     // minor ticks per major (5), 0 = none (frame unit)
    bool frames;       // the unit is one canvas frame
};
// First of {1 frame, 100 ms, 500 ms, 1 s, 5 s, 10 s, 30 s, 1 min, 5 min} whose spacing is >= min_px at pps
// (5 min when none is). Minor ticks at 1/5 of the major unit.
TickUnit PickTickUnit(double pps, int fps_num, int fps_den, float min_px = 80);
// Ruler label for tick time t: "1:05" (>= 1 s units), "1:05.50" (sub-second), "1:05:12f" (frames).
const char* FormatTick(char* buf, unsigned n, TimeUs t, const TickUnit& u, int fps_num, int fps_den);

} // namespace ct::ui
