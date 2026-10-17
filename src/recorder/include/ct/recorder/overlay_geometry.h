#pragma once
// ct/recorder/overlay_geometry.h
// Pure geometry behind the region selection overlay, countdown and recording frame (docs/05 section 3): handle
// hit-testing, drag/resize/nudge with even sizes, window-snap selection over a z-ordered window list, the final
// region/monitor choice, size-label placement and the frame edges. No Windows calls, so everything is unit-tested.
// All rects are virtual-screen physical pixels unless a name says otherwise.

#include <ct/core/types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ct::recorder::overlay {

constexpr int32_t kMinRegion = 64;      // smallest region the capture accepts (ClampRegionToDisplay)
constexpr int32_t kHandlePx = 10;       // resize handle squares (scaled by DPI by the caller)
constexpr int32_t kFrameThickness = 2;  // recording frame

enum class Handle : uint8_t { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Inside };

// Rounds width and height down to even numbers (never below 0).
RectI MakeEven(RectI r);
// Normalized rect spanned by two drag points (both inclusive pixels: a drag from (0,0) to (9,9) is 10x10), with even
// size. A zero-length drag gives a 0x0 rect.
RectI RectFromDrag(PointI a, PointI b);
// Center of each handle square for a selection; Handle::Inside/None return the selection center.
PointI HandleCenter(const RectI& sel, Handle h);
RectI HandleRect(const RectI& sel, Handle h, int32_t handle_px = kHandlePx);
// Which handle (or Inside / None) is at p. Handles win over Inside; the hit area is the handle square grown by
// `slop` px on every side. Corner handles win over edge handles.
Handle HitTest(const RectI& sel, PointI p, int32_t handle_px = kHandlePx, int32_t slop = 3);

// Resizes `start` by dragging handle h by `delta`. The opposite edge stays fixed; the dragged edge cannot cross it
// closer than min_size, and stays inside bounds. Inside moves the whole rect (clamped into bounds). Result is even
// (the dragged edges absorb the rounding).
RectI ApplyDrag(const RectI& start, Handle h, PointI delta, const RectI& bounds, int32_t min_size = kMinRegion);
// Arrow-key nudge: moves (or with resize = true grows/shrinks the bottom-right edge) by (dx, dy), clamped into bounds.
RectI Nudge(const RectI& sel, int32_t dx, int32_t dy, bool resize, const RectI& bounds);

// One top-level window as seen by EnumWindows (top of the z-order first).
struct SnapWindow {
    RectI rect;            // DWMWA_EXTENDED_FRAME_BOUNDS (falls back to GetWindowRect)
    bool visible = true;
    bool cloaked = false;  // DWMWA_CLOAKED (other virtual desktop, suspended UWP)
    bool tool = false;     // WS_EX_TOOLWINDOW
    bool minimized = false;
    bool own_process = false;
};
bool IsSnappable(const SnapWindow& w);
// Index of the topmost snappable window containing p, or -1.
int PickSnapWindow(std::span<const SnapWindow> zorder, PointI p);
// The snap rect for window `w` under p: its rect intersected with the display that contains p (even size). Empty
// when nothing snappable is under p.
RectI SnapRectAt(std::span<const SnapWindow> zorder, PointI p, const RectI& display_rect);

// Final region (docs/05, P05 5.5): the display with the larger overlap wins (ties: the one containing the region's
// centre, then the lower index); the region is intersected with it, sizes rounded down to even and grown to at
// least kMinRegion inside that display. Returns the display index or -1 when no display overlaps.
struct FinalRegion { RectI region; int display = -1; };
FinalRegion Finalize(const RectI& sel, std::span<const RectI> display_rects);

// "1920 × 1080" (UTF-8 multiplication sign).
std::string SizeLabel(int32_t w, int32_t h);
// Where the size pill goes: below the selection's bottom-right corner (right-aligned), else above its top edge,
// else inside the bottom-right corner; always inside bounds. Before a selection exists use a 0x0 sel at the cursor.
RectI LabelRect(const RectI& sel, SizeI label, const RectI& bounds, int32_t margin = 8);

// Monitor presets for the M key: the next display index after `current` (-1 starts at 0).
int NextMonitor(int current, int count);

// The 4 frame edges (top, bottom, left, right) of a `thickness` px frame just outside the region. An edge that would
// lie outside every display is moved inside the region instead (it is excluded from capture anyway), so the frame
// is visible for full-monitor regions too.
struct FrameEdges { RectI edge[4]; };
FrameEdges FrameEdgesFor(const RectI& region, std::span<const RectI> display_rects, int32_t thickness = kFrameThickness);

// Countdown bubble: a square of `diameter` centred in the region (clamped into it when the region is smaller).
RectI CenteredSquare(const RectI& region, int32_t diameter);

// Union of two rects (an empty rect is ignored).
RectI Union(const RectI& a, const RectI& b);
RectI Inflate(const RectI& r, int32_t by);

} // namespace ct::recorder::overlay
