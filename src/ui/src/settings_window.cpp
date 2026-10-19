// Settings window drawing (P11 11.6; docs/05 sections 1, 4, 7). Layout: a 212 DIP section list on the left (bg0),
// the section on the right as cards (bg2, radius 10) of rows "label + hint | control", footer with Done.
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/settings_window.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/level_monitor.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>
#include <ct/version.h>

#include <imgui_internal.h>

#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace ct::ui {

using namespace metrics;

// Glyphs beyond icons.h used only here; ct_ui_tests settings_icons_present checks both icon fonts.
constexpr const char* ICON_KEYBOARD = "";
constexpr const char* ICON_REFRESH = "";
const char* const kSettingsExtraIcons[kSettingsExtraIconCount] = {ICON_KEYBOARD, ICON_REFRESH};

namespace {

constexpr float kWinW = 840, kWinH = 580, kSidebarW = 212, kFooterH = 60;
constexpr float kContentPadX = 28, kContentPadY = 24, kCardPad = 16, kRowMinH = 52, kCtrlW = 240;
constexpr const char* kPopupId = "##ct_settings";

// ---- Card / row layout -----------------------------------------------------------------------------------------
struct CardLayout {
    ImVec2 start{};
    float width = 0;
    int rows = 0;
    // current row
    float row_y = 0, row_h = 0;
};
CardLayout g_card;

void SubHeading(const char* text) {
    FontScope f(FontRole::Label);
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Dp(6)));
}

void CardBegin(const char* heading = nullptr) {
    if (heading) SubHeading(heading);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    g_card.start = ImGui::GetCursorScreenPos();
    g_card.width = ImGui::GetContentRegionAvail().x;
    g_card.rows = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dp(8), 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToVec4(col::bg1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ToVec4(col::bg3));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ToVec4(col::bg4));
    ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(col::bg3));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(col::bg4));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(Mix(col::bg4, col::text_faint, 0.3f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, Dp(1));
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(col::border));
}

void CardEnd() {
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(g_card.start.x + g_card.width, ImGui::GetCursorScreenPos().y);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(g_card.start, end, col::bg2, Dp(kRadiusCard));
    dl->AddRect(g_card.start, end, col::border, Dp(kRadiusCard));
    dl->ChannelsMerge();
    ImGui::Dummy(ImVec2(0, Dp(20)));
}

float CardInnerLeft() { return g_card.start.x + Dp(kCardPad); }
float CardInnerRight() { return g_card.start.x + g_card.width - Dp(kCardPad); }

void RowSeparator() {
    if (g_card.rows++ == 0) return;
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(CardInnerLeft(), y), ImVec2(CardInnerRight(), y), col::border, Dp(1));
}

float WrappedHeight(const char* text, float wrap, FontRole role) {
    FontScope f(role);
    return ImGui::CalcTextSize(text, nullptr, false, wrap).y;
}

void DrawLabelBlock(float x, float y, float wrap_w, const char* label, const char* hint, ImU32 hint_col = col::text_dim) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
    ImGui::TextUnformatted(label);
    if (hint && *hint) {
        ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y + Dp(2)));
        FontScope f(FontRole::Label);
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(hint_col));
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
}

// Row "label (+ hint) ............ [control ctrl_w wide, ctrl_h tall]". Leaves the cursor at the control.
void RowBegin(const char* label, const char* hint, float ctrl_w, float ctrl_h = 0, ImU32 hint_col = col::text_dim) {
    RowSeparator();
    if (ctrl_h <= 0) ctrl_h = ImGui::GetFrameHeight();
    const float x0 = CardInnerLeft();
    const float y0 = ImGui::GetCursorScreenPos().y;
    const float label_w = CardInnerRight() - x0 - ctrl_w - Dp(24);
    float block_h = ImGui::GetTextLineHeight();
    if (hint && *hint) block_h += Dp(2) + WrappedHeight(hint, label_w, FontRole::Label);
    float row_h = block_h + Dp(24);
    if (row_h < ctrl_h + Dp(20)) row_h = ctrl_h + Dp(20);
    if (row_h < Dp(kRowMinH)) row_h = Dp(kRowMinH);
    DrawLabelBlock(x0, std::floor(y0 + (row_h - block_h) * 0.5f), label_w, label, hint, hint_col);
    g_card.row_y = y0;
    g_card.row_h = row_h;
    ImGui::SetCursorScreenPos(ImVec2(CardInnerRight() - ctrl_w, std::floor(y0 + (row_h - ctrl_h) * 0.5f)));
}

void RowEnd() {
    ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, g_card.row_y));
    ImGui::Dummy(ImVec2(g_card.width, g_card.row_h));
}

// Stacked row: label (+ hint) on top, the controls below across the full card width. Leaves the cursor at the
// control line; RowStackEnd closes it below the last item.
void RowStackBegin(const char* label, const char* hint) {
    RowSeparator();
    const float x0 = CardInnerLeft();
    const float y0 = ImGui::GetCursorScreenPos().y;
    g_card.row_y = y0;
    DrawLabelBlock(x0, y0 + Dp(14), CardInnerRight() - x0, label, hint);
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + Dp(10)));
}

void RowStackEnd() {
    const float bottom = ImGui::GetItemRectMax().y + Dp(14);
    g_card.row_h = bottom - g_card.row_y;
    RowEnd();
}

