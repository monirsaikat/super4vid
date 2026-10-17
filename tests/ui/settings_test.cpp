// P11 11.5/11.6 tests: settings save debounce, hotkey chord capture/format, conflict detection (duplicates, probe,
// a real RegisterHotKey conflict), error message formatting, the error banner and the settings window in a headless
// ImGui frame, first-run detection with a temporary settings directory.
#include "headless.h"

#include <ct/core/config.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/first_run.h>
#include <ct/ui/settings_window.h>
#include <ct/ui/strings.h>
#include <ct/ui/widgets.h>

#include <windows.h>

#include <cstdio>
#include <string>

using namespace ct;
using namespace ct::ui;
using ct::ui::testing::Headless;

namespace {

// A fresh directory under %TEMP% removed at scope exit.
struct TempDir {
    std::string path;
    TempDir() {
        path = fs::TempPath("");
        fs::CreateDirectories(path);
    }
    ~TempDir() { fs::DeleteTree(path); }
    std::string File(const char* name) const { return PathJoin(path, name); }
};

bool WaitFor(const std::function<bool()>& cond, int timeout_ms = 3000) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (cond()) return true;
        Sleep(10);
    }
    return cond();
}

} // namespace

// --- Debounce ---------------------------------------------------------------------------------------------------------

CT_TEST(settings_debouncer_coalesces_and_rearms) {
    Debouncer d(500);
    CT_CHECK(!d.Pending());
    CT_CHECK_EQ(d.RemainingMs(0), -1);
    d.Touch(1000);
    CT_CHECK(d.Pending());
    CT_CHECK(!d.Due(1499));
    CT_CHECK_EQ(d.RemainingMs(1200), 300);
    d.Touch(1300);   // a second change inside the window pushes the deadline out
    CT_CHECK(!d.Due(1500));
    CT_CHECK(!d.Due(1799));
    CT_CHECK(d.Due(1800));
    CT_CHECK_EQ(d.RemainingMs(1900), 0);
    d.Clear();
    CT_CHECK(!d.Due(5000));
}

CT_TEST(settings_saver_writes_once_after_quiet_period) {
    TempDir dir;
    const std::string path = dir.File("settings.json");
    {
        SettingsSaver saver(path, 100);
        Settings s;
        for (int q = 10; q <= 50; q += 10) {   // five changes in quick succession -> one write
            s.recording.quality = q;
            saver.Schedule(s);
            Sleep(5);
        }
        CT_CHECK(saver.Pending());
        CT_CHECK(WaitFor([&] { return saver.SaveCount() >= 1; }));
        Sleep(250);
        CT_CHECK_EQ(saver.SaveCount(), 1u);
        CT_CHECK(!saver.Pending());
        CT_CHECK(saver.LastError().empty());
        Settings loaded;
        CT_REQUIRE(loaded.Load(path, nullptr));
        CT_CHECK_EQ(loaded.recording.quality, 50);

        // Flush writes a pending change immediately; the destructor flushes too.
        s.recording.quality = 77;
        saver.Schedule(s);
        saver.Flush();
        CT_CHECK_EQ(saver.SaveCount(), 2u);
        CT_REQUIRE(loaded.Load(path, nullptr));
        CT_CHECK_EQ(loaded.recording.quality, 77);
        s.recording.quality = 88;
        saver.Schedule(s);
    }
    Settings loaded;
    CT_REQUIRE(loaded.Load(path, nullptr));
    CT_CHECK_EQ(loaded.recording.quality, 88);
}

// --- Hotkey chords --------------------------------------------------------------------------------------------------------

