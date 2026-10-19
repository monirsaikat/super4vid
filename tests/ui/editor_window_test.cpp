// ct_ui tests for the editor window pieces of P09 9.1/9.2/9.6/9.7: the docs/05 section 5 shortcut list against the
// shortcut table and the shell's dispatcher, shortcuts held back while a modal is open, the "<name>* - super4vid"
// title, the status-bar warning count (Log::WarningCount), banner de-duplication, and finding/showing the recorder
// bar from the editor (Ctrl+R).
#define CT_LOG_CATEGORY "test"
#include "headless.h"

#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/single_instance.h>
#include <ct/project/edit_ops.h>
#include <ct/test/ct_test.h>
#include <ct/ui/editor_banners.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/editor_window.h>
#include <ct/ui/recorder_app.h>
#include <ct/ui/shortcuts.h>
#include <ct/ui/strings.h>

#include <windows.h>

#include <set>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::ui;
using ct::platform::SingleInstance;
using ct::ui::testing::Headless;

namespace {

class EnvGuard {
public:
    explicit EnvGuard(const wchar_t* name) : m_name(name) {
        wchar_t buf[512];
        const DWORD n = GetEnvironmentVariableW(name, buf, 512);
        m_had = n > 0 && n < 512;
        if (m_had) m_value.assign(buf, n);
    }
    ~EnvGuard() { SetEnvironmentVariableW(m_name, m_had ? m_value.c_str() : nullptr); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const wchar_t* m_name;
    bool m_had = false;
    std::wstring m_value;
};

std::wstring g_copydata;
int g_copydata_count = 0;

LRESULT CALLBACK FakeBarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == SingleInstance::kCopyDataCommandLine && cds->lpData) {
            g_copydata.assign(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            while (!g_copydata.empty() && g_copydata.back() == L'\0') g_copydata.pop_back();
            ++g_copydata_count;
            return TRUE;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::wstring WindowText(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 256);
    return buf;
}

} // namespace

// Every row of docs/05 section 5 (keys as written there) resolves to the action it describes, and every action the
// table can produce is handled by the shell or by the editor window's on_action (no "not available" toast).
CT_TEST(docs05_shortcuts_map_to_their_actions) {
    struct Row {
        const char* keys;
        Action action;
    };
    const Row rows[] = {
        {"Space", Action::TogglePlay},       {"J", Action::Reverse},           {"K", Action::Stop},
        {"L", Action::Forward},              {"Shift+J", Action::ReverseSlow}, {"Shift+L", Action::ForwardSlow},
        {",", Action::NudgeLeft},            {".", Action::NudgeRight},        {"Shift+,", Action::NudgeLeft10},
        {"Shift+.", Action::NudgeRight10},   {"Left", Action::PrevFrame},      {"Right", Action::NextFrame},
        {"Shift+Left", Action::Back1s},      {"Shift+Right", Action::Forward1s}, {"Home", Action::GoStart},
        {"End", Action::GoEnd},              {"I", Action::LoopIn},            {"O", Action::LoopOut},
        {"Alt+I", Action::ClearLoopIn},      {"Alt+O", Action::ClearLoopOut},  {"S", Action::Split},
        {"Delete", Action::Delete},          {"Backspace", Action::Delete},    {"Shift+Delete", Action::RippleDelete},
        {"Ctrl+Z", Action::Undo},            {"Ctrl+Shift+Z", Action::Redo},   {"Ctrl+Y", Action::Redo},
        {"Ctrl+X", Action::Cut},             {"Ctrl+C", Action::Copy},         {"Ctrl+V", Action::Paste},
        {"Ctrl+D", Action::Duplicate},       {"Ctrl+A", Action::SelectAll},    {"M", Action::AddMarker},
        {"Z", Action::AddCameraKey},         {"Shift+Z", Action::RemoveCameraKey}, {"+", Action::ZoomIn},
        {"-", Action::ZoomOut},              {"F", Action::FitTimeline},       {"Ctrl+S", Action::Save},
        {"Ctrl+Shift+S", Action::SaveAs},    {"Ctrl+O", Action::Open},         {"Ctrl+N", Action::New},
        {"Ctrl+I", Action::Import},          {"Ctrl+E", Action::Export},       {"Ctrl+R", Action::OpenRecorder},
        {"Ctrl+Shift+R", Action::ToggleRipple}, {"N", Action::ToggleSnap},     {"Ctrl+1", Action::FocusMedia},
        {"Ctrl+2", Action::FocusPreview},    {"Ctrl+3", Action::FocusProperties}, {"Ctrl+4", Action::FocusTimeline},
        {"F11", Action::ToggleStats},        {"?", Action::CheatSheet},        {"F1", Action::CheatSheet},
    };
    size_t n = 0;
    const ShortcutDef* table = Shortcuts::Table(&n);
    std::set<Action> in_table;
    for (size_t i = 0; i < n; ++i) in_table.insert(table[i].action);
    for (const Row& r : rows) {
        ImGuiKeyChord want = 0;
        CT_REQUIRE(Shortcuts::Parse(r.keys, &want));
        bool found = false;
        for (size_t i = 0; i < n && !found; ++i) found = Shortcuts::Chord(i) == want && table[i].action == r.action;
        if (!found) std::printf("  docs/05 shortcut %s has no table entry for its action\n", r.keys);
        CT_CHECK(found);
    }
    // Ctrl+S / Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y work while typing (P09 9.1).
    for (size_t i = 0; i < n; ++i) {
        const Action a = table[i].action;
        if (a == Action::Save || a == Action::Undo || a == Action::Redo) CT_CHECK(table[i].always);
    }

    // Dispatch: the editor window handles the file/recorder actions; everything else is the shell's own.
    Headless h(1600, 900);
    EditorShell shell;
    const std::set<Action> window_actions = {Action::New, Action::Open, Action::Import, Action::Save, Action::SaveAs, Action::Export, Action::OpenRecorder};
    std::vector<Action> forwarded;
    shell.on_action = [&](Action a) {
        if (!window_actions.count(a)) return false;
        forwarded.push_back(a);
        return true;
    };
    for (int i = 0; i < 2; ++i) h.Frame([&] { shell.Frame(); }, false);
    for (Action a : in_table) {
        const size_t toasts = shell.Toasts().Count();
        h.Frame([&] {
            shell.Run(a);
            shell.Frame();
        }, false);
        if (shell.Toasts().Count() != toasts) std::printf("  action %d pushed a toast\n", static_cast<int>(a));
        CT_CHECK_EQ(shell.Toasts().Count(), toasts);
        shell.State().playback->Pause();
    }
    CT_CHECK_EQ(forwarded.size(), window_actions.size());
}

CT_TEST(shortcuts_are_held_back_while_a_modal_is_open) {
    Headless h(1600, 900);
    EditorShell shell;
    EditorState& s = shell.State();
    const bool snap0 = s.snap;
    bool modal = true;
    auto frame = [&] {
        h.Frame([&] {
            shell.Frame();
            if (modal && !ImGui::IsPopupOpen("Test modal")) ImGui::OpenPopup("Test modal");
            if (ImGui::BeginPopupModal("Test modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("modal");
                if (!modal) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }, false);
    };
    frame();
    frame();
    // N (toggle snapping) while the modal is up: ignored.
    h.Key(ImGuiKey_N, true);
    frame();
    h.Key(ImGuiKey_N, false);
    frame();
    CT_CHECK_EQ(s.snap, snap0);
    // Modal closed: N works again.
    modal = false;
    frame();
    frame();
    h.Key(ImGuiKey_N, true);
    frame();
    h.Key(ImGuiKey_N, false);
    frame();
    CT_CHECK_EQ(s.snap, !snap0);
}

CT_TEST(window_title_shows_the_dirty_marker) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CT_REQUIRE(hwnd != nullptr);
    {
        Headless h(1600, 900);
        EditorShell shell;
        shell.SetHostWindow(hwnd);
        EditorState& s = shell.State();
        h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(WindowText(hwnd) == L"Untitled - super4vid");
        project::Marker mk;
        mk.t_us = 1000000;
        mk.label = "m";
        CT_CHECK(s.Edit("Add marker", [&](project::Project& p) { return project::ops::AddMarker(p, mk); }));
        h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(WindowText(hwnd) == L"Untitled* - super4vid");
        CT_CHECK(s.Undo());
        h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(WindowText(hwnd) == L"Untitled - super4vid");
        CT_CHECK(s.Redo());   // restores the snapshot (named "Untitled"), so rename afterwards
        s.project.name = "Demo take";
        h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(WindowText(hwnd) == L"Demo take* - super4vid");
        s.undo.MarkSaved();
        h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(WindowText(hwnd) == L"Demo take - super4vid");
    }
    DestroyWindow(hwnd);
}

CT_TEST(warning_count_counts_warnings_and_errors) {
    const LogLevel level = Log::Level();
    Log::SetLevel(LogLevel::Info);
    const uint64_t before = Log::WarningCount();
    CT_LOG_INFO("warning_count test: info line");
    CT_CHECK_EQ(Log::WarningCount(), before);
    CT_LOG_WARN("warning_count test: warning line");
    CT_LOG_ERROR("warning_count test: error line");
    CT_CHECK_EQ(Log::WarningCount(), before + 2);
    Log::SetLevel(level);
}

CT_TEST(banner_stack_reports_the_visible_message) {
    Headless h;
    BannerStack b;
    CT_CHECK(b.Message(BannerPanel::Media) == nullptr);
    const int id = b.Show(BannerPanel::Media, "Media file not found: D:\\x.mp4. Relink it from the Media panel.", "Not found: D:\\x.mp4");
    CT_REQUIRE(b.Message(BannerPanel::Media) != nullptr);
    CT_CHECK(*b.Message(BannerPanel::Media) == "Media file not found: D:\\x.mp4. Relink it from the Media panel.");
    CT_CHECK(b.Message(BannerPanel::Preview) == nullptr);
    h.Frame([&] { b.Draw(BannerPanel::Media); });
    CT_CHECK(!h.IdConflict());
    b.Dismiss(id);
    CT_CHECK(b.Message(BannerPanel::Media) == nullptr);
    CT_CHECK(!std::string(str::kErrMediaUnreadable).empty() && !std::string(str::kErrNoAudioOutput).empty());
}

CT_TEST(recorder_bar_is_found_and_shown_from_the_editor) {
    EnvGuard guard(L"CT_INSTANCE_SUFFIX");
    SetEnvironmentVariableW(L"CT_INSTANCE_SUFFIX", L"ct-ui-tests-bar");
    const std::wstring cls = SingleInstance::MainWindowClass();
    CT_CHECK(cls == L"super4vidMain.ct-ui-tests-bar");
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = &FakeBarProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls.c_str();
    CT_REQUIRE(RegisterClassExW(&wc) != 0);
    // An editor window (no WS_EX_TOOLWINDOW) only: no bar.
    HWND editor = CreateWindowExW(0, cls.c_str(), L"Untitled - super4vid", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    CT_REQUIRE(editor != nullptr);
    CT_CHECK(FindRecorderBarWindow(editor) == nullptr);
    // The bar: a hidden tool window of the same class.
    HWND bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.c_str(), L"super4vid", WS_POPUP, 0, 0, 100, 40, nullptr, nullptr, wc.hInstance, nullptr);
    CT_REQUIRE(bar != nullptr);
    CT_CHECK(FindRecorderBarWindow(editor) == bar);
    CT_CHECK(FindRecorderBarWindow(bar) == nullptr);   // the caller's own window is skipped
    g_copydata.clear();
    g_copydata_count = 0;
    CT_CHECK(ShowRecorderBar(editor));
    CT_CHECK_EQ(g_copydata_count, 1);
    // A plain command line (just the exe): the bar shows itself instead of starting an editor.
    CT_CHECK(!g_copydata.empty());
    CT_CHECK(IsRecorderBarCommandLine(g_copydata.c_str()));
    CT_CHECK(ForwardedEditorArguments(g_copydata.c_str()).empty());
    DestroyWindow(bar);
    DestroyWindow(editor);
    UnregisterClassW(cls.c_str(), wc.hInstance);
}

// v0.1.0 bug: the "Export" menu and the title-bar "Export" button had the same ID, so hovering either one popped
// ImGui's "2 visible items with conflicting ID" error. Sweep the title bar with the menus closed, then open the File
// menu and walk across every header (each menu opens in turn) and down through its items.
CT_TEST(title_bar_and_open_menus_have_no_id_conflicts) {
    Headless h(1600, 900);
    EditorShell shell;
    std::vector<Action> forwarded;
    shell.on_action = [&](Action a) {   // nothing reaches the real window (no recorder launch, no dialogs)
        forwarded.push_back(a);
        return true;
    };
    for (int i = 0; i < 3; ++i) h.Frame([&] { shell.Frame(); }, false);
    const float bar_y = ImGui::GetFrameHeight() * 0.5f;
    bool conflict = false;
    float conflict_x = -1, conflict_y = -1;
    auto at = [&](float x, float y) {
        h.Mouse(x, y);
        h.Frame([&] { shell.Frame(); }, false);
        if (!conflict && h.IdConflict()) {
            conflict = true;
            conflict_x = x;
            conflict_y = y;
        }
    };
    for (float x = 2; x < 1600; x += 4) at(x, bar_y);   // menus closed: headers, title, Record, Export, chevron
    // Open File (the first header) by clicking it.
    at(20, bar_y);
    h.Button(0, true);
    h.Frame([&] { shell.Frame(); }, false);
    h.Button(0, false);
    h.Frame([&] { shell.Frame(); }, false);
    for (float x = 8; x < 420; x += 12) {   // the menu headers sit at the left of the bar
        at(x, bar_y);
        for (float y = bar_y + 14; y < 420; y += 9) at(x + 20, y);
        at(x, bar_y);
    }
    if (conflict) std::printf("  ID conflict while hovering (%.0f, %.0f)\n", conflict_x, conflict_y);
    CT_CHECK(!conflict);
}

// Whole-window sweep with a full project open: every hovered item in every panel has a unique ID (ImGui reports a
// conflict only for a hovered item, so the mouse visits a grid over the window), first with nothing selected, then
// with the first clip selected (the properties panel shows the clip sections).
CT_TEST(editor_window_grid_sweep_has_no_id_conflicts) {
    Headless h(1600, 900);
    EditorShell shell;
    shell.on_action = [](Action) { return true; };
    std::string err;
    CT_REQUIRE(shell.LoadProject("data/projects/full.ctproj", &err));
    for (int i = 0; i < 3; ++i) h.Frame([&] { shell.Frame(); }, false);
    bool conflict = false;
    float cx = -1, cy = -1;
    for (int pass = 0; pass < 2 && !conflict; ++pass) {
        if (pass == 1) {
            EditorState& s = shell.State();
            CT_REQUIRE(!s.project.clips.empty());
            s.selection.Clear();
            s.selection.clips = {s.project.clips.front().id};
        }
        for (float y = 4; y < 900 && !conflict; y += 14)
            for (float x = 4; x < 1600 && !conflict; x += 14) {
                h.Mouse(x, y);
                h.Frame([&] { shell.Frame(); }, false);
                if (h.IdConflict()) {
                    conflict = true;
                    cx = x;
                    cy = y;
                }
            }
    }
    if (conflict) std::printf("  ID conflict while hovering (%.0f, %.0f)\n", cx, cy);
    CT_CHECK(!conflict);
    CT_CHECK_EQ(shell.State().undo.UndoEntries(), size_t(0));   // hovering never edits
}