void Note(const char* icon, const char* text, ImU32 accent) {
    // Tinted callout inside the current card row area (used for privacy notes).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = CardInnerLeft(), x1 = CardInnerRight();
    const float y0 = ImGui::GetCursorScreenPos().y;
    const float icon_w = Dp(16);
    const float wrap = x1 - x0 - Dp(12) * 2 - icon_w - Dp(8);
    const float th = WrappedHeight(text, wrap, FontRole::Body);
    const float h = th + Dp(12) * 2;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h), Mix(col::bg2, accent, 0.10f), Dp(kRadiusControl + 2));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y0 + h), WithAlpha(accent, 0.35f), Dp(kRadiusControl + 2));
    {
        FontScope f(FontRole::Icon);
        dl->AddText(ImVec2(x0 + Dp(12), y0 + Dp(12) + Dp(1)), accent, icon);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + Dp(12) + icon_w + Dp(8), y0 + Dp(12)));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::Dummy(ImVec2(x1 - x0, h));
}

float SegWidth(const char* const* items, int n) {
    float w = Dp(6);
    for (int i = 0; i < n; ++i) w += ImGui::CalcTextSize(items[i], nullptr, true).x + Dp(24);
    return w;
}

// Segmented row over a value list: shows `values[i]` as items[i]; a value outside the list adds a custom item.
template <typename T>
bool SegRow(const char* label, const char* hint, const char* id, T& value, const T* values, const char* const* items,
            int n, const char* custom_fmt = nullptr) {
    const char* labels[8];
    char custom[32] = {};
    int count = n < 7 ? n : 7;
    for (int i = 0; i < count; ++i) labels[i] = items[i];
    int cur = -1;
    for (int i = 0; i < count; ++i)
        if (values[i] == value) cur = i;
    if constexpr (std::is_arithmetic_v<T>) {
        if (cur < 0 && custom_fmt) {
            std::snprintf(custom, sizeof(custom), custom_fmt, static_cast<int>(value));
            labels[count] = custom;
            cur = count++;
        }
    }
    RowBegin(label, hint, SegWidth(labels, count), Dp(kControlCompact));
    int sel = cur;
    const bool changed = SegmentedControl(id, &sel, labels, count) && sel < n;
    RowEnd();
    if (changed) value = values[sel];
    return changed;
}

bool ToggleRow(const char* label, const char* hint, const char* id, bool& v) {
    RowBegin(label, hint, Dp(32));
    bool changed = ToggleSwitch(id, &v);
    RowEnd();
    return changed;
}

void ValueRow(const char* label, const char* value, bool mono = false) {
    const float w = [&] {
        FontScope f(mono ? FontRole::Mono : FontRole::Body);
        return ImGui::CalcTextSize(value).x;
    }();
    const float max_w = (CardInnerRight() - CardInnerLeft()) * 0.6f;
    RowBegin(label, nullptr, w < max_w ? w : max_w, ImGui::GetTextLineHeight());
    FontScope f(mono ? FontRole::Mono : FontRole::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + max_w);
    ImGui::TextUnformatted(value);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    RowEnd();
}

// A spinner dot trio for "loading" states (animates; the window asks for redraws while enumeration runs).
void LoadingText(const char* text) {
    const float t = static_cast<float>(ImGui::GetTime());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight();
    for (int i = 0; i < 3; ++i) {
        const float a = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 6.0f - static_cast<float>(i) * 0.9f));
        dl->AddCircleFilled(ImVec2(p.x + Dp(4) + Dp(9) * static_cast<float>(i), p.y + fh * 0.5f), Dp(2.5f),
                            WithAlpha(col::accent, a), 12);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + Dp(32), p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

float LoadingWidth(const char* text) { return Dp(32) + ImGui::CalcTextSize(text).x; }

// Keycaps for a chord ("Ctrl" "Shift" "F9").
void DrawKeycaps(ImDrawList* dl, ImVec2 pos, float h, const std::string& label, bool dim) {
    float x = pos.x;
    size_t start = 0;
    FontScope f(FontRole::Label);
    const float fs = ImGui::GetFontSize();
    while (start <= label.size()) {
        size_t end = label.find('+', start);
        if (end == std::string::npos) end = label.size();
        const std::string part = label.substr(start, end - start);
        const float tw = ImGui::CalcTextSize(part.c_str()).x;
        const float w = tw + Dp(14) < h ? h : tw + Dp(14);
        const ImVec2 a(x, pos.y), b(x + w, pos.y + h);
        dl->AddRectFilled(ImVec2(a.x, a.y + Dp(1)), ImVec2(b.x, b.y + Dp(1)), col::bg0, Dp(5));
        dl->AddRectFilled(a, b, dim ? col::bg2 : col::bg4, Dp(5));
        dl->AddRect(a, b, col::border, Dp(5));
        dl->AddText(ImVec2(std::floor(a.x + (w - tw) * 0.5f), std::floor(a.y + (h - fs) * 0.5f)),
                    dim ? col::text_dim : col::text, part.c_str());
        x = b.x + Dp(4);
        start = end + 1;
    }
}