CT_TEST(hotkey_chord_capture_and_format) {
    Hotkey hk;
    CT_CHECK(CaptureChord(0, MOD_CONTROL, &hk) == ChordResult::Pending);
    CT_CHECK(CaptureChord(VK_F9, 0, &hk) == ChordResult::Captured);
    CT_CHECK(hk == (Hotkey{0, VK_F9}));
    CT_CHECK_EQ(HotkeyLabel(hk), std::string("F9"));
    CT_CHECK(CaptureChord('A', 0, &hk) == ChordResult::NeedsModifier);
    CT_CHECK(CaptureChord('A', MOD_SHIFT, &hk) == ChordResult::NeedsModifier);
    CT_CHECK(CaptureChord(VK_LEFT, 0, &hk) == ChordResult::NeedsModifier);
    CT_CHECK(CaptureChord('A', MOD_SHIFT | MOD_CONTROL, &hk) == ChordResult::Captured);
    CT_CHECK_EQ(HotkeyLabel(hk), std::string("Ctrl+Shift+A"));
    CT_CHECK(CaptureChord(VK_F9, MOD_SHIFT | MOD_ALT | MOD_CONTROL | MOD_WIN, &hk) == ChordResult::Captured);
    CT_CHECK_EQ(HotkeyLabel(hk), std::string("Ctrl+Alt+Shift+Win+F9"));
    Hotkey parsed;
    CT_REQUIRE(ParseHotkey(HotkeyLabel(hk), parsed));   // the label round-trips through the settings file format
    CT_CHECK(parsed == hk);
    CT_CHECK(CaptureChord(VK_ESCAPE, 0, &hk) == ChordResult::Cancelled);
    CT_CHECK(CaptureChord(VK_BACK, 0, &hk) == ChordResult::Cleared);
    CT_CHECK(CaptureChord(VK_DELETE, 0, &hk) == ChordResult::Cleared);
    CT_CHECK(CaptureChord(VK_ESCAPE, MOD_CONTROL, &hk) == ChordResult::Captured);
    CT_CHECK(CaptureChord(VK_PAUSE, 0, &hk) == ChordResult::Captured);
    CT_CHECK_EQ(HotkeyLabel(Hotkey{}), std::string(str::kSetHkNone));

    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_A), static_cast<unsigned>('A'));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_Z), static_cast<unsigned>('Z'));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_7), static_cast<unsigned>('7'));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_F1), static_cast<unsigned>(VK_F1));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_F24), static_cast<unsigned>(VK_F24));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_Keypad3), static_cast<unsigned>(VK_NUMPAD3));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_PageDown), static_cast<unsigned>(VK_NEXT));
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_LeftCtrl), 0u);
    CT_CHECK_EQ(VkFromImGuiKey(ImGuiKey_Semicolon), 0u);
}

CT_TEST(hotkey_conflict_detection) {
    Settings::Hotkeys h;   // F9 / F10 / F8 / Ctrl+Shift+F9
    int probes = 0;
    HotkeyProbe ok_probe = [&](Hotkey, std::string*) { ++probes; return true; };
    HotkeyProbe busy_probe = [&](Hotkey hk, std::string* e) {
        ++probes;
        *e = HotkeyToString(hk) + " is already used by another app";
        return false;
    };
    std::string err;
    // Duplicate of another action: refused before any probe.
    CT_CHECK(!CheckHotkey(h, HotkeyAction::Toggle, Hotkey{0, VK_F10}, ok_probe, &err));
    CT_CHECK(err.find("F10") != std::string::npos);
    CT_CHECK(err.find(str::kSetHkPause) != std::string::npos);
    CT_CHECK_EQ(probes, 0);
    // Unchanged: no probe (our own registration would fail it).
    CT_CHECK(CheckHotkey(h, HotkeyAction::Toggle, Hotkey{0, VK_F9}, busy_probe, &err));
    CT_CHECK_EQ(probes, 0);
    // Unset is always fine.
    CT_CHECK(CheckHotkey(h, HotkeyAction::Marker, Hotkey{}, busy_probe, &err));
    // New chord: the probe decides.
    CT_CHECK(CheckHotkey(h, HotkeyAction::Toggle, Hotkey{MOD_CONTROL, VK_F11}, ok_probe, &err));
    CT_CHECK_EQ(probes, 1);
    err.clear();
    CT_CHECK(!CheckHotkey(h, HotkeyAction::Toggle, Hotkey{MOD_CONTROL, VK_F11}, busy_probe, &err));
    CT_CHECK_EQ(err, std::string("Ctrl+F11 is already used by another app"));
}

CT_TEST(hotkey_system_probe_detects_real_registration) {
    // Hold a chord the way another app would (thread-bound RegisterHotKey), then probe it.
    const Hotkey hk{MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23};
    if (!RegisterHotKey(nullptr, 0x7001, hk.modifiers | MOD_NOREPEAT, hk.vk)) CT_SKIP("Ctrl+Alt+Shift+F23 is taken on this machine");
    std::string err;
    CT_CHECK(!SystemHotkeyProbe(hk, &err));
    CT_CHECK(err.find("Ctrl+Alt+Shift+F23") != std::string::npos);
    UnregisterHotKey(nullptr, 0x7001);
    err.clear();
    CT_CHECK(SystemHotkeyProbe(hk, &err));
    // The probe does not keep the chord: registering it again succeeds.
    CT_CHECK(RegisterHotKey(nullptr, 0x7002, hk.modifiers, hk.vk) != FALSE);
    UnregisterHotKey(nullptr, 0x7002);
}

