#pragma once
// ct/recorder/region_overlay.h
// Region selection overlay (docs/05 section 3): one topmost layered window over the whole virtual screen, drawn
// with Direct2D through platform::LayeredWindow and redrawn only in dirty rectangles. Dim everywhere except the
// selection, crosshair and size label before dragging, hover highlight of the window under the cursor (click
// selects it), 8 resize handles after the first drag, move by dragging inside, arrow-key nudge (Shift 10 px,
// Ctrl resizes), Enter/Space confirm, Esc cancel, R restart, M cycles full-monitor presets. Sizes stay even.
// Output: a region (even, >= 64x64) on one display (the one with the larger overlap; v1 records one monitor).
// Thread model: UI thread only; Open() returns immediately and the done callback runs on the UI thread from the
// window procedure. RunModal() pumps messages itself (tools, tests).
// Lifetime: Close() or the destructor destroys the window (calling done with confirmed = false if still open).

#include <ct/capture/display.h>
#include <ct/core/types.h>
#include <ct/recorder/overlay_geometry.h>

#include <memory>
#include <vector>

namespace ct::recorder {

class RegionOverlay {
public:
    enum class Mode { Region, Window };   // Window: click picks the window under the cursor and confirms at once
    struct Result {
        bool confirmed = false;
        RectI region;            // virtual-screen px
        int display_index = -1;  // DisplayInfo::index of the display the region belongs to
    };
    using DoneFn = void (*)(void* user, const Result& result);

    RegionOverlay();
    ~RegionOverlay();
    RegionOverlay(const RegionOverlay&) = delete;
    RegionOverlay& operator=(const RegionOverlay&) = delete;

    // displays: capture::EnumerateDisplays. initial (optional): start in the adjust state with this selection.
    bool Open(const std::vector<capture::DisplayInfo>& displays, Mode mode, DoneFn done, void* user,
              const RectI* initial = nullptr);
    void Close();   // cancel
    bool IsOpen() const;
    double LastDrawMs() const;   // slowest redraw pass of the last update (diagnostics)
    double MaxUpdateMs() const;  // slowest incremental update since Open (all dirty passes of one input event)
    // Writes what the overlay currently shows to a PNG (the window is excluded from screenshots, so this is how
    // its rendering is checked).
    bool SaveSnapshot(const char* png_path_utf8) const;

    // Opens, pumps messages until the user confirms or cancels, returns the result.
    static Result RunModal(const std::vector<capture::DisplayInfo>& displays, Mode mode, const RectI* initial = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::recorder
