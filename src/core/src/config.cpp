#define CT_LOG_CATEGORY "config"
#include <ct/core/config.h>

#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace ct {

// ---- Hotkeys -------------------------------------------------------------------------------------------------

namespace {

struct KeyName {
    const char* name;
    UINT vk;
};

// First name per vk is the canonical one HotkeyToString prints.
constexpr KeyName kKeys[] = {
    {"Space", VK_SPACE},       {"Enter", VK_RETURN},      {"Return", VK_RETURN},    {"Tab", VK_TAB},
    {"Esc", VK_ESCAPE},        {"Escape", VK_ESCAPE},     {"Backspace", VK_BACK},   {"Delete", VK_DELETE},
    {"Del", VK_DELETE},        {"Insert", VK_INSERT},     {"Ins", VK_INSERT},       {"Home", VK_HOME},
    {"End", VK_END},           {"PageUp", VK_PRIOR},      {"PgUp", VK_PRIOR},       {"PageDown", VK_NEXT},
    {"PgDn", VK_NEXT},         {"Left", VK_LEFT},         {"Right", VK_RIGHT},      {"Up", VK_UP},
    {"Down", VK_DOWN},         {"PrintScreen", VK_SNAPSHOT}, {"PrtSc", VK_SNAPSHOT}, {"Pause", VK_PAUSE},
};

constexpr UINT kModAlt = MOD_ALT, kModCtrl = MOD_CONTROL, kModShift = MOD_SHIFT, kModWin = MOD_WIN;

bool ParseKey(std::string_view t, UINT& vk) {
    if (t.size() == 1) {
        const char c = t[0];
        if (c >= 'a' && c <= 'z') { vk = static_cast<UINT>('A' + (c - 'a')); return true; }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { vk = static_cast<UINT>(c); return true; }
        return false;
    }
    auto number_after = [&t](size_t prefix, int lo, int hi, int& n) {
        if (t.size() <= prefix || t.size() > prefix + 2) return false;
        n = 0;
        for (size_t i = prefix; i < t.size(); ++i) {
            if (t[i] < '0' || t[i] > '9') return false;
            n = n * 10 + (t[i] - '0');
        }
        return n >= lo && n <= hi && !(t[prefix] == '0' && t.size() > prefix + 1);
    };
    int n = 0;
    if ((t[0] == 'F' || t[0] == 'f') && number_after(1, 1, 24, n)) { vk = static_cast<UINT>(VK_F1 + n - 1); return true; }
    if (t.size() == 4 && IEquals(t.substr(0, 3), "num") && number_after(3, 0, 9, n)) {
        vk = static_cast<UINT>(VK_NUMPAD0 + n);
        return true;
    }
    for (const KeyName& k : kKeys)
        if (IEquals(t, k.name)) { vk = k.vk; return true; }
    return false;
}

} // namespace

bool ParseHotkey(std::string_view text, Hotkey& out) {
    const std::string_view trimmed = Trim(text);
    if (IEquals(trimmed, "none")) {
        out = Hotkey{};
        return true;
    }
    Hotkey hk;
    for (std::string_view part : Split(trimmed, '+', true)) {
        part = Trim(part);
        if (part.empty()) return false;
        if (IEquals(part, "ctrl") || IEquals(part, "control")) hk.modifiers |= kModCtrl;
        else if (IEquals(part, "shift")) hk.modifiers |= kModShift;
        else if (IEquals(part, "alt")) hk.modifiers |= kModAlt;
        else if (IEquals(part, "win") || IEquals(part, "windows")) hk.modifiers |= kModWin;
        else {
            UINT vk = 0;
            if (hk.vk != 0 || !ParseKey(part, vk)) return false;   // second key or unknown name
            hk.vk = vk;
        }
    }
    if (hk.vk == 0) return false;
    out = hk;
    return true;
}

