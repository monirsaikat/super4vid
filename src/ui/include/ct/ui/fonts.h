#pragma once
// ct/ui/fonts.h
// System font set of docs/05 section 1, loaded once into the ImGui atlas. ImGui >= 1.92 rasterizes glyphs on demand
// at any size, so a DPI change only sets style.FontScaleDpi (no atlas rebuild; see DECISIONS P00 0.4).
//   Body  "Segoe UI Variable" (SegUIVar.ttf) 14 px, fallback "Segoe UI" (segoeui.ttf)
//   Label body font at 12 px
//   Title "Segoe UI Semibold" (seguisb.ttf) 20 px (stb_truetype cannot pick a variable-font weight instance)
//   Strong semibold at 14 px (section headers, buttons)
//   Mono  "Cascadia Mono" 12 px, fallback "Consolas"
// Icons from "Segoe Fluent Icons" (SegoeIcons.ttf), fallback "Segoe MDL2 Assets" (segmdl2.ttf), are merged into the
// body and semibold fonts (U+E700..U+F8FF), so ICON_* literals can be mixed with text.
// Thread model: UI thread, between frames.

#include <imgui.h>

namespace ct::ui {

enum class FontRole { Body, Label, Title, Strong, Mono, Icon, IconLarge };

struct Fonts {
    // Loads from `fonts_dir` (default C:\Windows\Fonts). Falls back to ImGui's embedded font when no system font
    // exists. Returns false only when nothing could be loaded.
    static bool Load(ImFontAtlas& atlas, const char* fonts_dir = nullptr);
    static void Unload();               // forget the fonts (before destroying the context that owns the atlas)
    static ImFont* Get(FontRole role);
    static float Size(FontRole role);   // unscaled px (DIP)
    static bool HasIcons();             // an icon font was merged
    static const char* IconFontName();  // "Segoe Fluent Icons", "Segoe MDL2 Assets" or ""
};

// RAII ImGui::PushFont(Get(role), Size(role)).
class FontScope {
public:
    explicit FontScope(FontRole role);
    ~FontScope();
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

} // namespace ct::ui
