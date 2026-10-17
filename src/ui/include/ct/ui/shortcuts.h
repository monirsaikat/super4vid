#pragma once
// ct/ui/shortcuts.h
// The editor keyboard shortcut table (docs/05 section 5, plus the P09 9.4/9.6 additions: Shift+J/L half speed and
// ,/. nudges), parsed into ImGuiKeyChord. Dispatch once per frame with Shortcuts::Poll; the cheat sheet renders
// the same table.
// Thread model: UI thread (Poll reads the current ImGui context).

#include <imgui.h>

#include <cstddef>

namespace ct::ui {

enum class Action {
    None,
    TogglePlay, Reverse, Stop, Forward, ReverseSlow, ForwardSlow,
    PrevFrame, NextFrame, Back1s, Forward1s, GoStart, GoEnd,
    LoopIn, LoopOut, ClearLoopIn, ClearLoopOut,
    Split, Delete, RippleDelete,
    Undo, Redo,
    Cut, Copy, Paste, Duplicate, SelectAll,
    AddMarker, AddCameraKey, RemoveCameraKey,
    ZoomIn, ZoomOut, FitTimeline,
    NudgeLeft, NudgeRight, NudgeLeft10, NudgeRight10,
    Save, SaveAs, Open, New, Import, Export, OpenRecorder,
    ToggleRipple, ToggleSnap,
    FocusMedia, FocusPreview, FocusProperties, FocusTimeline,
    ToggleStats, CheatSheet,
    Count
};

enum class ShortcutGroup { Playback, Editing, Timeline, File, View };

struct ShortcutDef {
    const char* keys;          // "Ctrl+Shift+Z", "Space", "+", "?", "Keypad+"
    Action action;
    const char* description;   // shown in the cheat sheet and tooltips
    ShortcutGroup group;
    bool always = false;       // works even while a text field has focus (Ctrl+S/Z/Y)
    bool repeat = false;       // fires on key repeat (frame stepping, nudges, zoom)
};

struct Shortcuts {
    static const ShortcutDef* Table(size_t* count);
    // "Ctrl+Shift+S" -> ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S. Names: A-Z, 0-9, F1-F24, Space, Left, Right, Up,
    // Down, Home, End, Delete, Backspace, Enter, Escape, Tab, "+" (=), "-", ",", ".", "?" (Shift+/), Keypad+, Keypad-.
    // Modifiers: Ctrl, Shift, Alt. False for unknown names.
    static bool Parse(const char* keys, ImGuiKeyChord* out);
    static ImGuiKeyChord Chord(size_t index);   // parsed table entry (0 when unparsable)
    // Fills `out` with the actions whose chord was pressed this frame (exact modifier match). Entries without
    // `always` are skipped while io.WantTextInput. Returns the number written.
    static int Poll(Action* out, int max);
    // First table entry of an action (for menus/tooltips: "Ctrl+Z"), "" when none.
    static const char* KeysFor(Action a);
    static const char* GroupName(ShortcutGroup g);
};

} // namespace ct::ui
