#pragma once
// ct/ui/recorder_app.h
// The default mode of Camtesia.exe (docs/03 section 2): the recorder bar in its own frameless topmost window with a
// D3D11 flip swapchain and Dear ImGui (multi-viewport, so popovers and toasts get their own capture-excluded windows),
// an event-driven loop (0 redraws when idle), the tray icon and the hotkeys. --tray starts with the bar hidden.
// Diagnostics (all optional): --stats (redraws/s), --time-to-window, --screenshot out.png [--frames N]
// [--bar-open monitor|fps|mic|system|camera] [--bar-record] [--bar-toast], --synthetic-camera, --countdown S,
// --bench-hotkey N (N x [Ctrl+Alt+Shift+F22 -> first frame, stop]).
// Thread model: runs on the calling (main, STA) thread until the user quits. Lifetime: owns everything it creates.

#include <ct/ui/recorder_bar.h>

#include <ct/platform/win.h>

#include <string>

namespace ct::ui {

struct RecorderAppOptions {
    bool tray = false;
    bool stats = false;
    bool time_to_window = false;
    std::string screenshot;   // PNG of the bar (and its open popovers) after `frames` frames, then exit
    int frames = 5;
    HICON icon = nullptr;     // app icon (window + tray)
    RecorderBar::Hooks hooks; // open_editor / open_settings; empty open_editor launches "Camtesia.exe --editor <path>"
};

// Returns the process exit code.
int RunRecorderApp(const RecorderAppOptions& options);

// Launches that must not forward to a running instance: diagnostics (--screenshot, --time-to-window, --bench-hotkey)
// and an editor started from the bar (environment CAMTESIA_SEPARATE_INSTANCE=1).
bool RunsAsSeparateInstance(const wchar_t* cmdline);

} // namespace ct::ui