CT_TEST(settings_window_assign_hotkey_applies_and_notifies) {
    TempDir dir;
    const std::string path = dir.File("settings.json");
    int changes = 0;
    Settings last;
    {
        SettingsWindow w(path);
        w.on_changed = [&](const Settings& s) {
            ++changes;
            last = s;
        };
        bool busy = false;
        w.probe = [&](Hotkey hk, std::string* e) {
            if (busy) *e = HotkeyToString(hk) + " is already used by another app";
            return !busy;
        };
        std::string err;
        CT_CHECK(w.AssignHotkeyForTest(HotkeyAction::Marker, Hotkey{MOD_CONTROL, 'M'}, &err));
        CT_CHECK_EQ(changes, 1);
        CT_CHECK(last.hotkeys.marker == (Hotkey{MOD_CONTROL, 'M'}));
        CT_CHECK(!w.AssignHotkeyForTest(HotkeyAction::Pause, Hotkey{MOD_CONTROL, 'M'}, &err));   // duplicate
        busy = true;
        CT_CHECK(!w.AssignHotkeyForTest(HotkeyAction::Pause, Hotkey{MOD_CONTROL, 'P'}, &err));   // another app
        CT_CHECK_EQ(changes, 1);
        CT_CHECK(w.Current().hotkeys.pause == (Hotkey{0, VK_F10}));
        w.ApplyForTest([](Settings& s) { s.editor.snap = false; });
        CT_CHECK_EQ(changes, 2);
        CT_CHECK(!last.editor.snap);
    }   // destructor flushes the debounced save
    Settings loaded;
    CT_REQUIRE(loaded.Load(path, nullptr));
    CT_CHECK(loaded.hotkeys.marker == (Hotkey{MOD_CONTROL, 'M'}));
    CT_CHECK(!loaded.editor.snap);
}

// --- Error messages ----------------------------------------------------------------------------------------------------

CT_TEST(error_message_formatting) {
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DiskFull, "D:\\Videos\\Camtesia\\a.ctrec"),
                std::string("Not enough disk space on D:. Recording stopped and saved."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DiskFull, "e:"), std::string("Not enough disk space on E:. Recording stopped and saved."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DiskFull, ""),
                std::string("Not enough disk space on the output drive. Recording stopped and saved."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::MediaMissing, "C:\\clips\\intro 100%.mp4"),
                std::string("Media file not found: C:\\clips\\intro 100%.mp4. Relink it from the Media panel."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::NoEncoder), std::string(str::kErrNoEncoder));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::MicBusy, "ignored"), std::string(str::kErrMicBusy));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DeviceLost), std::string(str::kErrDeviceLost));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::AccessLost), std::string(str::kErrAccessLost));

    CT_CHECK_EQ(DriveOf("\\\\server\\share\\rec\\x.mp4"), std::string("\\\\server\\share"));
    CT_CHECK_EQ(DriveOf("\\\\?\\D:\\very\\long"), std::string("D:"));
    CT_CHECK_EQ(DriveOf("\\\\?\\UNC\\nas\\videos\\a"), std::string("\\\\nas\\videos"));
    CT_CHECK_EQ(DriveOf("relative\\path"), std::string());
    CT_CHECK_EQ(FillPlaceholder("a %s b", "100%"), std::string("a 100% b"));

    const std::string d = FormatErrorDetails(static_cast<long>(0x887A0005), "C:\\logs", "frame 1234");
    CT_CHECK(d.find("Error code: 0x887A0005") != std::string::npos);
    CT_CHECK(d.find("DXGI_ERROR_DEVICE_REMOVED") != std::string::npos);
    CT_CHECK(d.find("Log folder: C:\\logs") != std::string::npos);
    CT_CHECK(d.find("frame 1234") != std::string::npos);
    CT_CHECK(FormatErrorDetails(0, "C:\\logs", "").find("Error code") == std::string::npos);
}