// The recorder field. Returns true when clicked.
bool HotkeyField(const char* id, Hotkey hk, bool capturing, bool error, float w) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float h = ImGui::GetFrameHeight() + Dp(4);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(w, h));
    const ImGuiID gid = window->GetID(id);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    ImDrawList* dl = window->DrawList;
    const float r = Dp(kRadiusControl);
    ImU32 bg = capturing ? Mix(col::bg1, col::accent, 0.12f) : hovered ? col::bg3 : col::bg1;
    dl->AddRectFilled(bb.Min, bb.Max, bg, r);
    const ImU32 border = capturing ? col::accent : error ? WithAlpha(col::danger, 0.8f) : col::border;
    dl->AddRect(bb.Min, bb.Max, border, r, capturing ? Dp(1.5f) : Dp(1));
    const float pad = Dp(5);
    if (capturing) {
        const ImGuiIO& io = ImGui::GetIO();
        std::string held_mods;
        if (io.KeyCtrl) held_mods += "Ctrl+";
        if (io.KeyAlt) held_mods += "Alt+";
        if (io.KeyShift) held_mods += "Shift+";
        if (io.KeySuper) held_mods += "Win+";
        const float fs = ImGui::GetFontSize();
        if (held_mods.empty()) {
            const float a = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
            dl->AddCircleFilled(ImVec2(bb.Min.x + Dp(12), bb.GetCenter().y), Dp(3.5f), WithAlpha(col::record, a), 12);
            dl->AddText(ImVec2(bb.Min.x + Dp(22), std::floor(bb.Min.y + (h - fs) * 0.5f)), col::accent_hover,
                        str::kSetHkPress);
        } else {
            held_mods.pop_back();
            DrawKeycaps(dl, ImVec2(bb.Min.x + pad, bb.Min.y + pad), h - pad * 2, held_mods, true);
        }
    } else if (!hk.IsSet()) {
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Min.x + Dp(10), std::floor(bb.Min.y + (h - fs) * 0.5f)), col::text_faint, str::kSetHkNone);
    } else {
        DrawKeycaps(dl, ImVec2(bb.Min.x + pad, bb.Min.y + pad), h - pad * 2, HotkeyToString(hk), false);
    }
    if (!capturing) {
        FontScope f(FontRole::Icon);
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Max.x - Dp(10) - fs, std::floor(bb.Min.y + (h - fs) * 0.5f)),
                    hovered ? col::text_dim : col::text_faint, ICON_EDIT);
    }
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        dl->AddRect(bb.Min - Dp(2, 2), bb.Max + Dp(2, 2), col::accent, r + Dp(2), Dp(2));
    return pressed;
}

// Folder picker (IFileOpenDialog, FOS_PICKFOLDERS). A shell dialog is modal by nature and runs on the UI thread.
bool PickFolder(std::string& inout) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!inout.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(Utf8ToWide(inout).c_str(), nullptr, IID_PPV_ARGS(&start))))
            dlg->SetFolder(start.Get());
    }
    HWND owner = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
    if (FAILED(dlg->Show(owner))) return false;   // cancelled
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
    inout = WideToUtf8(path);
    CoTaskMemFree(path);
    return true;
}

// Combo in the app style: no ImGui arrow box, a chevron glyph inside the frame instead.
bool BeginCombo(const char* id, const char* preview, float width, ImGuiComboFlags flags = 0) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImGui::SetNextItemWidth(width);
    const bool open = ImGui::BeginCombo(id, preview, flags | ImGuiComboFlags_NoArrowButton);
    FontScope f(FontRole::Icon);
    const float fs = ImGui::GetFontSize() * 0.75f;
    dl->AddText(ImGui::GetFont(), fs, ImVec2(pos.x + width - Dp(10) - fs, std::floor(pos.y + (h - fs) * 0.5f)),
                disabled ? col::text_faint : col::text_dim, ICON_CHEVRON_DOWN);
    return open;
}

struct SectionInfo {
    const char* icon;
    const char* label;
    const char* desc;
};
const SectionInfo kSections[kSettingsSectionCount] = {
    {ICON_RECORD, str::kSecRecording, str::kSetDescRecording},
    {ICON_MICROPHONE, str::kSecAudioSettings, str::kSetDescAudio},
    {ICON_CAMERA, str::kSecCameraSettings, str::kSetDescCamera},
    {ICON_KEYBOARD, str::kSecHotkeys, str::kSetDescHotkeys},
    {ICON_EDIT, str::kSecEditor, str::kSetDescEditor},
    {ICON_EXPORT, str::kSecExport, str::kSetDescExport},
    {ICON_LOCK, str::kSecPrivacy, str::kSetDescPrivacy},
    {ICON_INFO, str::kSecAbout, str::kSetDescAbout},
};

} // namespace

// ---- Window frame ------------------------------------------------------------------------------------------------------

const char* SettingsWindow::WindowName() { return kPopupId; }

