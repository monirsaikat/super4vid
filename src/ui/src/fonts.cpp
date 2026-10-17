// System fonts + merged icon font (docs/05 section 1).
#define CT_LOG_CATEGORY "ui"
#include <ct/core/log.h>
#include <ct/ui/fonts.h>

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace ct::ui {
namespace {

ImFont* g_body = nullptr;
ImFont* g_strong = nullptr;
ImFont* g_mono = nullptr;
const char* g_icon_name = "";

// Persist for the atlas lifetime (ImFontConfig::GlyphRanges must outlive the font).
const ImWchar kIconRanges[] = {0xE700, 0xF8FF, 0};

bool Exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

const char* Pick(char* buf, size_t n, const char* dir, const char* const* names, int count) {
    for (int i = 0; i < count; ++i) {
        std::snprintf(buf, n, "%s\\%s", dir, names[i]);
        if (Exists(buf)) return buf;
    }
    return nullptr;
}

void MergeIcons(ImFontAtlas& atlas, const char* icon_path, float size) {
    if (!icon_path) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.GlyphRanges = kIconRanges;
    cfg.GlyphMinAdvanceX = size;      // icons are square; keep them aligned in buttons
    cfg.GlyphOffset = ImVec2(0, 2.0f);   // Segoe icon glyphs sit above the text baseline
    cfg.PixelSnapH = true;
    atlas.AddFontFromFileTTF(icon_path, size, &cfg);
}

} // namespace

bool Fonts::Load(ImFontAtlas& atlas, const char* fonts_dir) {
    char dir_buf[MAX_PATH];
    if (!fonts_dir) {
        const UINT n = GetWindowsDirectoryA(dir_buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 8) std::snprintf(dir_buf, sizeof(dir_buf), "C:\\Windows");
        std::snprintf(dir_buf + std::strlen(dir_buf), sizeof(dir_buf) - std::strlen(dir_buf), "\\Fonts");
        fonts_dir = dir_buf;
    }
    static const char* const kBody[] = {"SegUIVar.ttf", "segoeui.ttf"};
    static const char* const kStrong[] = {"seguisb.ttf", "SegUIVar.ttf", "segoeui.ttf"};
    static const char* const kMono[] = {"CascadiaMono.ttf", "consola.ttf"};
    static const char* const kIcons[] = {"SegoeIcons.ttf", "segmdl2.ttf"};
    char body_path[MAX_PATH], strong_path[MAX_PATH], mono_path[MAX_PATH], icon_path[MAX_PATH];
    const char* body = Pick(body_path, MAX_PATH, fonts_dir, kBody, 2);
    const char* strong = Pick(strong_path, MAX_PATH, fonts_dir, kStrong, 3);
    const char* mono = Pick(mono_path, MAX_PATH, fonts_dir, kMono, 2);
    const char* icons = Pick(icon_path, MAX_PATH, fonts_dir, kIcons, 2);
    g_icon_name = !icons ? "" : (std::strstr(icons, "SegoeIcons") ? "Segoe Fluent Icons" : "Segoe MDL2 Assets");

    const float body_px = Size(FontRole::Body);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    g_body = body ? atlas.AddFontFromFileTTF(body, body_px, &cfg) : nullptr;
    if (!g_body) {
        CT_LOG_WARN("no system UI font in %s; using the ImGui default font", fonts_dir);
        g_body = atlas.AddFontDefault();
    }
    if (!g_body) return false;
    MergeIcons(atlas, icons, body_px);

    g_strong = strong ? atlas.AddFontFromFileTTF(strong, body_px, &cfg) : nullptr;
    if (g_strong) MergeIcons(atlas, icons, body_px);
    else g_strong = g_body;

    g_mono = mono ? atlas.AddFontFromFileTTF(mono, Size(FontRole::Mono)) : nullptr;
    if (!g_mono) g_mono = g_body;

    CT_LOG_INFO("UI fonts: body %s, strong %s, mono %s, icons %s", body ? body : "(default)",
                strong ? strong : "(body)", mono ? mono : "(body)", icons ? icons : "(none)");
    return true;
}

void Fonts::Unload() {
    g_body = g_strong = g_mono = nullptr;
    g_icon_name = "";
}

ImFont* Fonts::Get(FontRole role) {
    switch (role) {
    case FontRole::Title:
    case FontRole::Strong: return g_strong;
    case FontRole::Mono: return g_mono;
    default: return g_body;
    }
}

float Fonts::Size(FontRole role) {
    switch (role) {
    case FontRole::Label: return 12;
    case FontRole::Title: return 20;
    case FontRole::Mono: return 12;
    case FontRole::Icon: return 16;
    case FontRole::IconLarge: return 20;
    default: return 14;
    }
}

bool Fonts::HasIcons() { return g_icon_name[0] != 0; }
const char* Fonts::IconFontName() { return g_icon_name; }

FontScope::FontScope(FontRole role) { ImGui::PushFont(Fonts::Get(role), Fonts::Size(role)); }
FontScope::~FontScope() { ImGui::PopFont(); }

} // namespace ct::ui
