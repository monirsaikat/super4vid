#pragma once
// ct/ui/panels/export_dialog.h
// File > Export (docs/phases/P10 10.1): preset list (built-in + user, "Save as preset..."), settings (container, codec
// with HEVC gated on a hardware encoder, size, fps, rate control, audio bitrate, GIF options, range), destination
// through IFileSaveDialog, estimated size, then a progress modal (bar, frames, x real time, ETA, Cancel, Minimize)
// while an exportng::ExportJob runs in the background, and a completion toast with Open / Show in folder.
//
// Thread model: UI thread, inside an ImGui frame. Disk/MF work (user presets, EncoderCaps) runs on ct::Async and
// returns through Async::PostToUi; the export runs on its own thread (ExportJob). Lifetime: one per editor window;
// the destructor cancels a running export and waits for it. The Host's device/context/toasts must outlive it.

#include <ct/core/types.h>
#include <ct/export/export_job.h>
#include <ct/export/preset.h>
#include <ct/project/project.h>
#include <ct/ui/error_ux.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ct::platform { class D3DDevice; }
namespace ct::render { class RenderContext; }
namespace ct::audio { class AudioDevices; }

namespace ct::ui {

class ToastStack;

class ExportDialog {
public:
    struct Host {
        platform::D3DDevice* device = nullptr;
        render::RenderContext* render_context = nullptr;   // shared with the preview (the job locks the device per step)
        audio::AudioDevices* audio_devices = nullptr;
        ToastStack* toasts = nullptr;                      // completion / error toasts (may be null)
        void* owner_hwnd = nullptr;                        // HWND parent of the save dialog
        std::function<void()> request_redraw;              // called from the job's completion (UI thread)
    };

    ExportDialog();
    ~ExportDialog();
    ExportDialog(const ExportDialog&) = delete;
    ExportDialog& operator=(const ExportDialog&) = delete;

    // Opens the dialog for a snapshot of `project` (copied here: later edits do not reach the export). `playhead_us`
    // is the PNG frame; `preset_id` preselects a preset (empty = the last used, else the first built-in). Ignored
    // while an export is running (the progress window is brought back instead).
    void Open(const project::Project& project, TimeUs playhead_us, const Host& host, const std::string& preset_id = {});
    // Draws the dialog / progress window. Call every frame; cheap when closed.
    void Draw();

    bool IsOpen() const { return m_open; }
    bool Busy() const;                    // an export is running
    double Fraction() const;              // 0..1 while Busy (status bar indicator)
    bool WantsRedraw() const;             // Busy (progress animates) or async results pending
    void CancelExport();
    // Status-bar hook: shows the minimized progress window again.
    void Restore() { m_minimized = false; }

    // P11 11.5: a failed export reopens the dialog with a red banner (plain-language message, Details, and
    // "Choose another folder..." / "Try again"). Tests drive it through these hooks.
    const ErrorBanner& Banner() const { return m_banner; }
    void SetFaultsForTest(const exportng::ExportFaults& f) { m_faults = f; }
    void SetDestinationForTest(std::string path) { m_dest = std::move(path); }
    void StartExportForTest() { StartExport(); }
    void RunBannerActionForTest(int action) { RunBannerAction(action); }
    const std::string& Destination() const { return m_dest; }

    // Pure helpers (tests).
    static std::string DefaultFileName(const std::string& project_name, exportng::Preset::Container);
    static std::string SanitizeFileName(const std::string& name);
    static std::string MakePresetId(const std::string& name);   // "My 4K export!" -> "my-4k-export"

private:
    void DrawSettings();
    void DrawProgress();
    void StartExport();
    void OnFinished();
    void RunBannerAction(int action);
    bool BrowseDestination();
    void SelectPreset(const exportng::Preset&);
    void FixExtension();
    TimeUs RangeDuration() const;

    Host m_host;
    project::Project m_project;
    TimeUs m_playhead = 0;
    bool m_open = false;
    bool m_minimized = false;
    std::vector<exportng::Preset> m_builtin, m_user;
    int m_selected = 0;                   // index into builtin + user
    exportng::Preset m_preset;            // edited copy
    std::string m_dest;
    int m_range = 0;                      // 0 whole project, 1 loop region
    int m_size_choice = 0, m_fps_choice = 0;
    char m_save_name[64] = {};
    bool m_hevc_hw = false, m_caps_known = false;
    std::shared_ptr<int> m_alive;         // async callbacks check it (the dialog may be gone)
    std::unique_ptr<exportng::ExportJob> m_job;
    std::string m_job_path;
    std::string m_last_preset_id;
    std::string m_error;
    ErrorBanner m_banner;
    exportng::ExportFaults m_faults;
};

} // namespace ct::ui