void SettingsWindow::Draw() {
    if (!m_open) {
        // Closed from outside (Close() between frames): end the ImGui modal too, or it keeps blocking input.
        if (!m_fill_viewport && ImGui::IsPopupOpen(kPopupId)) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID id = ImGui::GetID(kPopupId);
            for (int i = 0; i < g.OpenPopupStack.Size; ++i) {
                if (g.OpenPopupStack[i].PopupId == id) {
                    ImGui::ClosePopupToLevel(i, true);
                    break;
                }
            }
        }
        return;
    }
    PollAsync();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, m_fill_viewport ? 0.0f : Dp(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, m_fill_viewport ? 0.0f : Dp(1));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ToVec4(col::bg1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg1));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ToVec4(WithAlpha(col::bg0, 0.62f)));
    bool visible = false;
    if (m_fill_viewport) {
        ImGui::SetNextWindowPos(m_has_host ? m_host_pos : vp->WorkPos);
        ImGui::SetNextWindowSize(m_has_host ? m_host_size : vp->WorkSize);
        visible = ImGui::Begin(kPopupId, nullptr, flags | ImGuiWindowFlags_NoBringToFrontOnFocus);
    } else {
        if (!ImGui::IsPopupOpen(kPopupId)) ImGui::OpenPopup(kPopupId);
        ImVec2 size = Dp(kWinW, kWinH);
        size.x = size.x < vp->WorkSize.x - Dp(16) ? size.x : vp->WorkSize.x - Dp(16);
        size.y = size.y < vp->WorkSize.y - Dp(16) ? size.y : vp->WorkSize.y - Dp(16);
        ImGui::SetNextWindowSize(size);
        ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        visible = ImGui::BeginPopupModal(kPopupId, nullptr, flags);
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    if (!visible) {
        if (m_fill_viewport) ImGui::End();
        return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Sidebar background (bg0, rounded on the left like the window).
    const float side_w = Dp(kSidebarW);
    dl->AddRectFilled(origin, ImVec2(origin.x + side_w, origin.y + avail.y), col::bg0,
                      m_fill_viewport ? 0.0f : Dp(12), ImDrawFlags_RoundLeft);
    dl->AddLine(ImVec2(origin.x + side_w, origin.y), ImVec2(origin.x + side_w, origin.y + avail.y), col::border, Dp(1));
    DrawSidebar(side_w);

    // Content.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + side_w + Dp(1), origin.y));
    const float content_w = avail.x - side_w - Dp(1);
    const float footer_h = Dp(kFooterH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(kContentPadX, kContentPadY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToVec4(col::bg1));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Dp(8));
    if (ImGui::BeginChild("##content", ImVec2(content_w, avail.y - footer_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoSavedSettings)) {
        const SectionInfo& si = kSections[static_cast<int>(m_section)];
        SectionTitle(si.label, si.desc);
        switch (m_section) {
        case SettingsSection::Recording: DrawRecording(); break;
        case SettingsSection::Audio: DrawAudio(); break;
        case SettingsSection::Camera: DrawCamera(); break;
        case SettingsSection::Hotkeys: DrawHotkeys(); break;
        case SettingsSection::Editor: DrawEditor(); break;
        case SettingsSection::Export: DrawExport(); break;
        case SettingsSection::Privacy: DrawPrivacy(); break;
        case SettingsSection::About: DrawAbout(); break;
        }
        if (m_capturing >= 0) PollCapture();
        if (m_section != SettingsSection::Audio && MeterTestActive()) StopMeterTest();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Footer.
    const float fy = origin.y + avail.y - footer_h;
    const float fx = origin.x + side_w + Dp(1);
    dl->AddLine(ImVec2(fx, fy), ImVec2(origin.x + avail.x, fy), col::border, Dp(1));
    {
        const float fs = ImGui::GetFontSize();
        const std::string err = m_saver->LastError();
        const bool failed = !err.empty();
        {
            FontScope f(FontRole::Icon);
            dl->AddText(ImVec2(fx + Dp(kContentPadX), std::floor(fy + (footer_h - ImGui::GetFontSize()) * 0.5f)),
                        failed ? col::danger : col::success, failed ? ICON_WARNING : ICON_CHECK);
        }
        dl->AddText(ImVec2(fx + Dp(kContentPadX) + Dp(24), std::floor(fy + (footer_h - fs) * 0.5f)),
                    failed ? col::danger : col::text_dim, failed ? str::kSetSaveFailed : str::kSetSavedHint);
    }
    const float done_w = Dp(96), done_h = Dp(kControlDefault);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + avail.x - Dp(kContentPadX) - done_w, fy + (footer_h - done_h) * 0.5f));
    const bool done = Button(str::kSetDone, ButtonKind::Primary, ImVec2(done_w, done_h));
    ItemTooltip(str::kSetDone, "Esc");

    // Esc closes (unless a hotkey is being recorded or a combo is open); Enter closes when nothing has keyboard focus.
    const ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool close = done;
    if (focused && m_capturing < 0 && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
        if (!io.NavVisible && ImGui::GetActiveID() == 0 &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            close = true;
    }
    if (close) {
        Close();
        if (!m_fill_viewport) ImGui::CloseCurrentPopup();
    }
    if (m_fill_viewport) ImGui::End();
    else ImGui::EndPopup();
}

void SettingsWindow::DrawSidebar(float width) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float bottom = origin.y + ImGui::GetContentRegionAvail().y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Title.
    {
        FontScope f(FontRole::Title);
        dl->AddText(ImVec2(origin.x + Dp(20), origin.y + Dp(22)), col::text, str::kSettingsTitle);
    }
    float y = origin.y + Dp(68);
    const float item_h = Dp(34);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    for (int i = 0; i < kSettingsSectionCount; ++i) {
        const SectionInfo& s = kSections[i];
        const ImRect bb(ImVec2(origin.x + Dp(10), y), ImVec2(origin.x + width - Dp(10), y + item_h));
        ImGui::PushID(i);
        const ImGuiID id = window->GetID("##sec");
        ImGui::SetCursorScreenPos(bb.Min);
        ImGui::ItemSize(bb);
        bool hovered = false, held = false;
        if (ImGui::ItemAdd(bb, id)) {
            if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
                if (m_capturing >= 0) EndCapture();
                m_section = static_cast<SettingsSection>(i);
            }
        }
        const bool sel = static_cast<int>(m_section) == i;
        if (sel) {
            dl->AddRectFilled(bb.Min, bb.Max, col::accent_soft, Dp(kRadiusControl + 1));
            dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + Dp(9)), ImVec2(bb.Min.x + Dp(3), bb.Max.y - Dp(9)), col::accent,
                              Dp(2));
        } else if (hovered) {
            dl->AddRectFilled(bb.Min, bb.Max, col::bg3, Dp(kRadiusControl + 1));
        }
        {
            // Icons sit in an 18 DIP slot; wide glyphs (the keyboard) are scaled down to fit it.
            FontScope f(FontRole::Icon);
            float fs = ImGui::GetFontSize();
            const float slot = Dp(18);
            unsigned int cp = 0;
            ImTextCharFromUtf8(&cp, s.icon, nullptr);
            const ImFontGlyph* g = ImGui::GetFontBaked()->FindGlyphNoFallback(static_cast<ImWchar>(cp));
            const float gw = g ? g->X1 - g->X0 : slot;   // drawn extent, not the (square) advance
            if (gw > slot) fs *= slot / gw;
            const float scale = gw > slot ? slot / gw : 1.0f;
            const float x0 = g ? g->X0 * scale : 0.0f;
            const float w = gw * scale;
            dl->AddText(ImGui::GetFont(), fs,
                        ImVec2(std::floor(bb.Min.x + Dp(12) + (slot - w) * 0.5f - x0),
                               std::floor(bb.Min.y + (item_h - fs) * 0.5f)),
                        sel ? col::accent_hover : col::text_dim, s.icon);
        }
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Min.x + Dp(40), std::floor(bb.Min.y + (item_h - fs) * 0.5f)),
                    sel ? col::text : hovered ? col::text : col::text_dim, s.label);
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            dl->AddRect(bb.Min - Dp(2, 2), bb.Max + Dp(2, 2), col::accent, Dp(kRadiusControl + 3), Dp(2));
        ImGui::PopID();
        y += item_h + Dp(2);
    }
    // Version at the bottom of the sidebar.
    {
        FontScope f(FontRole::Label);
        char v[64];
        std::snprintf(v, sizeof(v), "super4vid %s", CT_VERSION_STRING);
        dl->AddText(ImVec2(origin.x + Dp(22), bottom - Dp(34)), col::text_faint, v);
    }
}

