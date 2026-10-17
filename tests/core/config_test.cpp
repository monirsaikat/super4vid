// ct/core/config.h: settings defaults/load/save/migrate, unknown-key preservation, hotkeys.
#include <ct/core/config.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

struct TempDir {
    std::string path = ct::fs::TempPath("");
    TempDir() { ct::fs::CreateDirectories(path); }
    ~TempDir() { ct::fs::DeleteTree(path); }
};

std::string SettingsSampleFromDocs() {
    std::string md;
    if (!ct::fs::ReadAll("../docs/04-file-formats.md", md)) ct::fs::ReadAll("docs/04-file-formats.md", md);
    const size_t section = md.find("## 4. Settings");
    const size_t start = md.find("```json", section);
    if (section == std::string::npos || start == std::string::npos) return std::string();
    const size_t body = md.find('\n', start) + 1;
    return md.substr(body, md.find("```", body) - body);
}

} // namespace

CT_TEST(config_defaults_match_docs_sample) {
    std::string sample = SettingsSampleFromDocs();
    CT_REQUIRE(!sample.empty());
    const size_t placeholder = sample.find("\"full|half\"");
    CT_REQUIRE(placeholder != std::string::npos);
    sample.replace(placeholder, 11, "\"full\"");   // docs show the enum's options; the default is "full"
    ct::Json doc;
    CT_REQUIRE(ct::Json::Parse(sample, doc));
    const ct::Json defaults = ct::Settings::Defaults().ToJson();
    CT_CHECK(defaults == doc);   // same keys, same order, same values
    if (!(defaults == doc)) std::printf("%s\n", defaults.Dump().c_str());
    const ct::Settings parsed = ct::Settings::FromJson(doc);
    CT_CHECK(parsed.Extra().Size() == 0);
    CT_CHECK_EQ(parsed.ToJson().Dump(), defaults.Dump());
}

CT_TEST(config_save_load_roundtrip) {
    TempDir dir;
    const std::string path = ct::PathJoin(dir.path, "sub\\settings.json");   // parent is created by Save
    ct::Settings missing;
    ct::Err err;
    CT_CHECK(missing.Load(path, &err));   // missing file = defaults
    CT_CHECK(err.Ok());
    CT_CHECK_EQ(missing.ToJson().Dump(), ct::Settings::Defaults().ToJson().Dump());

    ct::Settings s = ct::Settings::Defaults();
    s.recording.fps = 30;
    s.recording.mic_device = "{0.0.1.00000000}.{guid}";
    s.recording.output_dir = "D:\\Videos\\caf\xC3\xA9";
    s.hotkeys.marker = ct::Hotkey{MOD_ALT, 'M'};
    s.editor.theme = "light";
    s.export_.last_dir = "C:\\exports";
    s.window.editor = {-1200, 40, 1280, 720, true};
    CT_REQUIRE(s.Save(path, &err));
    ct::Settings back;
    CT_REQUIRE(back.Load(path, &err));
    CT_CHECK_EQ(back.ToJson().Dump(), s.ToJson().Dump());
    CT_CHECK_EQ(back.recording.fps, 30);
    CT_CHECK_EQ(back.recording.output_dir, s.recording.output_dir);
    CT_CHECK(back.hotkeys.marker == s.hotkeys.marker);
    CT_CHECK_EQ(back.window.editor.x, -1200);
    CT_CHECK(back.window.editor.maximized);
    CT_CHECK_EQ(back.Version(), ct::Settings::kVersion);
    std::string text;
    CT_REQUIRE(ct::fs::ReadAll(path, text));
    CT_CHECK(text.find("\"mic_device\": \"{0.0.1.00000000}.{guid}\"") != std::string::npos);
    CT_CHECK(text.find("\"webcam_device\": null") != std::string::npos);
    CT_CHECK(text.find("\"marker\": \"Alt+M\"") != std::string::npos);
}

CT_TEST(config_unknown_keys_preserved) {
    TempDir dir;
    const std::string path = ct::PathJoin(dir.path, "settings.json");
    const char* text = R"({
  "version": 1,
  "future_feature": { "enabled": true, "list": [1, 2, 3] },
  "recording": { "fps": 50, "new_knob": 5, "codec": "hevc" },
  "window": { "editor": { "x": 5, "monitor": "DISPLAY2" }, "third": { "x": 1 } }
})";
    CT_REQUIRE(ct::fs::WriteAtomic(path, text));
    ct::Settings s;
    ct::Err err;
    CT_REQUIRE(s.Load(path, &err));
    CT_CHECK_EQ(s.recording.fps, 50);
    CT_CHECK_EQ(s.recording.codec, std::string("hevc"));
    CT_CHECK_EQ(s.window.editor.x, 5);
    CT_CHECK_EQ(s.window.editor.w, 1600);   // missing key -> default
    s.recording.fps = 24;
    CT_REQUIRE(s.Save(path, &err));
    std::string saved;
    CT_REQUIRE(ct::fs::ReadAll(path, saved));
    ct::Json j;
    CT_REQUIRE(ct::Json::Parse(saved, j));
    CT_CHECK(j["future_feature"]["enabled"].AsBool());
    CT_CHECK_EQ(j["future_feature"]["list"].Size(), size_t(3));
    CT_CHECK_EQ(j["recording"]["new_knob"].AsInt(), 5);
    CT_CHECK_EQ(j["recording"]["fps"].AsInt(), 24);
    CT_CHECK_EQ(j["window"]["editor"]["monitor"].AsString(), std::string("DISPLAY2"));
    CT_CHECK_EQ(j["window"]["third"]["x"].AsInt(), 1);
    // Known keys come first in docs order, unknown keys after them.
    CT_CHECK_EQ(j.Items()[0].first, std::string("version"));
    CT_CHECK_EQ(j.Items().back().first, std::string("future_feature"));
}

