#pragma once
// ct/ui/theme.h
// Design tokens of docs/05 section 1 (colors, radii, spacing, geometry) and the ImGui style mapping.
// All sizes are in DIPs; multiply by Dp() at draw time (the DPI scale is style.FontScaleDpi, set by the app on
// every WM_DPICHANGED). Thread model: UI thread (reads the current ImGui context).

#include <imgui.h>

#include <cstdint>

namespace ct::ui {

// 0xRRGGBB (+ alpha 0..255) -> ImU32 (ImGui packs ABGR).
constexpr ImU32 Rgb(uint32_t rgb, uint32_t a = 255) {
    return (a << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}
// Same color with alpha multiplied by `alpha` (0..1).
ImU32 WithAlpha(ImU32 col, float alpha);
// Linear blend a -> b.
ImU32 Mix(ImU32 a, ImU32 b, float t);
ImVec4 ToVec4(ImU32 col);

namespace col {
constexpr ImU32 bg0 = Rgb(0x0F1115);          // window background
constexpr ImU32 bg1 = Rgb(0x161920);          // panels, timeline background
constexpr ImU32 bg2 = Rgb(0x1E222B);          // cards, inputs, track lanes
constexpr ImU32 bg2_alt = Rgb(0x1B1F27);      // alternate lane rows
constexpr ImU32 bg3 = Rgb(0x262B36);          // hover
constexpr ImU32 bg4 = Rgb(0x2E3441);          // pressed / active
constexpr ImU32 border = Rgb(0x2C323E);       // 1 px borders, separators
constexpr ImU32 text = Rgb(0xE6E8EE);
constexpr ImU32 text_dim = Rgb(0x9AA3B2);
constexpr ImU32 text_faint = Rgb(0x5F6878);
constexpr ImU32 accent = Rgb(0x4F8CFF);
constexpr ImU32 accent_hover = Rgb(0x6CA0FF);
constexpr ImU32 accent_soft = Rgb(0x4F8CFF, 46);   // 18 %
constexpr ImU32 record = Rgb(0xFF4D4F);
constexpr ImU32 success = Rgb(0x3DDC84);
constexpr ImU32 warn = Rgb(0xFFB020);
constexpr ImU32 danger = Rgb(0xFF4D4F);
// Clip kinds: fill + text.
constexpr ImU32 clip_video = Rgb(0x2B6CB0), clip_video_text = Rgb(0x63B3ED);
constexpr ImU32 clip_camera = Rgb(0x6B46C1), clip_camera_text = Rgb(0xB794F4);
constexpr ImU32 clip_audio = Rgb(0x276749), clip_audio_text = Rgb(0x68D391), waveform = Rgb(0x9AE6B4);
constexpr ImU32 clip_annotation = Rgb(0x975A16), clip_annotation_text = Rgb(0xF6E05E);
constexpr ImU32 camera_kf = Rgb(0x4F8CFF);
} // namespace col

namespace metrics {
// Radii
constexpr float kRadiusControl = 6, kRadiusCard = 10, kRadiusCanvas = 14, kRadiusClip = 6, kRadiusPill = 999;
// Spacing scale 4 / 8 / 12 / 16 / 24 / 32
constexpr float kSp1 = 4, kSp2 = 8, kSp3 = 12, kSp4 = 16, kSp5 = 24, kSp6 = 32;
constexpr float kPanelPadding = 12;
constexpr float kControlCompact = 28, kControlDefault = 32;
// Timeline geometry (docs/05 section 4)
constexpr float kTlHeader = 160, kTlRuler = 24, kTlGap = 2, kTlClipInset = 1;
constexpr float kTlVideo = 72, kTlCamera = 56, kTlAudio = 56, kTlAnnotation = 40, kTlCameraKf = 28;
constexpr float kTlTrimHandle = 8, kTlSnapPx = 8, kTlDragThreshold = 3, kTlDiamond = 8;
// Editor default layout at 1600x900
constexpr float kMediaWidth = 260, kPropertiesWidth = 320, kTimelineHeight = 300;
constexpr float kPreviewMargin = 24;
constexpr float kMotionMs = 160;   // fades/slides of panels, popovers, toasts
} // namespace metrics

// v * current DPI scale (style.FontScaleDpi; 1 when no context).
float Dp(float v);
ImVec2 Dp(float x, float y);

struct Theme {
    // docs/05 section 1 "ImGui style mapping": sizes at 96 dpi and colors. The app scales sizes afterwards
    // (style.ScaleAllSizes(dpi)) and sets style.FontScaleDpi.
    static void Apply(ImGuiStyle& style);
    // WCAG relative contrast ratio of two opaque colors (accessibility baseline: >= 4.5 for text tokens).
    static float Contrast(ImU32 fg, ImU32 bg);
};

// Ease-out cubic for the 160 ms UI motion (t in [0,1]).
float EaseOut(float t);

} // namespace ct::ui