void SettingsWindow::SectionTitle(const char* title, const char* desc) {
    {
        FontScope f(FontRole::Title);
        ImGui::TextUnformatted(title);
    }
    ImGui::Dummy(ImVec2(0, Dp(2)));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Dp(14)));
}

bool SettingsWindow::FolderRow(const char* label, std::string& value, const char* placeholder, const char* id) {
    RowStackBegin(label, nullptr);
    ImGui::PushID(id);
    bool changed = false;
    const float browse_w = ImGui::CalcTextSize(str::kSetBrowse).x + Dp(24);
    const float open_w = ImGui::GetFrameHeight();
    const float field_w = CardInnerRight() - CardInnerLeft() - browse_w - open_w - Dp(16);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputTextWithHint("##path", placeholder, buf, sizeof(buf))) {
        value = buf;
        changed = true;
    }
    ImGui::SameLine(0, Dp(8));
    if (ImGui::Button(str::kSetBrowse, ImVec2(browse_w, 0))) {
        std::string picked = value.empty() ? platform::Paths::DefaultRecordingsDir() : value;
        if (PickFolder(picked)) {
            value = picked;
            changed = true;
        }
    }
    ImGui::SameLine(0, Dp(8));
    if (IconButton("##open", ICON_FOLDER_OPEN, str::kSetOpen, false, nullptr, ImVec2(open_w, open_w)))
        OpenFolderAsync(value.empty() ? platform::Paths::DefaultRecordingsDir() : value);
    ImGui::PopID();
    RowStackEnd();
    return changed;
}

// ---- Sections ------------------------------------------------------------------------------------------------------------

void SettingsWindow::DrawRecording() {
    Settings::Recording r = m_settings.recording;
    bool changed = false;
    CardBegin();
    {
        static const int fps_v[] = {24, 30, 60};
        static const char* fps_l[] = {"24 fps", "30 fps", "60 fps"};
        changed |= SegRow(str::kSetFrameRate, nullptr, "##fps", r.fps, fps_v, fps_l, 3, "%d fps");
        static const std::string codec_v[] = {"h264", "hevc"};
        static const char* codec_l[] = {"H.264", "HEVC"};
        std::string codec = r.codec;
        if (SegRow(str::kSetCodec, m_hevc_available ? nullptr : str::kSetCodecHevcUnavailable, "##codec", codec, codec_v,
                   codec_l, 2) &&
            (codec != "hevc" || m_hevc_available)) {
            r.codec = codec;
            changed = true;
        }
        RowBegin(str::kSetQuality, str::kSetQualityHint, Dp(kCtrlW));
        ImGui::SetNextItemWidth(Dp(kCtrlW));
        if (ImGui::SliderInt("##quality", &r.quality, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp)) changed = true;
        RowEnd();
        static const int cd_v[] = {0, 3, 5, 10};
        static const char* cd_l[] = {"Off", "3 s", "5 s", "10 s"};
        changed |= SegRow(str::kSetCountdown, nullptr, "##countdown", r.countdown_s, cd_v, cd_l, 4, "%d s");
        static const int hz_v[] = {60, 120, 240};
        static const char* hz_l[] = {"60 Hz", "120 Hz", "240 Hz"};
        changed |= SegRow(str::kSetCursorRate, nullptr, "##cursorhz", r.cursor_hz, hz_v, hz_l, 3, "%d Hz");
        changed |= ToggleRow(str::kSetHideBar, nullptr, "##hidebar", r.hide_bar_while_recording);
    }
    CardEnd();
    CardBegin();
    changed |= FolderRow(str::kSetOutputFolder, r.output_dir, str::kSetDefaultFolder, "##outdir");
    CardEnd();
    if (changed) Apply([&r](Settings& s) { s.recording = r; });
}

