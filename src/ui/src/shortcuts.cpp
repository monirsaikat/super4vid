// Keyboard shortcut table (docs/05 section 5) and dispatch.
#include <ct/ui/shortcuts.h>

#include <imgui_internal.h>

#include <cstring>

namespace ct::ui {
namespace {

using G = ShortcutGroup;

const ShortcutDef kTable[] = {
    {"Space", Action::TogglePlay, "Play / pause", G::Playback},
    {"J", Action::Reverse, "Reverse (repeat: 2x, 4x)", G::Playback},
    {"K", Action::Stop, "Stop", G::Playback},
    {"L", Action::Forward, "Forward (repeat: 2x, 4x)", G::Playback},
    {"Shift+J", Action::ReverseSlow, "Reverse at 0.5x", G::Playback},
    {"Shift+L", Action::ForwardSlow, "Forward at 0.5x", G::Playback},
    {"Left", Action::PrevFrame, "Previous frame", G::Playback, false, true},
    {"Right", Action::NextFrame, "Next frame", G::Playback, false, true},
    {"Shift+Left", Action::Back1s, "Back 1 second", G::Playback, false, true},
    {"Shift+Right", Action::Forward1s, "Forward 1 second", G::Playback, false, true},
    {"Home", Action::GoStart, "Go to start", G::Playback},
    {"End", Action::GoEnd, "Go to end", G::Playback},
    {"I", Action::LoopIn, "Set loop in", G::Playback},
    {"O", Action::LoopOut, "Set loop out", G::Playback},
    {"Alt+I", Action::ClearLoopIn, "Clear loop in", G::Playback},
    {"Alt+O", Action::ClearLoopOut, "Clear loop out", G::Playback},

    {"S", Action::Split, "Split at playhead", G::Editing},
    {"Delete", Action::Delete, "Delete selection (ripple if enabled)", G::Editing},
    {"Backspace", Action::Delete, "Delete selection (ripple if enabled)", G::Editing},
    {"Shift+Delete", Action::RippleDelete, "Ripple delete", G::Editing},
    {"Ctrl+Z", Action::Undo, "Undo", G::Editing, true},
    {"Ctrl+Shift+Z", Action::Redo, "Redo", G::Editing, true},
    {"Ctrl+Y", Action::Redo, "Redo", G::Editing, true},
    {"Ctrl+X", Action::Cut, "Cut", G::Editing},
    {"Ctrl+C", Action::Copy, "Copy", G::Editing},
    {"Ctrl+V", Action::Paste, "Paste at playhead", G::Editing},
    {"Ctrl+D", Action::Duplicate, "Duplicate", G::Editing},
    {"Ctrl+A", Action::SelectAll, "Select all on the timeline", G::Editing},
    {",", Action::NudgeLeft, "Nudge selection 1 frame left", G::Editing, false, true},
    {".", Action::NudgeRight, "Nudge selection 1 frame right", G::Editing, false, true},
    {"Shift+,", Action::NudgeLeft10, "Nudge selection 10 frames left", G::Editing, false, true},
    {"Shift+.", Action::NudgeRight10, "Nudge selection 10 frames right", G::Editing, false, true},

    {"M", Action::AddMarker, "Add marker", G::Timeline},
    {"Z", Action::AddCameraKey, "Add camera keyframe at playhead", G::Timeline},
    {"Shift+Z", Action::RemoveCameraKey, "Remove nearest camera keyframe", G::Timeline},
    {"+", Action::ZoomIn, "Zoom in", G::Timeline, false, true},
    {"Keypad+", Action::ZoomIn, "Zoom in", G::Timeline, false, true},
    {"-", Action::ZoomOut, "Zoom out", G::Timeline, false, true},
    {"Keypad-", Action::ZoomOut, "Zoom out", G::Timeline, false, true},
    {"F", Action::FitTimeline, "Fit timeline", G::Timeline},
    {"Ctrl+Shift+R", Action::ToggleRipple, "Toggle ripple mode", G::Timeline},
    {"N", Action::ToggleSnap, "Toggle snapping", G::Timeline},

    {"Ctrl+S", Action::Save, "Save", G::File, true},
    {"Ctrl+Shift+S", Action::SaveAs, "Save as", G::File},
    {"Ctrl+O", Action::Open, "Open", G::File},
    {"Ctrl+N", Action::New, "New project", G::File},
    {"Ctrl+I", Action::Import, "Import media", G::File},
    {"Ctrl+E", Action::Export, "Export", G::File},
    {"Ctrl+R", Action::OpenRecorder, "Open recorder bar", G::File},

    {"Ctrl+1", Action::FocusMedia, "Focus Media", G::View},
    {"Ctrl+2", Action::FocusPreview, "Focus Preview", G::View},
    {"Ctrl+3", Action::FocusProperties, "Focus Properties", G::View},
    {"Ctrl+4", Action::FocusTimeline, "Focus Timeline", G::View},
    {"F11", Action::ToggleStats, "Toggle stats overlay", G::View},
    {"?", Action::CheatSheet, "Shortcut cheat sheet", G::View},
    {"F1", Action::CheatSheet, "Shortcut cheat sheet", G::View},
};
constexpr size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

ImGuiKeyChord g_chords[kCount];
bool g_parsed = false;

void EnsureParsed() {
    if (g_parsed) return;
    for (size_t i = 0; i < kCount; ++i)
        if (!Shortcuts::Parse(kTable[i].keys, &g_chords[i])) g_chords[i] = 0;
    g_parsed = true;
}

bool Eq(const char* a, size_t n, const char* b) { return std::strlen(b) == n && _strnicmp(a, b, n) == 0; }

bool ParseKey(const char* s, size_t n, ImGuiKeyChord* key) {
    if (n == 1) {
        const char c = s[0];
        if (c >= 'A' && c <= 'Z') return *key = ImGuiKey_A + (c - 'A'), true;
        if (c >= 'a' && c <= 'z') return *key = ImGuiKey_A + (c - 'a'), true;
        if (c >= '0' && c <= '9') return *key = ImGuiKey_0 + (c - '0'), true;
        switch (c) {
        case '+': return *key = ImGuiKey_Equal, true;   // the unshifted "=/+" key; numpad has Keypad+
        case '=': return *key = ImGuiKey_Equal, true;
        case '-': return *key = ImGuiKey_Minus, true;
        case ',': return *key = ImGuiKey_Comma, true;
        case '.': return *key = ImGuiKey_Period, true;
        case '/': return *key = ImGuiKey_Slash, true;
        case '?': return *key = ImGuiMod_Shift | ImGuiKey_Slash, true;
        default: return false;
        }
    }
    if ((s[0] == 'F' || s[0] == 'f') && n <= 3) {
        int v = 0;
        for (size_t i = 1; i < n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        if (v >= 1 && v <= 24) return *key = ImGuiKey_F1 + (v - 1), true;
        return false;
    }
    struct Named { const char* name; ImGuiKey key; };
    static const Named kNamed[] = {
        {"Space", ImGuiKey_Space}, {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow},
        {"Up", ImGuiKey_UpArrow}, {"Down", ImGuiKey_DownArrow}, {"Home", ImGuiKey_Home}, {"End", ImGuiKey_End},
        {"Delete", ImGuiKey_Delete}, {"Backspace", ImGuiKey_Backspace}, {"Enter", ImGuiKey_Enter},
        {"Escape", ImGuiKey_Escape}, {"Esc", ImGuiKey_Escape}, {"Tab", ImGuiKey_Tab},
        {"PageUp", ImGuiKey_PageUp}, {"PageDown", ImGuiKey_PageDown},
        {"Keypad+", ImGuiKey_KeypadAdd}, {"Keypad-", ImGuiKey_KeypadSubtract},
    };
    for (const Named& k : kNamed)
        if (Eq(s, n, k.name)) return *key = k.key, true;
    return false;
}

} // namespace

const ShortcutDef* Shortcuts::Table(size_t* count) {
    if (count) *count = kCount;
    return kTable;
}

bool Shortcuts::Parse(const char* keys, ImGuiKeyChord* out) {
    if (!keys || !*keys) return false;
    ImGuiKeyChord mods = 0;
    const char* p = keys;
    for (;;) {
        // A token ends at '+' unless the token is the single character "+" (or "Keypad+").
        const char* end = p;
        if (*end == '+') ++end;
        while (*end && *end != '+') ++end;
        if (*end == '+' && end - p == 6 && _strnicmp(p, "Keypad", 6) == 0) ++end;   // "Keypad+"
        const size_t n = static_cast<size_t>(end - p);
        if (*end == '\0') {
            ImGuiKeyChord key = 0;
            if (!ParseKey(p, n, &key)) return false;
            *out = mods | key;
            return true;
        }
        if (Eq(p, n, "Ctrl")) mods |= ImGuiMod_Ctrl;
        else if (Eq(p, n, "Shift")) mods |= ImGuiMod_Shift;
        else if (Eq(p, n, "Alt")) mods |= ImGuiMod_Alt;
        else return false;
        p = end + 1;
        if (!*p) return false;
    }
}

ImGuiKeyChord Shortcuts::Chord(size_t index) {
    EnsureParsed();
    return index < kCount ? g_chords[index] : 0;
}

int Shortcuts::Poll(Action* out, int max) {
    EnsureParsed();
    const bool text = ImGui::GetIO().WantTextInput;
    int n = 0;
    for (size_t i = 0; i < kCount && n < max; ++i) {
        if (!g_chords[i] || (text && !kTable[i].always)) continue;
        const ImGuiInputFlags flags = kTable[i].repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None;
        if (ImGui::IsKeyChordPressed(g_chords[i], flags, ImGuiKeyOwner_Any)) out[n++] = kTable[i].action;
    }
    return n;
}

const char* Shortcuts::KeysFor(Action a) {
    for (const ShortcutDef& d : kTable)
        if (d.action == a) return d.keys;
    return "";
}

const char* Shortcuts::GroupName(ShortcutGroup g) {
    switch (g) {
    case G::Playback: return "Playback";
    case G::Editing: return "Editing";
    case G::Timeline: return "Timeline";
    case G::File: return "File";
    case G::View: return "View";
    }
    return "";
}

} // namespace ct::ui
