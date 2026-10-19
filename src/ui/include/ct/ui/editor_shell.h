#pragma once
// ct/ui/editor_shell.h
// The editor window contents (P09 9.1 shell): menu bar, docking layout (Media 260 | Preview | Properties 320 over
// Timeline 300 at 1600x900, docs/05 section 4), status bar, toasts, shortcut dispatch, cheat sheet, stats overlay.
// The preview area is drawn by a callback (the compositor output in the second half of P09); without one it shows
// a styled placeholder of the canvas. Used by `super4vid.exe --editor-demo`; the P09 EditorWindow wraps it with
// file dialogs, TimelinePlayer, Compositor and autosave.
// Thread model: UI thread. Lifetime: create after ImGui::CreateContext, destroy before DestroyContext.

#include <ct/ui/editor_banners.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/panels/media_panel.h>
#include <ct/ui/panels/preview_panel.h>
#include <ct/ui/panels/properties_panel.h>
#include <ct/ui/panels/status_bar.h>
#include <ct/ui/panels/timeline_widget.h>
#include <ct/ui/shortcuts.h>
#include <ct/ui/widgets.h>

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    PreviewPanel& Preview() { return m_preview; }
    ToastStack& Toasts() { return m_toasts; }
    BannerStack& Banners() { return m_banners; }
    void SetMediaProvider(TimelineMediaProvider* p);
    void SetHostWindow(void* hwnd) { m_hwnd = hwnd; }   // window title "<name>* - super4vid"
    void Run(Action a);                                  // shortcut / menu / transport dispatcher
    void ResetLayout() { m_reset_layout = true; }

    // Second-half hooks (the editor window sets them; null = the menu item is disabled).
    std::function<bool(Action)> on_action;                       // file/export/recorder actions; true = handled
    std::function<void()> on_import_recording;                   // File > Import recording... (folder picker)
    std::function<void(const std::string&)> on_open_recent;      // File > Open recent > path
    std::function<void()> on_close_project;                      // File > Close project
    std::function<void()> on_preferences;                        // Edit > Preferences (P11)
    std::function<void()> on_open_logs;                          // Help > Open logs folder / status bar warnings
    std::function<void()> on_about;                              // Help > About
    // Export menu / the Export button's chevron: open the export dialog with this built-in preset preselected.
    std::function<void(const std::string& preset_id)> on_export_preset;
    std::function<void()> on_export_restore;                     // status-bar export pill: show the progress again
    std::vector<std::string> recent_files;                       // File > Open recent, newest first
    StatusInfo status;                                           // filled by the owner each frame
    // Layout persistence: when true the docking layout comes from the ImGui ini (io.IniFilename) and is built only
    // when the ini has none (or after View > Reset layout). False (demo/tests): built on the first frame.
    bool persist_layout = false;
    // Extra per-frame statistics for the F11 overlay (owner-provided lines, e.g. GPU passes, cache, queues).
    std::function<void()> draw_stats_extra;

private:
    void DispatchShortcuts();
    void DrawMenuBar();
    void DrawExportPresetItems();
    void BuildLayout(unsigned int dockspace_id);
    void DrawStats();
    void UpdateTitle();

    EditorState m_state;
    TimelineWidget m_timeline;
    PropertiesPanel m_props;
    MediaPanel m_media;
    PreviewPanel m_preview;
    ToastStack m_toasts;
    BannerStack m_banners;
    std::unique_ptr<TimelineMediaProvider> m_demo_media;
    bool m_layout_built = false;
    bool m_reset_layout = false;
    bool m_cheat_sheet = false;
    bool m_stats = false;
    bool m_show_panels[4] = {true, true, true, true};
    void* m_hwnd = nullptr;
    std::string m_title;
    std::vector<std::pair<std::string, std::string>> m_export_presets;   // built-in (id, name), dialog order
    double m_last_frame_s = 0;
    double m_frame_ms = 0;
};

// Saves the window's client area as PNG (PrintWindow(PW_RENDERFULLCONTENT), BitBlt fallback) through
// platform::ImageIo. COM must be initialized. Returns false on failure.
bool SaveWindowScreenshot(void* hwnd, const char* path_utf8);

} // namespace ct::ui