void SettingsWindow::DrawAudio() {
    Settings::Recording r = m_settings.recording;
    bool changed = false;
    CardBegin();
    changed |= ToggleRow(str::kSetMic, nullptr, "##mic", r.mic);
    if (!m_devices_ready) {
        RowBegin(str::kSetMicDevice, nullptr, LoadingWidth(str::kSetLoadingDevices));
        LoadingText(str::kSetLoadingDevices);
        RowEnd();
    } else {
        RowBegin(str::kSetMicDevice, m_mics.empty() ? str::kSetNoMics : nullptr, Dp(kCtrlW + 40));
        ImGui::BeginDisabled(!r.mic || m_mics.empty());
        std::string preview = str::kSetDefaultDevice;
        for (const Mic& m : m_mics)
            if (m.id == r.mic_device) preview = m.name;
        if (BeginCombo("##micdev", preview.c_str(), Dp(kCtrlW + 40))) {
            if (ImGui::Selectable(str::kSetDefaultDevice, r.mic_device.empty())) {
                r.mic_device.clear();
                changed = true;
            }
            for (const Mic& m : m_mics) {
                ImGui::PushID(m.id.c_str());
                if (ImGui::Selectable(m.name.c_str(), m.id == r.mic_device)) {
                    r.mic_device = m.id;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        RowEnd();
    }
    {
        // Test microphone: live meter (docs/phases/P11 11.6 "meters test"). Restarted when the device changes.
        const bool running = MeterTestActive();
        if (running && m_meter->GetSource() == LevelMonitor::Source::Microphone && m_meter_device != r.mic_device) StartMeterTest();
        const LevelMonitor::Status ms = m_meter ? m_meter->GetStatus() : LevelMonitor::Status::Stopped;
        const char* hint = ms == LevelMonitor::Status::Failed     ? str::kSetMicTestFailed
                           : ms == LevelMonitor::Status::Starting ? str::kSetMicTestStarting
                                                                  : str::kSetMicTestHint;
        const float meter_w = Dp(160), btn_w = Dp(72);
        RowBegin(str::kSetMicTest, hint, meter_w + Dp(8) + btn_w, 0,
                 ms == LevelMonitor::Status::Failed ? col::danger : col::text_dim);
        const double now = ImGui::GetTime();
        const float dt = static_cast<float>(now - m_meter_t);
        m_meter_t = now;
        const float shown = m_meter ? m_meter->Shown(dt) : -120.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - Dp(8)) * 0.5f));
        MeterBar("##mictest_meter", shown, ImVec2(meter_w, Dp(8)));
        ImGui::SetCursorScreenPos(ImVec2(p.x + meter_w + Dp(8), p.y));
        ImGui::BeginDisabled(!r.mic && !running);
        if (Button(running ? str::kSetMicTestStop : str::kSetMicTestStart, ButtonKind::Normal, ImVec2(btn_w, 0))) {
            if (running) StopMeterTest();
            else StartMeterTest();
        }
        ImGui::EndDisabled();
        RowEnd();
    }
    changed |= ToggleRow(str::kSetSystemAudio, str::kSetSystemAudioHint, "##sysaudio", r.system_audio);
    CardEnd();
    if (IconButton("##refresh_audio", ICON_REFRESH, nullptr, false, str::kSetRefresh) && !AsyncBusy()) {
        m_devices_ready = false;
        StartEnumeration(true);
    }
    if (changed) Apply([&r](Settings& s) { s.recording = r; });
}

