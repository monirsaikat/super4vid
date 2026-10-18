#pragma once
// ct/ui/editor_window.h
// The editor document window (docs/phases/P09 "EditorWindow"): EditorShell + the project player (TimelinePlayer),
// the compositor-rendered preview, MediaCache-backed thumbnails/peaks, file operations (New / Open / Open recent /
// Save / Save As / Import media / Import recording / Close, drag-and-drop), media probing on the IO pool, relink /
// reveal / remove, Auto-Zoom regeneration, autosave + "Recover unsaved project?" prompt, the unsaved-changes prompt,
// error banners, and the status bar numbers.
// Thread model: UI thread (COM STA) only; IO-pool jobs post their results with ct::Async::PostToUi.
// Lifetime: Create() after the ImGui context and its DX11/Win32 backends exist; Destroy() before they go.

#include <ct/core/types.h>
#include <ct/ui/editor_shell.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ct::platform {
class D3DDevice;
}
namespace ct::audio {
class AudioDevices;
}

namespace ct::ui {

class EditorWindow {
public:
    EditorWindow();
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // `devices` may be null (no audio output). Starts the recovery scan and loads the recent-files list.
    bool Create(void* hwnd, platform::D3DDevice& dev, audio::AudioDevices* devices);
    void Destroy();

    // .ctproj -> open (prompts when dirty); .ctrec folder (or its meta.json) -> new project from the recording when
    // the current one is empty, else import into it; media files -> import into the media bin.
    void OpenPath(std::string_view path);
    void OpenPaths(const std::vector<std::string>& paths);   // drag and drop (WM_DROPFILES)
    // File > Export (Ctrl+E): the export dialog on a snapshot of the project, `preset_id` preselected (empty = last
    // used). One dialog per window; while an export runs this brings its progress window back.
    void OpenExportDialog(const std::string& preset_id = {});

    // Outside NewFrame/Render: runs deferred shell dialogs (they must not open inside an ImGui frame), releases last
    // frame's pinned thumbnails.
    void BeforeFrame();
    void Frame();                       // between NewFrame and Render
    bool WantsContinuousRedraw() const;
    void OnTimer();                     // ~1 Hz: autosave tick (no redraw)

    // WM_CLOSE: true = close now; false = the unsaved-changes prompt is shown (the window stays).
    bool CloseRequested();
    std::function<void()> on_close_confirmed;   // the prompt's Save / Don't save path (the app destroys the window)

    EditorShell& Shell() { return *m_shell; }
    EditorState& State() { return m_shell->State(); }

    // Measurements (F11 overlay, --bench modes, tests).
    struct Stats {
        double last_scrub_ms = -1;          // playhead change -> exact frame rendered (paused)
        double last_render_cpu_ms = 0;      // Compositor CPU time of the last preview frame
        uint64_t preview_renders = 0;
        uint64_t frames_inexact_playing = 0;   // preview frames rendered while playing without every exact frame
        uint64_t playing_frames = 0;
        int open_caches = 0, pending_opens = 0;
        double cache_mb = 0;
    };
    const Stats& GetStats() const { return m_stats; }
    void SetRedrawsPerSecond(uint32_t r) { m_redraws_per_s = r; }
    std::function<void(double ms)> on_scrub_latency;   // bench hook: every completed scrub measurement

private:
    struct Impl;
    std::unique_ptr<EditorShell> m_shell;
    std::unique_ptr<Impl> m_impl;
    Stats m_stats;
    uint32_t m_redraws_per_s = 0;
};

} // namespace ct::ui
