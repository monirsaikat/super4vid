// ct_ui tests for the editor integration pieces: Export wiring in the shell (menu/button hooks, the status-bar export
// pill), forwarding a double-clicked project from the running recorder bar to a new editor process (argument
// re-quoting), and the CT_INSTANCE_SUFFIX single-instance override.
#include "headless.h"

#include <ct/export/preset.h>
#include <ct/platform/single_instance.h>
#include <ct/test/ct_test.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/panels/status_bar.h>
#include <ct/ui/recorder_app.h>

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::ui;
using ct::platform::SingleInstance;
using ct::ui::testing::Headless;

namespace {

std::vector<std::wstring> Argv(const std::wstring& cmdline) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    std::vector<std::wstring> out;
    for (int i = 0; argv && i < argc; ++i) out.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    return out;
}

// Restores (or clears) an environment variable at scope exit.
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

} // namespace

CT_TEST(forwarded_editor_arguments_round_trip) {
    // A double-clicked project / recording (the .ctproj association passes --editor "%1").
    const std::wstring proj = L"\"C:\\Program Files\\Camtesia\\Camtesia.exe\" --editor \"D:\\My Videos\\demo take.ctproj\"";
    const std::wstring args = ForwardedEditorArguments(proj.c_str());
    CT_CHECK(!args.empty());
    std::vector<std::wstring> v = Argv(L"Camtesia.exe " + args);
    CT_REQUIRE(v.size() == 3);
    CT_CHECK(v[1] == L"--editor");
    CT_CHECK(v[2] == L"D:\\My Videos\\demo take.ctproj");

    // A bare .ctrec folder with a trailing backslash: the closing quote must survive re-quoting.
    const std::wstring rec = L"Camtesia.exe \"D:\\rec\\2026-10-17 12-03-44.ctrec\\\\\"";
    CT_CHECK_EQ(Argv(rec).size(), size_t(2));
    CT_CHECK(!IsRecorderBarCommandLine(rec.c_str()));
    v = Argv(L"Camtesia.exe " + ForwardedEditorArguments(rec.c_str()));
    CT_REQUIRE(v.size() == 2);
    CT_CHECK(v[1] == L"D:\\rec\\2026-10-17 12-03-44.ctrec\\");

    // Recorder-bar command lines (plain second launch, --tray, bar diagnostics) are not editor launches.
    CT_CHECK(ForwardedEditorArguments(L"Camtesia.exe").empty());
    CT_CHECK(ForwardedEditorArguments(L"Camtesia.exe --tray").empty());
    CT_CHECK(ForwardedEditorArguments(L"Camtesia.exe --countdown 1").empty());
    CT_CHECK(ForwardedEditorArguments(nullptr).empty());
    // `--editor` alone opens an empty editor.
    CT_CHECK(ForwardedEditorArguments(L"Camtesia.exe --editor") == L"--editor");

    // Quoting rules.
    CT_CHECK(QuoteCommandLineArg(L"plain") == L"plain");
    CT_CHECK(QuoteCommandLineArg(L"") == L"\"\"");
    for (const std::wstring s : {L"a b", L"a\"b", L"a\\\\\"b c", L"c:\\x y\\", L"tab\there"}) {
        v = Argv(L"x.exe " + QuoteCommandLineArg(s));
        CT_REQUIRE(v.size() == 2);
        CT_CHECK(v[1] == s);
    }
}

CT_TEST(bench_flags_skip_the_single_instance_check) {
    CT_CHECK(RunsAsSeparateInstance(L"Camtesia.exe --editor p.ctproj --bench-drag 600"));
    CT_CHECK(RunsAsSeparateInstance(L"Camtesia.exe --bench-hotkey 5"));
    EnvGuard g(L"CAMTESIA_SEPARATE_INSTANCE");
    SetEnvironmentVariableW(L"CAMTESIA_SEPARATE_INSTANCE", nullptr);
    CT_CHECK(!RunsAsSeparateInstance(L"Camtesia.exe --editor p.ctproj"));
}

