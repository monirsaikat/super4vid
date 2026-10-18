#pragma once
// ct/ui/panels/status_bar.h
// Status bar (docs/05 section 4) along the bottom of the main viewport, and the shortcut cheat sheet window.
// Thread model: UI thread, inside a frame. Call DrawStatusBar before DockSpaceOverViewport so the dock space
// leaves room for it.

#include <ct/ui/editor_state.h>

namespace ct::ui {

struct StatusInfo {
    double autosave_age_s = -1;   // < 0: never autosaved
    double cache_mb = -1, cache_budget_mb = -1;   // < 0: unknown (before P06)
    int warnings = 0;             // log warnings since start (click opens the log)
    double gpu_ms = -1;           // Compositor::LastTiming (P08); < 0 unknown
    double frame_ms = 0;          // UI CPU frame time
    double export_fraction = -1;  // 0..1 while an export runs in the background (progress pill); < 0 none
};

// What the user clicked in the status bar this frame.
enum class StatusBarClick { None, Warnings, Export };

constexpr float kStatusBarHeight = 26;   // DIP

// Returns the clicked indicator: the warnings count (opens the log) or the export progress pill (restores the
// minimized export progress window).
StatusBarClick DrawStatusBar(const EditorState& state, const StatusInfo& info);
// Cheat sheet listing the shortcut table grouped by section. `open` toggles with ? / F1.
void DrawShortcutSheet(bool* open);

} // namespace ct::ui