CT_TEST(config_bad_values_and_versions) {
    ct::Json j;
    CT_REQUIRE(ct::Json::Parse(R"({"recording": {"fps": 1000, "quality": "high", "codec": "vp9", "mic": 1,
        "countdown_s": -4, "mic_device": 7}, "hotkeys": {"toggle": "Ctrl+Nope", "pause": "None"},
        "editor": {"frame_cache_mb": 1, "theme": "neon"}, "window": {"editor": {"w": 10, "h": 10}}})", j));
    const ct::Settings s = ct::Settings::FromJson(j);
    CT_CHECK_EQ(s.recording.fps, 240);                       // clamped
    CT_CHECK_EQ(s.recording.quality, 70);                    // wrong type -> default
    CT_CHECK_EQ(s.recording.codec, std::string("h264"));     // unknown enum -> default
    CT_CHECK(s.recording.mic);                               // number is not a bool -> default
    CT_CHECK_EQ(s.recording.countdown_s, 0);
    CT_CHECK(s.recording.mic_device.empty());
    CT_CHECK(s.hotkeys.toggle == ct::Settings::Defaults().hotkeys.toggle);   // unparseable -> default
    CT_CHECK(!s.hotkeys.pause.IsSet());                       // "None" disables
    CT_CHECK_EQ(s.editor.frame_cache_mb, 32);
    CT_CHECK_EQ(s.editor.theme, std::string("dark"));
    CT_CHECK_EQ(s.window.editor.w, 1600);
    CT_CHECK_EQ(s.Version(), 0);   // no "version" key

    ct::Settings old = s;
    old.Migrate();
    CT_CHECK_EQ(old.Version(), ct::Settings::kVersion);

    CT_REQUIRE(ct::Json::Parse(R"({"version": 7, "recording": {"fps": 30}})", j));
    ct::Settings newer = ct::Settings::FromJson(j);
    newer.Migrate();
    CT_CHECK_EQ(newer.Version(), 7);   // a newer build's file keeps its version
    CT_CHECK_EQ(newer.ToJson()["version"].AsInt(), 7);

    TempDir dir;
    const std::string path = ct::PathJoin(dir.path, "settings.json");
    CT_REQUIRE(ct::fs::WriteAtomic(path, "{ \"version\": 1, \"recording\": { \"fps\": 30, } }"));
    ct::Settings broken;
    broken.recording.fps = 5;
    ct::Err err;
    CT_CHECK(!broken.Load(path, &err));
    CT_CHECK(!err.Ok());
    CT_CHECK(err.message.find("line 1") != std::string::npos);
    CT_CHECK_EQ(broken.recording.fps, 60);   // defaults after a failed load
}

CT_TEST(config_hotkey_parse_format) {
    ct::Hotkey hk;
    CT_REQUIRE(ct::ParseHotkey("Ctrl+Shift+F9", hk));
    CT_CHECK_EQ(hk.modifiers, UINT(MOD_CONTROL | MOD_SHIFT));
    CT_CHECK_EQ(hk.vk, UINT(VK_F9));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Ctrl+Shift+F9"));
    CT_REQUIRE(ct::ParseHotkey("  shift + control + f9 ", hk));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Ctrl+Shift+F9"));
    CT_REQUIRE(ct::ParseHotkey("win+alt+a", hk));
    CT_CHECK_EQ(hk.modifiers, UINT(MOD_WIN | MOD_ALT));
    CT_CHECK_EQ(hk.vk, UINT('A'));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Alt+Win+A"));
    CT_REQUIRE(ct::ParseHotkey("F24", hk));
    CT_CHECK_EQ(hk.vk, UINT(VK_F24));
    CT_REQUIRE(ct::ParseHotkey("Ctrl+3", hk));
    CT_CHECK_EQ(hk.vk, UINT('3'));
    CT_REQUIRE(ct::ParseHotkey("Alt+PgDn", hk));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Alt+PageDown"));
    CT_REQUIRE(ct::ParseHotkey("Ctrl+Num7", hk));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Ctrl+Num7"));
    CT_REQUIRE(ct::ParseHotkey("Escape", hk));
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("Esc"));
    CT_REQUIRE(ct::ParseHotkey("none", hk));
    CT_CHECK(!hk.IsSet());
    CT_CHECK_EQ(ct::HotkeyToString(hk), std::string("None"));
    for (const char* bad : {"", "Ctrl", "Ctrl+Shift", "Ctrl+A+B", "Ctrl+Foo", "F0", "F25", "F09", "Ctrl++A", "+A", "A+"})
        CT_CHECK(!ct::ParseHotkey(bad, hk));
    // Every printable hotkey parses back to itself.
    bool roundtrip = true;
    for (UINT vk : {UINT('Z'), UINT('0'), UINT(VK_F1), UINT(VK_SPACE), UINT(VK_DELETE), UINT(VK_SNAPSHOT), UINT(VK_NUMPAD0)})
        for (UINT mods = 0; mods < 16; ++mods) {
            ct::Hotkey in{mods, vk}, out;
            if (!ct::ParseHotkey(ct::HotkeyToString(in), out) || !(out == in)) roundtrip = false;
        }
    CT_CHECK(roundtrip);
}