void SettingsWindow::DrawCamera() {
    Settings::Recording r = m_settings.recording;
    bool changed = false;
    const Cam* selected = nullptr;
    for (const Cam& c : m_cams)
        if (c.link == r.webcam_device) selected = &c;
    CardBegin();
    if (!m_cams_ready) {
        RowBegin(str::kSetWebcam, nullptr, LoadingWidth(str::kSetLoadingDevices));
        LoadingText(str::kSetLoadingDevices);
        RowEnd();
    } else {
        RowBegin(str::kSetWebcam, m_cams.empty() ? str::kSetNoCameras : nullptr, Dp(kCtrlW + 40));
        const char* preview = selected ? selected->name.c_str() : str::kSetNoWebcam;
        if (BeginCombo("##cam", preview, Dp(kCtrlW + 40))) {
            if (ImGui::Selectable(str::kSetNoWebcam, selected == nullptr)) {
                r.webcam_device.clear();
                changed = true;
            }
            for (const Cam& c : m_cams) {
                ImGui::PushID(c.link.c_str());
                if (ImGui::Selectable(c.name.c_str(), &c == selected)) {
                    r.webcam_device = c.link;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        RowEnd();
    }
    {
        static const char* const kCommon[] = {"1920x1080", "1280x720", "640x480"};
        std::vector<std::string> sizes;
        if (selected && !selected->sizes.empty()) sizes = selected->sizes;
        else sizes.assign(std::begin(kCommon), std::end(kCommon));
        RowBegin(str::kSetResolution, nullptr, Dp(kCtrlW));
        ImGui::BeginDisabled(selected == nullptr);
        if (BeginCombo("##camres", r.webcam_resolution.c_str(), Dp(kCtrlW))) {
            for (const std::string& s : sizes) {
                if (ImGui::Selectable(s.c_str(), s == r.webcam_resolution)) {
                    r.webcam_resolution = s;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        RowEnd();
    }
    CardEnd();
    if (IconButton("##refresh_cam", ICON_REFRESH, nullptr, false, str::kSetRefresh) && !AsyncBusy()) {
        m_cams_ready = false;
        StartEnumeration(true);
    }
    if (changed) Apply([&r](Settings& s) { s.recording = r; });
}

void SettingsWindow::DrawHotkeys() {
    CardBegin();
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const auto a = static_cast<HotkeyAction>(i);
        const bool capturing = m_capturing == i;
        const char* hint = !m_hotkey_error[i].empty() ? m_hotkey_error[i].c_str()
                           : capturing                 ? m_capture_hint.c_str()
                                                       : nullptr;
        const bool warn = !m_hotkey_error[i].empty() || (capturing && m_capture_hint != str::kSetHkCaptureHint);
        RowBegin(HotkeyActionLabel(a), hint, Dp(220), ImGui::GetFrameHeight() + Dp(4),
                 warn ? col::danger : capturing ? col::accent_hover : col::text_dim);
        ImGui::PushID(i);
        if (HotkeyField("##hk", HotkeyFor(m_settings.hotkeys, a), capturing, !m_hotkey_error[i].empty(), Dp(220))) {
            if (capturing) EndCapture();
            else {
                if (m_capturing >= 0) EndCapture();
                BeginCapture(a);
            }
        }
        ImGui::PopID();
        RowEnd();
    }
    CardEnd();
    if (IconButton("##hkreset", ICON_UNDO, nullptr, false, str::kSetHkReset)) {
        if (m_capturing >= 0) EndCapture();
        if (suspend_hotkeys) suspend_hotkeys();
        Apply([](Settings& s) { s.hotkeys = Settings::Defaults().hotkeys; });
        for (std::string& e : m_hotkey_error) e.clear();
        std::string err;
        if (reregister_hotkeys && !reregister_hotkeys(m_settings.hotkeys, &err)) m_hotkey_error[0] = err;
    }
    // A click anywhere else stops listening.
    if (m_capturing >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) EndCapture();
}

void SettingsWindow::PollCapture() {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID owner = window->GetID("##hk_capture");
    // Claim every key so ImGui navigation (Tab, Esc, arrows, Enter) and app shortcuts do not see them.
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (ImGui::IsMouseKey(key) || ImGui::IsGamepadKey(key) || ImGui::IsLRModKey(key) || ImGui::IsAliasKey(key)) continue;
        ImGui::SetKeyOwner(key, owner, ImGuiInputFlags_LockThisFrame);
    }
    const ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (ImGui::IsMouseKey(key) || ImGui::IsGamepadKey(key) || ImGui::IsLRModKey(key) || ImGui::IsAliasKey(key)) continue;
        if (!ImGui::IsKeyPressed(key, ImGuiInputFlags_None, owner)) continue;
        const unsigned vk = VkFromImGuiKey(key);
        if (vk == 0) continue;
        unsigned mods = 0;
        if (io.KeyCtrl) mods |= MOD_CONTROL;
        if (io.KeyAlt) mods |= MOD_ALT;
        if (io.KeyShift) mods |= MOD_SHIFT;
        if (io.KeySuper) mods |= MOD_WIN;
        Hotkey hk;
        const int idx = m_capturing;
        const auto action = static_cast<HotkeyAction>(idx);
        switch (CaptureChord(vk, mods, &hk)) {
        case ChordResult::Pending: break;
        case ChordResult::Cancelled: EndCapture(); break;
        case ChordResult::NeedsModifier: m_capture_hint = str::kSetHkNeedsModifier; break;
        case ChordResult::Cleared:
            Apply([action](Settings& s) { HotkeyFor(s.hotkeys, action) = Hotkey{}; });
            EndCapture();
            break;
        case ChordResult::Captured: {
            std::string err;
            if (AssignHotkey(action, hk, &err)) {
                EndCapture();
            } else {
                // Keep listening so the user can try another chord right away.
                m_capture_hint = err;
            }
            break;
        }
        }
        break;
    }
}

void SettingsWindow::DrawEditor() {
    Settings::Editor e = m_settings.editor;
    bool changed = false;
    CardBegin();
    {
        static const int as_v[] = {0, 30, 60, 300};
        static const char* as_l[] = {"Off", "30 s", "1 min", "5 min"};
        changed |= SegRow(str::kSetAutosave, nullptr, "##autosave", e.autosave_s, as_v, as_l, 4, "%d s");
        static const int fc_v[] = {128, 256, 512, 1024, 2048};
        static const char* fc_l[] = {"128 MB", "256 MB", "512 MB", "1 GB", "2 GB"};
        changed |= SegRow(str::kSetFrameCache, str::kSetFrameCacheHint, "##framecache", e.frame_cache_mb, fc_v, fc_l, 5,
                          "%d MB");
        static const std::string pq_v[] = {"full", "half"};
        static const char* pq_l[] = {str::kSetFull, str::kSetHalf};
        changed |= SegRow(str::kSetPreviewQuality, nullptr, "##previewq", e.preview_quality, pq_v, pq_l, 2);
    }
    CardEnd();
    CardBegin();
    changed |= ToggleRow(str::kSetSnap, nullptr, "##snap", e.snap);
    changed |= ToggleRow(str::kSetRipple, nullptr, "##ripple", e.ripple);
    CardEnd();
    if (changed) Apply([&e](Settings& s) { s.editor = e; });
}

void SettingsWindow::DrawExport() {
    Settings::Export x = m_settings.export_;
    bool changed = false;
    CardBegin();
    RowBegin(str::kSetDefaultPreset, nullptr, Dp(kCtrlW + 40));
    std::string preview = x.last_preset;
    for (const PresetItem& p : m_presets)
        if (p.id == x.last_preset) preview = p.name;
    if (BeginCombo("##preset", preview.c_str(), Dp(kCtrlW + 40), ImGuiComboFlags_HeightLarge)) {
        bool user_header = false;
        for (const PresetItem& p : m_presets) {
            if (p.user && !user_header) {
                user_header = true;
                ImGui::Separator();
                ImGui::TextDisabled("%s", str::kSetUserPresets);
            }
            ImGui::PushID(p.id.c_str());
            ImGui::PushID(p.user ? 1 : 0);
            if (ImGui::Selectable(p.name.c_str(), p.id == x.last_preset)) {
                x.last_preset = p.id;
                changed = true;
            }
            ImGui::PopID();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    RowEnd();
    CardEnd();
    CardBegin();
    changed |= FolderRow(str::kSetExportFolder, x.last_dir, str::kSetExportFolderDefault, "##exportdir");
    CardEnd();
    if (changed) Apply([&x](Settings& s) { s.export_ = x; });
}

void SettingsWindow::DrawPrivacy() {
    Settings::Recording r = m_settings.recording;
    Settings::Privacy p = m_settings.privacy;
    bool changed_r = false, changed_p = false;
    CardBegin();
    {
        static const std::string ks_v[] = {"off", "shortcuts", "all"};
        static const char* ks_l[] = {str::kSetKsOff, str::kSetKsShortcuts, str::kSetKsAll};
        changed_r |= SegRow(str::kSetKeystrokes, nullptr, "##keystrokes", r.keystrokes, ks_v, ks_l, 3);
        const bool all = r.keystrokes == "all";
        const char* note = r.keystrokes == "off" ? str::kSetKsNoteOff : all ? str::kSetKsNoteAll : str::kSetKsNoteShortcuts;
        Note(all ? ICON_WARNING : ICON_LOCK, note, all ? col::warn : col::accent);
        ImGui::Dummy(ImVec2(0, Dp(kCardPad)));
    }
    CardEnd();
    CardBegin();
    changed_p |= ToggleRow(str::kSetCrashDumps, str::kSetCrashDumpsHint, "##crashdumps", p.crash_dumps);
    {
        const std::string& dir = platform::Paths::SettingsDir();
        const float bw = ImGui::CalcTextSize(str::kSetOpenDataFolder).x + Dp(44);
        RowBegin(str::kSetDataFolder, dir.c_str(), bw);
        if (IconButton("##opendata", ICON_FOLDER_OPEN, nullptr, false, str::kSetOpenDataFolder)) OpenFolderAsync(dir);
        RowEnd();
    }
    CardEnd();
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(str::kSetLocalOnly);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (changed_r) Apply([&r](Settings& s) { s.recording.keystrokes = r.keystrokes; });
    if (changed_p) Apply([&p](Settings& s) { s.privacy = p; });
}

void SettingsWindow::DrawAbout() {
    // Identity card: logo mark, name, version line.
    CardBegin();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x0 = CardInnerLeft();
        const float y0 = ImGui::GetCursorScreenPos().y;
        const float h = Dp(76);
        const ImVec2 c(x0 + Dp(24), y0 + h * 0.5f);
        dl->AddCircleFilled(c, Dp(22), Mix(col::bg2, col::record, 0.18f), 40);
        dl->AddCircle(c, Dp(22), WithAlpha(col::record, 0.55f), 40, Dp(1.5f));
        dl->AddCircleFilled(c, Dp(9), col::record, 32);
        {
            FontScope f(FontRole::Title);
            dl->AddText(ImVec2(x0 + Dp(60), y0 + Dp(16)), col::text, str::kAppName);
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%s %s  \xC2\xB7  %s %s  \xC2\xB7  %s %s", str::kSetVersion, CT_VERSION_STRING,
                      str::kSetCommit, CT_GIT_HASH, str::kSetBuilt, CT_BUILD_DATE);
        dl->AddText(ImVec2(x0 + Dp(60), y0 + Dp(44)), col::text_dim, line);
        ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, y0));
        ImGui::Dummy(ImVec2(g_card.width, h));
        g_card.rows = 1;
    }
    ValueRow(str::kSetGpu, m_gpu_name.empty() ? "-" : m_gpu_name.c_str());
    CardEnd();

    CardBegin(str::kSetEncoders);
    {
        const float x0 = CardInnerLeft();
        const float y0 = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + Dp(14)));
        if (!m_encoders_ready) {
            LoadingText(str::kSetLoadingEncoders);
        } else {
            FontScope f(FontRole::Mono);
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + CardInnerRight() - x0);
            ImGui::TextUnformatted(m_encoder_report.empty() ? "-" : m_encoder_report.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        const float bottom = ImGui::GetItemRectMax().y + Dp(14);
        ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, y0));
        ImGui::Dummy(ImVec2(g_card.width, bottom - y0));
    }
    CardEnd();

    if (IconButton("##copydiag", ICON_COPY, nullptr, false, str::kSetCopyDiagnostics)) {
        const std::string d = BuildDiagnostics(m_gpu_name, m_encoder_report);
        ImGui::SetClipboardText(d.c_str());
        m_copied_at = ImGui::GetTime();
    }
    ImGui::SameLine(0, Dp(8));
    if (IconButton("##openlogs", ICON_FOLDER_OPEN, nullptr, false, str::kErrOpenLogFolder))
        OpenFolderAsync(platform::Paths::LogsDir());
    if (ImGui::GetTime() - m_copied_at < 2.5) {
        ImGui::SameLine(0, Dp(12));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::success));
        ImGui::TextUnformatted(str::kSetCopied);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, Dp(20)));

    CardBegin(str::kSetLicenses);
    {
        const float x0 = CardInnerLeft();
        const float y0 = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(x0, y0 + Dp(14)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + CardInnerRight() - x0);
        ImGui::TextUnformatted(str::kSetLicenseImGui);
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + Dp(6)));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextUnformatted(str::kSetLicenseSystem);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const float bottom = ImGui::GetItemRectMax().y + Dp(14);
        ImGui::SetCursorScreenPos(ImVec2(g_card.start.x, y0));
        ImGui::Dummy(ImVec2(g_card.width, bottom - y0));
    }
    CardEnd();
}

} // namespace ct::ui