std::string HotkeyToString(Hotkey hk) {
    if (hk.vk == 0) return "None";
    std::string s;
    if (hk.modifiers & kModCtrl) s += "Ctrl+";
    if (hk.modifiers & kModAlt) s += "Alt+";
    if (hk.modifiers & kModShift) s += "Shift+";
    if (hk.modifiers & kModWin) s += "Win+";
    const UINT vk = hk.vk;
    char buf[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        s.push_back(static_cast<char>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(buf, sizeof(buf), "F%u", vk - VK_F1 + 1);
        s += buf;
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(buf, sizeof(buf), "Num%u", vk - VK_NUMPAD0);
        s += buf;
    } else {
        const char* name = nullptr;
        for (const KeyName& k : kKeys)
            if (k.vk == vk) { name = k.name; break; }
        if (name) {
            s += name;
        } else {
            std::snprintf(buf, sizeof(buf), "VK%02X", vk);   // not parseable, but never empty
            s += buf;
        }
    }
    return s;
}

// ---- Settings ------------------------------------------------------------------------------------------------

namespace {

Json NullableString(const std::string& s) { return s.empty() ? Json() : Json(s); }

std::string ReadNullableString(const Json& section, std::string_view key, const std::string& def) {
    const Json& v = section[key];
    if (v.IsString()) return v.AsString();
    if (v.IsNull() && section.Has(key)) return std::string();
    return def;
}

Hotkey ReadHotkey(const Json& section, std::string_view key, Hotkey def) {
    Hotkey hk;
    const Json& v = section[key];
    if (v.IsString() && ParseHotkey(v.AsString(), hk)) return hk;
    return def;
}

std::string OneOf(const std::string& v, std::initializer_list<const char*> allowed, const char* def) {
    for (const char* a : allowed)
        if (v == a) return v;
    return def;
}

// Keys of `src` that `known` does not have, recursing into objects both sides have.
Json UnknownKeys(const Json& src, const Json& known) {
    Json out = Json::Object();
    for (const auto& [key, value] : src.Items()) {
        if (!known.Has(key)) {
            out.Set(key, value);
        } else if (value.IsObject() && known[key].IsObject()) {
            Json nested = UnknownKeys(value, known[key]);
            if (nested.Size() > 0) out.Set(key, std::move(nested));
        }
    }
    return out;
}

void MergeUnknown(Json& dst, const Json& extra) {
    for (const auto& [key, value] : extra.Items()) {
        if (!dst.Has(key)) dst.Set(key, value);
        else if (value.IsObject() && dst[key].IsObject()) MergeUnknown(dst[key], value);
    }
}

Json KnownJson(const Settings& s, int version) {
    Json j = Json::Object();
    j["version"] = version;
    Json& r = j["recording"];
    r["fps"] = s.recording.fps;
    r["codec"] = s.recording.codec;
    r["quality"] = s.recording.quality;
    r["mic_device"] = NullableString(s.recording.mic_device);
    r["system_audio"] = s.recording.system_audio;
    r["mic"] = s.recording.mic;
    r["webcam_device"] = NullableString(s.recording.webcam_device);
    r["webcam_resolution"] = s.recording.webcam_resolution;
    r["countdown_s"] = s.recording.countdown_s;
    r["keystrokes"] = s.recording.keystrokes;
    r["cursor_hz"] = s.recording.cursor_hz;
    r["output_dir"] = NullableString(s.recording.output_dir);
    r["hide_bar_while_recording"] = s.recording.hide_bar_while_recording;
    Json& h = j["hotkeys"];
    h["toggle"] = HotkeyToString(s.hotkeys.toggle);
    h["pause"] = HotkeyToString(s.hotkeys.pause);
    h["marker"] = HotkeyToString(s.hotkeys.marker);
    h["cancel"] = HotkeyToString(s.hotkeys.cancel);
    Json& e = j["editor"];
    e["theme"] = s.editor.theme;
    e["autosave_s"] = s.editor.autosave_s;
    e["frame_cache_mb"] = s.editor.frame_cache_mb;
    e["snap"] = s.editor.snap;
    e["ripple"] = s.editor.ripple;
    e["preview_quality"] = s.editor.preview_quality;
    Json& x = j["export"];
    x["last_preset"] = s.export_.last_preset;
    x["last_dir"] = NullableString(s.export_.last_dir);
    j["privacy"]["crash_dumps"] = s.privacy.crash_dumps;
    Json& we = j["window"]["editor"];
    we["x"] = s.window.editor.x;
    we["y"] = s.window.editor.y;
    we["w"] = s.window.editor.w;
    we["h"] = s.window.editor.h;
    we["maximized"] = s.window.editor.maximized;
    Json& wb = j["window"]["bar"];
    wb["x"] = s.window.bar.x;
    wb["y"] = s.window.bar.y;
    return j;
}

} // namespace

Settings Settings::Defaults() { return Settings{}; }

Json Settings::ToJson() const {
    Json j = KnownJson(*this, m_version);
    MergeUnknown(j, m_extra);
    return j;
}

Settings Settings::FromJson(const Json& j) {
    Settings s;
    const Settings d;
    s.m_version = j.Get<int>("version", 0);

    const Json& r = j["recording"];
    s.recording.fps = r.Get<int>("fps", d.recording.fps);
    s.recording.codec = r.Get<std::string>("codec", d.recording.codec);
    s.recording.quality = r.Get<int>("quality", d.recording.quality);
    s.recording.mic_device = ReadNullableString(r, "mic_device", d.recording.mic_device);
    s.recording.system_audio = r.Get<bool>("system_audio", d.recording.system_audio);
    s.recording.mic = r.Get<bool>("mic", d.recording.mic);
    s.recording.webcam_device = ReadNullableString(r, "webcam_device", d.recording.webcam_device);
    s.recording.webcam_resolution = r.Get<std::string>("webcam_resolution", d.recording.webcam_resolution);
    s.recording.countdown_s = r.Get<int>("countdown_s", d.recording.countdown_s);
    s.recording.keystrokes = r.Get<std::string>("keystrokes", d.recording.keystrokes);
    s.recording.cursor_hz = r.Get<int>("cursor_hz", d.recording.cursor_hz);
    s.recording.output_dir = ReadNullableString(r, "output_dir", d.recording.output_dir);
    s.recording.hide_bar_while_recording = r.Get<bool>("hide_bar_while_recording", d.recording.hide_bar_while_recording);

    const Json& h = j["hotkeys"];
    s.hotkeys.toggle = ReadHotkey(h, "toggle", d.hotkeys.toggle);
    s.hotkeys.pause = ReadHotkey(h, "pause", d.hotkeys.pause);
    s.hotkeys.marker = ReadHotkey(h, "marker", d.hotkeys.marker);
    s.hotkeys.cancel = ReadHotkey(h, "cancel", d.hotkeys.cancel);

    const Json& e = j["editor"];
    s.editor.theme = e.Get<std::string>("theme", d.editor.theme);
    s.editor.autosave_s = e.Get<int>("autosave_s", d.editor.autosave_s);
    s.editor.frame_cache_mb = e.Get<int>("frame_cache_mb", d.editor.frame_cache_mb);
    s.editor.snap = e.Get<bool>("snap", d.editor.snap);
    s.editor.ripple = e.Get<bool>("ripple", d.editor.ripple);
    s.editor.preview_quality = e.Get<std::string>("preview_quality", d.editor.preview_quality);

    const Json& x = j["export"];
    s.export_.last_preset = x.Get<std::string>("last_preset", d.export_.last_preset);
    s.export_.last_dir = ReadNullableString(x, "last_dir", d.export_.last_dir);

    s.privacy.crash_dumps = j["privacy"].Get<bool>("crash_dumps", d.privacy.crash_dumps);

    const Json& we = j["window"]["editor"];
    s.window.editor.x = we.Get<int>("x", d.window.editor.x);
    s.window.editor.y = we.Get<int>("y", d.window.editor.y);
    s.window.editor.w = we.Get<int>("w", d.window.editor.w);
    s.window.editor.h = we.Get<int>("h", d.window.editor.h);
    s.window.editor.maximized = we.Get<bool>("maximized", d.window.editor.maximized);
    const Json& wb = j["window"]["bar"];
    s.window.bar.x = wb.Get<int>("x", d.window.bar.x);
    s.window.bar.y = wb.Get<int>("y", d.window.bar.y);

    s.Sanitize();
    s.m_extra = UnknownKeys(j, KnownJson(s, s.m_version));
    return s;
}

void Settings::Sanitize() {
    const Settings d;
    recording.fps = std::clamp(recording.fps, 1, 240);
    recording.codec = OneOf(recording.codec, {"h264", "hevc"}, "h264");
    recording.quality = std::clamp(recording.quality, 1, 100);
    recording.countdown_s = std::clamp(recording.countdown_s, 0, 10);
    recording.cursor_hz = std::clamp(recording.cursor_hz, 30, 1000);
    recording.keystrokes = OneOf(recording.keystrokes, {"shortcuts", "all", "off"}, "shortcuts");
    editor.theme = OneOf(editor.theme, {"dark", "light"}, "dark");
    editor.autosave_s = std::clamp(editor.autosave_s, 0, 3600);
    editor.frame_cache_mb = std::clamp(editor.frame_cache_mb, 32, 16384);
    editor.preview_quality = OneOf(editor.preview_quality, {"full", "half"}, "full");
    if (window.editor.w < 640 || window.editor.h < 400) {
        window.editor.w = d.window.editor.w;
        window.editor.h = d.window.editor.h;
    }
}

void Settings::Migrate() {
    if (m_version >= kVersion) return;   // current, or written by a newer build: keep as is
    // Version 0 (no "version" key) -> 1: same layout. Future steps go here: if (m_version < 2) { ...; m_version = 2; }
    m_version = kVersion;
}

bool Settings::Load(std::string_view path, Err* err) {
    *this = Defaults();
    if (!fs::Exists(path)) return true;
    std::string text;
    if (!fs::ReadAll(path, text)) {
        if (err) err->Set(Format("Could not read the settings file %.*s.", static_cast<int>(path.size()), path.data()));
        return false;
    }
    Json j;
    std::string parse_error;
    if (!Json::Parse(text, j, &parse_error) || !j.IsObject()) {
        if (err)
            err->Set(Format("The settings file is damaged (%s). Defaults are used.",
                            parse_error.empty() ? "not a JSON object" : parse_error.c_str()));
        CT_LOG_WARN("settings %.*s: %s", static_cast<int>(path.size()), path.data(), parse_error.c_str());
        return false;
    }
    *this = FromJson(j);
    Migrate();
    return true;
}

bool Settings::Save(std::string_view path, Err* err) const {
    const std::string_view parent = PathParent(path);
    if (!parent.empty()) fs::CreateDirectories(parent);
    if (!fs::WriteAtomic(path, ToJson().Dump(2) + "\n")) {
        if (err) err->Set(Format("Could not save the settings file %.*s.", static_cast<int>(path.size()), path.data()));
        return false;
    }
    return true;
}

} // namespace ct