CT_TEST(instance_suffix_override) {
    EnvGuard g(L"CT_INSTANCE_SUFFIX");
    SetEnvironmentVariableW(L"CT_INSTANCE_SUFFIX", nullptr);
    CT_CHECK(SingleInstance::Suffix().empty());
    CT_CHECK(SingleInstance::MutexName() == L"Local\\Camtesia.Instance");
    CT_CHECK(SingleInstance::MainWindowClass() == L"CamtesiaMain");

    SetEnvironmentVariableW(L"CT_INSTANCE_SUFFIX", L"wt-a256");
    CT_CHECK(SingleInstance::MutexName() == L"Local\\Camtesia.Instance.wt-a256");
    CT_CHECK(SingleInstance::MainWindowClass() == L"CamtesiaMain.wt-a256");

    // Only [A-Za-z0-9._-], at most 32 characters; nothing valid left = no suffix.
    CT_CHECK(SingleInstance::SanitizeSuffix(L"a\\b/c d:e") == L"abcde");
    CT_CHECK(SingleInstance::SanitizeSuffix(std::wstring(40, L'x')).size() == size_t(32));
    SetEnvironmentVariableW(L"CT_INSTANCE_SUFFIX", L"\\/ :");
    CT_CHECK(SingleInstance::MutexName() == L"Local\\Camtesia.Instance");

    // Two instances with different suffixes both acquire; the same suffix forwards (no window: Acquire gives up).
    SetEnvironmentVariableW(L"CT_INSTANCE_SUFFIX", L"ct-ui-tests");
    const std::wstring name = SingleInstance::MutexName();
    HANDLE other = CreateMutexW(nullptr, FALSE, (std::wstring(SingleInstance::kBaseMutexName) + L".ct-ui-tests-other").c_str());
    CT_REQUIRE(other != nullptr);
    CT_CHECK(SingleInstance::Acquire(name.c_str(), L""));
    SingleInstance::Release();
    CloseHandle(other);
}

CT_TEST(shell_export_hooks_and_status_pill) {
    Headless h(1600, 900);
    EditorShell shell;
    std::vector<Action> actions;
    shell.on_action = [&](Action a) {
        actions.push_back(a);
        return true;
    };
    std::string preset;
    shell.on_export_preset = [&](const std::string& id) { preset = id; };
    int restores = 0;
    shell.on_export_restore = [&] { ++restores; };
    shell.Run(Action::Export);
    CT_REQUIRE(actions.size() == 1);
    CT_CHECK(actions[0] == Action::Export);

    // Menus and the preset popup draw without ID conflicts, with and without a running export.
    for (int i = 0; i < 3; ++i) h.Frame([&] { shell.Frame(); }, false);
    CT_CHECK(!h.IdConflict());
    shell.status.export_fraction = 0.42;
    for (int i = 0; i < 3; ++i) h.Frame([&] { shell.Frame(); }, false);
    CT_CHECK(!h.IdConflict());
    CT_CHECK(!exportng::Preset::Builtin().empty());

    // Click along the status bar from the right: one spot is the export pill (restores the progress window).
    EditorState state;
    StatusInfo info;
    info.export_fraction = 0.5;
    const float y = 900 - kStatusBarHeight * 0.5f;
    bool hit = false;
    for (float x = 1600 - 8; x > 1000 && !hit; x -= 6) {
        StatusBarClick c = StatusBarClick::None;
        h.Mouse(x, y);
        h.Frame([&] { (void)DrawStatusBar(state, info); }, false);
        h.Button(0, true);
        h.Frame([&] { (void)DrawStatusBar(state, info); }, false);
        h.Button(0, false);
        h.Frame([&] { c = DrawStatusBar(state, info); }, false);
        hit = c == StatusBarClick::Export;
    }
    CT_CHECK(hit);
    // Without an export there is no pill to click.
    info.export_fraction = -1;
    bool any_export = false;
    for (float x = 1600 - 8; x > 1000; x -= 12) {
        StatusBarClick c = StatusBarClick::None;
        h.Mouse(x, y);
        h.Frame([&] { (void)DrawStatusBar(state, info); }, false);
        h.Button(0, true);
        h.Frame([&] { (void)DrawStatusBar(state, info); }, false);
        h.Button(0, false);
        h.Frame([&] { c = DrawStatusBar(state, info); }, false);
        any_export = any_export || c == StatusBarClick::Export;
    }
    CT_CHECK(!any_export);
    CT_CHECK_EQ(restores, 0);   // DrawStatusBar alone never calls the shell hook
}
