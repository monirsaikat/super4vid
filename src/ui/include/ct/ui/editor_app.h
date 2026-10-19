#pragma once
// ct/ui/editor_app.h
// The editor mode of super4vid.exe: `super4vid.exe --editor [file.ctproj | folder.ctrec | media...]` and a bare file
// argument (the .ctproj association passes `--editor "%1"`). Owns the main window (class "super4vidMain", so a second
// instance forwards its command line here with WM_COPYDATA), the D3D11 device + flip swapchain, Dear ImGui with the
// layout ini in %APPDATA%\super4vid\editor_layout.ini, the event loop (renders only after input, posted results or
// while EditorWindow wants continuous redraws: 0 redraws when idle), WM_DROPFILES and the unsaved-changes close path.
// Measurement modes for P09 9.7: --stats (redraws/s each second), --bench-drag, --bench-scrub, --bench-play.
// Thread model: the main (UI) thread, COM STA initialized by the caller.

#include <string>
#include <vector>

namespace ct::ui {

struct EditorLaunch {
    std::vector<std::string> paths;   // opened in order once the window exists
    std::string screenshot;           // --screenshot out.png: save the window after --frames N frames and exit
    int frames = 5;
    bool stats = false;               // --stats: print redraws/s once per second
    double seek_s = -1;               // --seek S: playhead (debug / screenshots)
    std::string select_clip;          // --select-clip ID (debug / screenshots)
    bool show_stats_overlay = false;  // --overlay: F11 stats overlay on
    int fit_mode = -1;                // --fit 0..3 (preview zoom)
    bool open_export = false;         // --open-export [preset-id]: File > Export once the files opened (screenshots)
    std::string export_preset;
    std::string bench;                // --bench-drag | --bench-scrub | --bench-play
    int bench_count = 0;              // frames / seeks / seconds
    int width_dip = 1600, height_dip = 900;
};

// True when the command line asks for the editor: `--editor` (optionally followed by paths) or any bare (non-option)
// argument. Fills `out` with the paths and editor options. `--editor-demo` is not the editor mode (main handles it).
bool ParseEditorLaunch(const wchar_t* command_line, EditorLaunch* out);

// Runs the editor until its window closes; returns the process exit code. `instance` is the HINSTANCE (icon).
int RunEditorApp(void* instance, int show_cmd, const EditorLaunch& launch);

} // namespace ct::ui
