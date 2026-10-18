// P11 11.6 keyboard-only pass over the editor: with keyboard navigation on (EditorShell::SetupImGui), Tab moves the
// focus through the controls of the Preview panel (transport) and of the Timeline panel (toolbar), Ctrl+Tab reaches
// every panel, and no Tab press lands on an ID conflict. (The recorder bar's order is checked in the real app with
// `Camtesia.exe --bar-keys`, see PROGRESS 11.6.)
#include "headless.h"

#include <ct/test/ct_test.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/strings.h>

#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace ct;
using namespace ct::ui;
using ct::ui::testing::Headless;

namespace {

void Press(Headless& h, EditorShell& shell, ImGuiKey key, bool ctrl = false) {
    if (ctrl) h.Key(ImGuiMod_Ctrl, true);
    h.Key(key, true);
    h.Frame([&] { shell.Frame(); }, false);
    h.Key(key, false);
    if (ctrl) h.Key(ImGuiMod_Ctrl, false);
    h.Frame([&] { shell.Frame(); }, false);
}

// The root (dock) window that owns the keyboard focus, "" when none.
std::string FocusRoot() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.NavWindow) return {};
    std::string name = g.NavWindow->RootWindow->Name;   // a docked panel is its own root (the dock host is not)
    const size_t hash = name.find("##");
    return hash == std::string::npos ? name : name.substr(0, hash);
}

} // namespace

CT_TEST(editor_tab_reaches_transport_and_timeline_controls) {
    Headless h(1600, 900);
    EditorShell::SetupImGui();
    CT_REQUIRE(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard);
    EditorShell shell;
    for (int i = 0; i < 3; ++i) h.Frame([&] { shell.Frame(); }, false);

    for (const char* panel : {str::kPanelPreview, str::kPanelTimeline}) {
        ImGui::SetWindowFocus(panel);
        h.Frame([&] { shell.Frame(); }, false);
        std::set<ImGuiID> seen;
        for (int i = 0; i < 24; ++i) {
            Press(h, shell, ImGuiKey_Tab);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.NavId && FocusRoot() == panel) seen.insert(g.NavId);
            CT_CHECK(!h.IdConflict());
        }
        std::printf("    %s: Tab visits %zu controls\n", panel, seen.size());
        CT_CHECK(seen.size() >= 3);   // transport buttons / timeline toolbar
    }

    // Ctrl held + Tab (ImGui's window switcher) walks every panel; the highlighted target is recorded per press.
    std::set<std::string> panels;
    h.Key(ImGuiMod_Ctrl, true);
    h.Frame([&] { shell.Frame(); }, false);
    for (int i = 0; i < 10; ++i) {
        h.Key(ImGuiKey_Tab, true);
        h.Frame([&] { shell.Frame(); }, false);
        h.Key(ImGuiKey_Tab, false);
        h.Frame([&] { shell.Frame(); }, false);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.NavWindowingTarget) {
            std::string name = g.NavWindowingTarget->RootWindow->Name;
            const size_t hash = name.find("##");
            panels.insert(hash == std::string::npos ? name : name.substr(0, hash));
        }
    }
    h.Key(ImGuiMod_Ctrl, false);
    h.Frame([&] { shell.Frame(); }, false);
    for (const char* p : {str::kPanelMedia, str::kPanelPreview, str::kPanelProperties, str::kPanelTimeline}) {
        if (!panels.count(p)) std::printf("    Ctrl+Tab did not reach %s\n", p);
        CT_CHECK(panels.count(p) == 1);
    }
}
