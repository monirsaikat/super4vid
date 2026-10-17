#pragma once
// overlay_style.h (private to ct_recorder)
// docs/05 section 1 color tokens used by the overlays, as local constants: the overlays are drawn with Direct2D on
// layered windows, not with ImGui, so they do not depend on ct_ui's theme (DECISIONS.md, P05 5.5).
// Also the overlay hint strings (English only in v1.0; move to ct/ui/strings.h when ct_ui owns all strings).

#include <ct/platform/win.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace ct::recorder::style {

constexpr D2D1_COLOR_F Rgb(uint32_t hex, float a = 1.f) {
    return D2D1_COLOR_F{static_cast<float>((hex >> 16) & 0xFF) / 255.f, static_cast<float>((hex >> 8) & 0xFF) / 255.f,
                        static_cast<float>(hex & 0xFF) / 255.f, a};
}

constexpr D2D1_COLOR_F kBg2 = Rgb(0x1E222B);
constexpr D2D1_COLOR_F kBorder = Rgb(0x2C323E);
constexpr D2D1_COLOR_F kText = Rgb(0xE6E8EE);
constexpr D2D1_COLOR_F kTextDim = Rgb(0x9AA3B2);
constexpr D2D1_COLOR_F kAccent = Rgb(0x4F8CFF);
constexpr D2D1_COLOR_F kRecord = Rgb(0xFF4D4F);
constexpr D2D1_COLOR_F kWhite = Rgb(0xFFFFFF);
constexpr D2D1_COLOR_F kBlack = Rgb(0x000000);
constexpr D2D1_COLOR_F kDim{0.f, 0.f, 0.f, 0.45f};        // rgba(0,0,0,0.45) outside the selection
constexpr D2D1_COLOR_F kHole{0.f, 0.f, 0.f, 1.f / 255.f}; // inside the selection: invisible but still hit-testable

constexpr const wchar_t* kFontUi = L"Segoe UI Variable Text";
constexpr const wchar_t* kFontDisplay = L"Segoe UI Variable Display";
constexpr const wchar_t* kFontFallback = L"Segoe UI";

constexpr const wchar_t* kHintRegion = L"Drag to select a region · click a window · M monitor · Enter confirm · Esc cancel";
constexpr const wchar_t* kHintAdjust = L"Drag handles to resize · arrows nudge (Shift 10 px) · R restart · Enter confirm · Esc cancel";
constexpr const wchar_t* kHintWindow = L"Click a window to record it · Esc cancel";

// A text format in the first installed family of (preferred, Segoe UI).
inline Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeTextFormat(IDWriteFactory* f, const wchar_t* family, float px,
                                                                DWRITE_FONT_WEIGHT weight) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    const wchar_t* use = kFontFallback;
    if (SUCCEEDED(f->GetSystemFontCollection(fonts.GetAddressOf(), FALSE))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) use = family;
    }
    if (FAILED(f->CreateTextFormat(use, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us",
                                   fmt.GetAddressOf())))
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return fmt;
}

} // namespace ct::recorder::style
