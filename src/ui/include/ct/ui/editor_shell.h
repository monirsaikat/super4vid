#pragma once
// ct/ui/editor_shell.h
// The editor window contents (P09 9.1 shell): menu bar, docking layout (Media 260 | Preview | Properties 320 over
// Timeline 300 at 1600x900, docs/05 section 4), status bar, toasts, shortcut dispatch, cheat sheet, stats overlay.
// The preview area is drawn by a callback (the compositor output in the second half of P09); without one it shows
// a styled placeholder of the canvas. Used by `Camtesia.exe --editor-demo`; the P09 EditorWindow wraps it with
// file dialogs, TimelinePlayer, Compositor and autosave.
// Thread model: UI thread. Lifetime: create after ImGui::CreateContext, destroy before DestroyContext.

#include <ct/ui/editor_state.h>
#include <ct/ui/panels/media_panel.h>
#include <ct/ui/panels/properties_panel.h>
#include <ct/ui/panels/status_bar.h>
#include <ct/ui/panels/timeline_widget.h>
#include <ct/ui/shortcuts.h>
#include <ct/ui/widgets.h>

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>

namespace ct::ui {

class EditorShell {
public:
    EditorShell();
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    // Theme::Apply + Fonts::Load + docking flags on the current context (call once, before the first frame).
    static void SetupImGui();

    bool LoadProject(const char* path_utf8, std::string* error);
    // --editor-demo (after LoadProject): synthetic waveforms so audio lanes show peaks without P06, a toast for
    // Import, the first screen clip selected.
    void EnableDemo();

    void Frame();   // builds the whole UI for one frame (between NewFrame and Render)
    bool WantsContinuousRedraw() const;

    EditorState& State() { return m_state; }
    TimelineWidget& Timeline() { return m_timeline; }
    PropertiesPanel& Properties() { return m_props; }
    MediaPanel& Media() { return m_media; }
    ToastStack& Toasts() { return m_toasts; }
    void SetMediaProvider(TimelineMediaProvider* p);
    void SetHostWindow(void* hwnd) { m_hwnd = hwnd; }   // window title "<name>* - Camtesia"

    // Second-half hooks.
    std::function<void(ImVec2 min, ImVec2 max)> draw_preview;   // draws the canvas image into [min, max]
    std::function<bool(Action)> on_action;                       // file/export/recorder actions; true = handled
    StatusInfo status;                                           // filled by the owner each frame

private:
    void DispatchShortcuts();
    void Run(Action a);
    void DrawMenuBar();
    void BuildLayout(unsigned int dockspace_id);
    void DrawPreview();
    void DrawPreviewPlaceholder(ImVec2 min, ImVec2 max);
    void DrawTransport();
    void DrawStats();
    void UpdateTitle();

    EditorState m_state;
    TimelineWidget m_timeline;
    PropertiesPanel m_props;
    MediaPanel m_media;
    ToastStack m_toasts;
    std::unique_ptr<TimelineMediaProvider> m_demo_media;
    bool m_layout_built = false;
    bool m_reset_layout = false;
    bool m_cheat_sheet = false;
    bool m_stats = false;
    bool m_show_panels[4] = {true, true, true, true};
    int m_fit_mode = 0;   // 0 fit, 1 50 %, 2 100 %, 3 200 %
    bool m_safe_areas = false;
    void* m_hwnd = nullptr;
    std::string m_title;
    double m_last_frame_s = 0;
    double m_frame_ms = 0;
};

// Saves the window's client area as PNG (PrintWindow(PW_RENDERFULLCONTENT), BitBlt fallback) through
// platform::ImageIo. COM must be initialized. Returns false on failure.
bool SaveWindowScreenshot(void* hwnd, const char* path_utf8);

} // namespace ct::ui