CT_TEST(error_banner_draws_and_dismisses) {
    Headless h(800, 300);
    ErrorBanner b;
    b.SetLogDir("C:\\logs");
    CT_CHECK(!b.Visible());
    h.Frame([&] { CT_CHECK(!b.Draw("##err")); });
    b.Show(ErrorKind::DiskFull, "D:\\x.mp4", static_cast<long>(0x80070070));   // ERROR_DISK_FULL
    CT_CHECK(b.Visible());
    CT_CHECK(b.DetailsText().find("0x80070070") != std::string::npos);
    float height = 0;
    h.Frame([&] {
        const float y0 = ImGui::GetCursorScreenPos().y;
        CT_CHECK(b.Draw("##err"));
        height = ImGui::GetCursorScreenPos().y - y0;
    });
    CT_CHECK(height > 20.0f);
    CT_CHECK(!h.IdConflict());
    b.Clear();
    CT_CHECK(!b.Visible());
    CT_CHECK(b.Message().empty());
}

// --- Settings window smoke test ---------------------------------------------------------------------------------------

CT_TEST(settings_window_draws_every_section) {
    TempDir dir;
    Headless h(900, 640);
    SettingsWindow w(dir.File("settings.json"));
    w.SetFillViewport(true);
    w.Open(Settings::Defaults());
    for (int i = 0; i < kSettingsSectionCount; ++i) {
        w.SetSection(static_cast<SettingsSection>(i));
        for (int f = 0; f < 3; ++f) h.Frame([&] { w.Draw(); }, false);
        CT_CHECK(w.IsOpen());
        CT_CHECK(!h.IdConflict());
    }
    w.Close();
    CT_CHECK(!w.IsOpen());
    CT_CHECK(!w.WantsRedraw());
}

CT_TEST(settings_window_modal_opens_and_closes) {
    TempDir dir;
    Headless h(1280, 800);
    SettingsWindow w(dir.File("settings.json"));
    w.Open(Settings::Defaults(), SettingsSection::Hotkeys);
    for (int f = 0; f < 3; ++f) h.Frame([&] { w.Draw(); }, false);
    bool popup_open = false;
    h.Frame([&] { popup_open = ImGui::IsPopupOpen("##ct_settings"); w.Draw(); }, false);
    CT_CHECK(popup_open);
    w.Close();   // from outside, between frames
    for (int f = 0; f < 2; ++f) h.Frame([&] { w.Draw(); }, false);
    h.Frame([&] { popup_open = ImGui::IsPopupOpen("##ct_settings"); }, false);
    CT_CHECK(!popup_open);
    CT_CHECK(!h.IdConflict());
}

CT_TEST(settings_icons_present_in_icon_font) {
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    int checked = 0;
    for (const char* file : {"SegoeIcons.ttf", "segmdl2.ttf"}) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, file);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        Headless h;
        ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16);
        CT_REQUIRE(f != nullptr);
        h.Frame([] {});
        for (const char* icon : kSettingsExtraIcons) {
            unsigned int cp = 0;
            ImTextCharFromUtf8(&cp, icon, nullptr);
            if (!f->IsGlyphInFont(static_cast<ImWchar>(cp))) {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "U+%04X missing in %s", cp, file);
                CT_FAIL(msg);
            }
        }
        ++checked;
    }
    if (!checked) CT_SKIP("no Segoe icon font installed");
}

// --- First run ------------------------------------------------------------------------------------------------------------

CT_TEST(first_run_detection_with_temp_settings_dir) {
    TempDir dir;
    const std::string settings_dir = dir.File("Camtesia");
    const std::string path = PathJoin(settings_dir, "settings.json");
    CT_CHECK(FirstRun::IsFirstRunAt(path));

    Settings s;
    s.recording.output_dir = dir.File("Videos\\Camtesia");
    Headless h;
    ToastStack toasts;
    CT_CHECK(FirstRun::Run(&toasts, s, path));
    CT_CHECK_EQ(toasts.Count(), static_cast<size_t>(1));
    // Folders and the settings file are written on the IO pool.
    CT_CHECK(WaitFor([&] { return fs::Exists(path); }));
    CT_CHECK(fs::IsDirectory(s.recording.output_dir));
    CT_CHECK(!FirstRun::IsFirstRunAt(path));
    Settings loaded;
    CT_REQUIRE(loaded.Load(path, nullptr));
    CT_CHECK_EQ(loaded.recording.output_dir, s.recording.output_dir);

    // Second start: no toast.
    CT_CHECK(!FirstRun::Run(&toasts, s, path));
    CT_CHECK_EQ(toasts.Count(), static_cast<size_t>(1));
    CT_CHECK(!std::string(str::kFirstRunToast).empty());
}
