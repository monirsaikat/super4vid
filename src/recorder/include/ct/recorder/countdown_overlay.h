#pragma once
// ct/recorder/countdown_overlay.h
// The 3-2-1 countdown (docs/05 section 2, P05 5.6): a click-through, topmost layered window centred in the capture
// region, excluded from capture: a 160 dip circle in bg2 at 85 % alpha with the number in 72 dip semibold, 1 s steps.
// It never takes focus; the recorder bar's window keeps the keyboard and calls Cancel() on Esc.
// Thread model: UI thread only; callbacks run on the UI thread (WM_TIMER of the overlay window).
// Lifetime: the destructor cancels a running countdown without calling done.

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <memory>

namespace ct::recorder {

class CountdownOverlay {
public:
    using DoneFn = void (*)(void* user, bool completed);   // completed = false after Cancel()
    using TickFn = void (*)(void* user, int seconds_left);  // after each step (seconds_left >= 1), optional

    CountdownOverlay();
    ~CountdownOverlay();
    CountdownOverlay(const CountdownOverlay&) = delete;
    CountdownOverlay& operator=(const CountdownOverlay&) = delete;

    // region: capture region (virtual px); dpi of its display (96 = 100 %). seconds <= 0 completes at once.
    bool Start(const RectI& region, UINT dpi, int seconds, DoneFn done, TickFn tick, void* user);
    void Cancel();
    bool IsRunning() const;
    int SecondsLeft() const;
    bool ExcludedFromCapture() const;

    // Tools/tests: runs a countdown with its own message loop; Esc (polled with GetAsyncKeyState, the overlay
    // has no focus) cancels. Returns true when it completed.
    static bool RunModal(const RectI& region, UINT dpi, int seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::recorder
