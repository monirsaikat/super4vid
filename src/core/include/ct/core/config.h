#pragma once
// ct/core/config.h
// Typed application settings mirroring %APPDATA%\Camtesia\settings.json (docs/04 section 4), with defaults, validation,
// versioned migration, and preservation of unknown keys (written back untouched, so a newer build's settings survive
// a round trip through an older one). Also hotkey strings ("Ctrl+Shift+F9") <-> RegisterHotKey values.
// Thread model: a Settings object is a plain value; Load/Save do blocking file IO (call them off the UI thread).

#include <ct/core/error.h>
#include <ct/core/json.h>

#include <string>
#include <string_view>

typedef unsigned int UINT;   // same definition as <windef.h>; keeps <windows.h> out of this header

namespace ct {

// modifiers: RegisterHotKey's MOD_ALT (0x1) | MOD_CONTROL (0x2) | MOD_SHIFT (0x4) | MOD_WIN (0x8). vk: a VK_* code.
// {0, 0} means "no hotkey" ("None").
struct Hotkey {
    UINT modifiers = 0;
    UINT vk = 0;
    bool IsSet() const { return vk != 0; }
    bool operator==(const Hotkey&) const = default;
};

// Case-insensitive, '+'-separated, spaces ignored: "Ctrl+Shift+F9", "alt + a", "Win+3", "F10", "None".
// Modifiers: Ctrl/Control, Shift, Alt, Win. Keys: A-Z, 0-9, F1-F24, Space, Enter, Tab, Esc, Backspace, Delete, Insert,
// Home, End, PageUp, PageDown, Left, Right, Up, Down, PrintScreen, Pause, Num0-Num9. Exactly one key.
bool ParseHotkey(std::string_view text, Hotkey& out);
std::string HotkeyToString(Hotkey hk);   // canonical order Ctrl+Alt+Shift+Win+Key; {0,0} -> "None"

struct Settings {
    static constexpr int kVersion = 1;

    // Optional strings ("mic_device": null) are empty when null.
    struct Recording {
        int fps = 60;                      // 1..240
        std::string codec = "h264";        // "h264" | "hevc"
        int quality = 70;                  // 1..100
        std::string mic_device;            // endpoint id; empty = default device
        bool system_audio = true;
        bool mic = true;
        std::string webcam_device;         // empty = none
        std::string webcam_resolution = "1280x720";
        int countdown_s = 3;               // 0..10
        std::string keystrokes = "shortcuts";
        int cursor_hz = 120;               // 30..1000
        std::string output_dir;            // empty = %USERPROFILE%\Videos\Camtesia
        bool hide_bar_while_recording = true;
    } recording;
    struct Hotkeys {
        Hotkey toggle{0, 0x78};            // F9
        Hotkey pause{0, 0x79};             // F10
        Hotkey marker{0, 0x77};            // F8
        Hotkey cancel{0x2 | 0x4, 0x78};    // Ctrl+Shift+F9
    } hotkeys;
    struct Editor {
        std::string theme = "dark";        // "dark" | "light"
        int autosave_s = 60;               // 0 = off, ..3600
        int frame_cache_mb = 256;          // 32..16384
        bool snap = true;
        bool ripple = false;
        std::string preview_quality = "full";   // "full" | "half"
    } editor;
    struct Export {                         // JSON key "export" ("export" is a C++ keyword)
        std::string last_preset = "mp4-1080p";
        std::string last_dir;
    } export_;
    struct Privacy {
        bool crash_dumps = true;
    } privacy;
    struct Window {
        struct Editor { int x = 100, y = 100, w = 1600, h = 900; bool maximized = false; } editor;
        struct Bar { int x = 0, y = 0; } bar;
    } window;

    static Settings Defaults();
    // Missing file = defaults (returns true). Unreadable/corrupt file: defaults, returns false with *err set.
    bool Load(std::string_view path, Err* err);
    bool Save(std::string_view path, Err* err) const;   // atomic write; creates the parent directory
    Json ToJson() const;                                // known fields in docs/04 order, then the preserved unknown keys
    static Settings FromJson(const Json& j);            // wrong types / out-of-range values fall back to defaults
    int Version() const { return m_version; }           // version found in the file (0 if absent), kVersion after Migrate
    // Upgrades older files step by step to kVersion. Newer files keep their version and unknown keys.
    void Migrate();
    const Json& Extra() const { return m_extra; }       // unknown keys, nested like the file

private:
    void Sanitize();

    int m_version = kVersion;
    Json m_extra;
};

} // namespace ct
