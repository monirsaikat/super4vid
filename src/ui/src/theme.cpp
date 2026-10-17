// Design tokens -> ImGui style (docs/05 section 1).
#include <ct/ui/theme.h>

#include <cmath>

namespace ct::ui {

ImU32 WithAlpha(ImU32 c, float alpha) {
    const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ImU32 out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<ImU32>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

ImVec4 ToVec4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

float Dp(float v) { return ImGui::GetCurrentContext() ? v * ImGui::GetStyle().FontScaleDpi : v; }
ImVec2 Dp(float x, float y) { return ImVec2(Dp(x), Dp(y)); }

float EaseOut(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float u = 1 - t;
    return 1 - u * u * u;
}

static float Linearize(float c) { return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
static float Luminance(ImU32 c) {
    const ImVec4 v = ToVec4(c);
    return 0.2126f * Linearize(v.x) + 0.7152f * Linearize(v.y) + 0.0722f * Linearize(v.z);
}

float Theme::Contrast(ImU32 fg, ImU32 bg) {
    float a = Luminance(fg), b = Luminance(bg);
    if (a < b) {
        const float t = a;
        a = b;
        b = t;
    }
    return (a + 0.05f) / (b + 0.05f);
}

void Theme::Apply(ImGuiStyle& s) {
    s = ImGuiStyle();
    s.WindowRounding = 10;
    s.ChildRounding = 10;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(8, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.TabBarBorderSize = 0;
    s.ChildBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 1;
    s.DockingSeparatorSize = 2;
    s.TabBarOverlineSize = 0;
    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;

    ImVec4* c = s.Colors;
    const ImVec4 transparent(0, 0, 0, 0);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = ToVec4(col::bg2);
    c[ImGuiCol_Text] = ToVec4(col::text);
    c[ImGuiCol_TextDisabled] = ToVec4(col::text_faint);
    c[ImGuiCol_WindowBg] = ToVec4(col::bg1);
    c[ImGuiCol_ChildBg] = ToVec4(col::bg1);
    c[ImGuiCol_PopupBg] = ToVec4(col::bg2);
    c[ImGuiCol_Border] = ToVec4(col::border);
    c[ImGuiCol_BorderShadow] = transparent;
    c[ImGuiCol_FrameBg] = ToVec4(col::bg2);
    c[ImGuiCol_FrameBgHovered] = ToVec4(col::bg3);
    c[ImGuiCol_FrameBgActive] = ToVec4(col::bg4);
    c[ImGuiCol_TitleBg] = ToVec4(col::bg0);
    c[ImGuiCol_TitleBgActive] = ToVec4(col::bg0);
    c[ImGuiCol_TitleBgCollapsed] = ToVec4(col::bg0);
    c[ImGuiCol_MenuBarBg] = ToVec4(col::bg0);
    c[ImGuiCol_ScrollbarBg] = transparent;
    c[ImGuiCol_ScrollbarGrab] = ToVec4(col::bg3);
    c[ImGuiCol_ScrollbarGrabHovered] = ToVec4(col::bg4);
    c[ImGuiCol_ScrollbarGrabActive] = ToVec4(col::bg4);
    c[ImGuiCol_CheckMark] = ToVec4(col::accent);
    c[ImGuiCol_CheckboxSelectedBg] = ToVec4(col::bg2);
    c[ImGuiCol_SliderGrab] = ToVec4(col::accent);
    c[ImGuiCol_SliderGrabActive] = ToVec4(col::accent_hover);
    c[ImGuiCol_Button] = ToVec4(col::bg2);
    c[ImGuiCol_ButtonHovered] = ToVec4(col::bg3);
    c[ImGuiCol_ButtonActive] = ToVec4(col::bg4);
    c[ImGuiCol_Header] = ToVec4(col::accent_soft);
    c[ImGuiCol_HeaderHovered] = ToVec4(col::bg3);
    c[ImGuiCol_HeaderActive] = ToVec4(col::bg4);
    c[ImGuiCol_Separator] = ToVec4(col::border);
    c[ImGuiCol_SeparatorHovered] = ToVec4(col::accent);
    c[ImGuiCol_SeparatorActive] = ToVec4(col::accent_hover);
    c[ImGuiCol_ResizeGrip] = transparent;
    c[ImGuiCol_ResizeGripHovered] = ToVec4(col::bg3);
    c[ImGuiCol_ResizeGripActive] = ToVec4(col::bg4);
    c[ImGuiCol_InputTextCursor] = ToVec4(col::text);
    c[ImGuiCol_Tab] = ToVec4(col::bg1);
    c[ImGuiCol_TabHovered] = ToVec4(col::bg3);
    c[ImGuiCol_TabSelected] = ToVec4(col::bg2);
    c[ImGuiCol_TabSelectedOverline] = ToVec4(col::accent);
    c[ImGuiCol_TabDimmed] = ToVec4(col::bg1);
    c[ImGuiCol_TabDimmedSelected] = ToVec4(col::bg1);
    c[ImGuiCol_TabDimmedSelectedOverline] = transparent;
    c[ImGuiCol_DockingPreview] = ToVec4(col::accent_soft);
    c[ImGuiCol_DockingEmptyBg] = ToVec4(col::bg0);
    c[ImGuiCol_PlotLines] = ToVec4(col::accent);
    c[ImGuiCol_PlotLinesHovered] = ToVec4(col::accent_hover);
    c[ImGuiCol_PlotHistogram] = ToVec4(col::accent);
    c[ImGuiCol_PlotHistogramHovered] = ToVec4(col::accent_hover);
    c[ImGuiCol_TableHeaderBg] = ToVec4(col::bg2);
    c[ImGuiCol_TableBorderStrong] = ToVec4(col::border);
    c[ImGuiCol_TableBorderLight] = ToVec4(col::border);
    c[ImGuiCol_TableRowBg] = transparent;
    c[ImGuiCol_TableRowBgAlt] = ToVec4(WithAlpha(col::bg2, 0.5f));
    c[ImGuiCol_TextLink] = ToVec4(col::accent_hover);
    c[ImGuiCol_TextSelectedBg] = ToVec4(Rgb(0x4F8CFF, 90));
    c[ImGuiCol_TreeLines] = ToVec4(col::border);
    c[ImGuiCol_DragDropTarget] = ToVec4(col::accent);
    c[ImGuiCol_DragDropTargetBg] = ToVec4(col::accent_soft);
    c[ImGuiCol_UnsavedMarker] = ToVec4(col::text_dim);
    c[ImGuiCol_NavCursor] = ToVec4(col::accent);
    c[ImGuiCol_NavWindowingHighlight] = ToVec4(col::accent);
    c[ImGuiCol_NavWindowingDimBg] = ToVec4(Rgb(0x000000, 110));
    c[ImGuiCol_ModalWindowDimBg] = ToVec4(Rgb(0x000000, 140));
}

} // namespace ct::ui
