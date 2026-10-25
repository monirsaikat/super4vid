// Shared custom widgets (docs/05 sections 1, 2, 4).
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ct::ui {

using namespace metrics;

void ItemTooltip(const char* name, const char* shortcut) {
    if (!name || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    if (!ImGui::BeginTooltip()) return;
    ImGui::TextUnformatted(name);
    if (shortcut && *shortcut) {
        ImGui::SameLine(0, Dp(12));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextUnformatted(shortcut);
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
}

static void DrawFocusRing(ImVec2 a, ImVec2 b, float rounding) {
    // docs/05: 2 px accent outside the control, radius + 2.
    ImGui::GetWindowDrawList()->AddRect(a - Dp(2, 2), b + Dp(2, 2), col::accent, rounding + Dp(2), Dp(2));
}

bool IconButton(const char* id, const char* icon, const char* tooltip, bool active, const char* label, ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const float h = size.y > 0 ? size.y : Dp(kControlCompact);
    float icon_w = 0, label_w = 0;
    {
        FontScope f(FontRole::Icon);
        icon_w = ImGui::CalcTextSize(icon).x;
    }
    if (label && *label) label_w = ImGui::CalcTextSize(label, nullptr, true).x;
    float w = size.x;
    if (w <= 0) w = label_w > 0 ? Dp(10) + icon_w + Dp(6) + label_w + Dp(10) : h;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(w, h));
    const ImGuiID gid = window->GetID(id);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    ImDrawList* dl = window->DrawList;
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImU32 bg = 0;
    if (held) bg = col::bg4;
    else if (active) bg = col::accent_soft;
    else if (hovered) bg = col::bg3;
    if (bg) dl->AddRectFilled(bb.Min, bb.Max, bg, Dp(kRadiusControl));
    const ImU32 fg = disabled ? col::text_faint : active ? col::accent_hover : hovered ? col::text : col::text_dim;
    {
        FontScope f(FontRole::Icon);
        const float fs = ImGui::GetFontSize();
        const float x = label_w > 0 ? bb.Min.x + Dp(10) : bb.Min.x + (w - icon_w) * 0.5f;
        dl->AddText(ImVec2(std::floor(x), std::floor(bb.Min.y + (h - fs) * 0.5f)), fg, icon);
    }
    if (label_w > 0) {
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Min.x + Dp(10) + icon_w + Dp(6), std::floor(bb.Min.y + (h - fs) * 0.5f)),
                    disabled ? col::text_faint : col::text, label, ImGui::FindRenderedTextEnd(label));
    }
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) DrawFocusRing(bb.Min, bb.Max, Dp(kRadiusControl));
    if (tooltip) ItemTooltip(tooltip);
    return pressed;
}

bool Button(const char* label, ButtonKind kind, ImVec2 size) {
    ImU32 base = col::bg2, hov = col::bg3, act = col::bg4, txt = col::text;
    switch (kind) {
    case ButtonKind::Primary: base = col::accent; hov = col::accent_hover; act = Mix(col::accent, col::bg0, 0.2f); txt = Rgb(0xFFFFFF); break;
    case ButtonKind::Danger: base = WithAlpha(col::danger, 0.16f); hov = WithAlpha(col::danger, 0.28f); act = WithAlpha(col::danger, 0.4f); txt = col::danger; break;
    case ButtonKind::Record: base = WithAlpha(col::record, 0.14f); hov = WithAlpha(col::record, 0.24f); act = WithAlpha(col::record, 0.34f); txt = Rgb(0xFF8A8B); break;
    default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(hov));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(act));
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(txt));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool SegmentedControl(const char* id, int* current, const char* const* items, int count, float item_width) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems || count <= 0) return false;
    ImGui::PushID(id);
    const float h = Dp(kControlCompact);
    const float pad = Dp(3);
    float widths[16];
    count = count > 16 ? 16 : count;
    float total = pad * 2;
    for (int i = 0; i < count; ++i) {
        widths[i] = item_width > 0 ? item_width : ImGui::CalcTextSize(items[i], nullptr, true).x + Dp(24);
        total += widths[i];
    }
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(total, h));
    ImGui::ItemSize(bb);
    ImDrawList* dl = window->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, col::bg0, Dp(kRadiusControl + 2));
    dl->AddRect(bb.Min, bb.Max, col::border, Dp(kRadiusControl + 2));
    bool changed = false;
    float x = bb.Min.x + pad;
    for (int i = 0; i < count; ++i) {
        const ImRect seg(ImVec2(x, bb.Min.y + pad), ImVec2(x + widths[i], bb.Max.y - pad));
        const ImGuiID sid = window->GetID(i);
        if (ImGui::ItemAdd(seg, sid)) {
            bool hovered = false, held = false;
            if (ImGui::ButtonBehavior(seg, sid, &hovered, &held) && *current != i) {
                *current = i;
                changed = true;
            }
            const bool sel = *current == i;
            if (sel) dl->AddRectFilled(seg.Min, seg.Max, col::bg3, Dp(kRadiusControl));
            else if (hovered) dl->AddRectFilled(seg.Min, seg.Max, WithAlpha(col::bg3, 0.5f), Dp(kRadiusControl));
            const ImVec2 ts = ImGui::CalcTextSize(items[i], nullptr, true);
            dl->AddText(ImVec2(std::floor(seg.Min.x + (widths[i] - ts.x) * 0.5f), std::floor(seg.Min.y + (seg.GetHeight() - ts.y) * 0.5f)),
                        sel ? col::text : hovered ? col::text : col::text_dim, items[i], ImGui::FindRenderedTextEnd(items[i]));
        }
        x += widths[i];
    }
    ImGui::PopID();
    return changed;
}

bool ToggleSwitch(const char* id, bool* v) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const float h = Dp(18), w = Dp(32);
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(w, frame_h));
    const ImGuiID gid = window->GetID(id);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if (pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(gid);
    }
    const ImVec2 a(bb.Min.x, bb.Min.y + (frame_h - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = window->DrawList;
    const ImU32 track = *v ? (hovered ? col::accent_hover : col::accent) : (hovered ? col::bg4 : col::bg3);
    dl->AddRectFilled(a, b, track, h * 0.5f);
    const float r = h * 0.5f - Dp(3);
    const float cx = *v ? b.x - h * 0.5f : a.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, a.y + h * 0.5f), r, *v ? Rgb(0xFFFFFF) : col::text_dim, 20);
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) DrawFocusRing(a, b, h * 0.5f);
    return pressed;
}

ImU32 MeterColor(float level_db) {
    if (level_db < -18) return col::success;
    if (level_db < -6) return col::warn;
    return col::danger;
}

void MeterBar(const char* id, float level_db, ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + size);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, window->GetID(id))) return;
    ImDrawList* dl = window->DrawList;
    // Three segments: [-60,-18) green, [-18,-6) amber, [-6,0] red; each lights proportionally.
    const float bounds[4] = {-60, -18, -6, 0};
    const ImU32 colors[3] = {col::success, col::warn, col::danger};
    const float gap = Dp(2);
    const float seg_w = (size.x - gap * 2) / 3;
    for (int i = 0; i < 3; ++i) {
        const float x0 = bb.Min.x + i * (seg_w + gap);
        const ImVec2 a(x0, bb.Min.y), b(x0 + seg_w, bb.Max.y);
        dl->AddRectFilled(a, b, col::bg3, Dp(2));
        float fill = (level_db - bounds[i]) / (bounds[i + 1] - bounds[i]);
        fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
        if (fill > 0) dl->AddRectFilled(a, ImVec2(x0 + seg_w * fill, b.y), colors[i], Dp(2));
    }
}

ImVec2 FitViewport(ImVec2 size, float margin) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return size;
    const float max_x = vp->WorkSize.x - Dp(margin) * 2, max_y = vp->WorkSize.y - Dp(margin) * 2;
    if (size.x > 0 && max_x > 0 && size.x > max_x) size.x = max_x;
    if (size.y > 0 && max_y > 0 && size.y > max_y) size.y = max_y;
    return size;
}

bool BeginPopover(const char* id) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(id) + 0x5EED;
    const bool open_now = ImGui::IsPopupOpen(id);
    if (!open_now) {
        st->SetFloat(key, -1.0f);
        return false;
    }
    float opened_at = st->GetFloat(key, -1.0f);
    const float now = static_cast<float>(ImGui::GetTime());
    if (opened_at < 0) {
        opened_at = now;
        st->SetFloat(key, now);
    }
    const float t = EaseOut((now - opened_at) * 1000.0f / kMotionMs);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(kRadiusCard));
    const bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(3);
    return open;
}

void EndPopover() { ImGui::EndPopup(); }

bool SectionHeader(const char* label, bool default_open, const char* right_text) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImGuiID gid = window->GetID(label);
    ImGuiStorage* st = ImGui::GetStateStorage();
    bool open = st->GetBool(gid, default_open);
    const float h = Dp(30);
    const ImVec2 pos = window->DC.CursorPos;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImRect bb(pos, pos + ImVec2(w, h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return open;
    bool hovered = false, held = false;
    if (ImGui::ButtonBehavior(bb, gid, &hovered, &held)) {
        open = !open;
        st->SetBool(gid, open);
    }
    ImDrawList* dl = window->DrawList;
    if (hovered) dl->AddRectFilled(bb.Min, bb.Max, WithAlpha(col::bg3, 0.6f), Dp(kRadiusControl));
    {
        FontScope f(FontRole::Label);
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Min.x + Dp(4), std::floor(bb.Min.y + (h - fs) * 0.5f + Dp(1))), col::text_faint,
                    open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT);
    }
    {
        FontScope f(FontRole::Strong);
        const float fs = ImGui::GetFontSize();
        dl->AddText(ImVec2(bb.Min.x + Dp(22), std::floor(bb.Min.y + (h - fs) * 0.5f)), col::text, label,
                    ImGui::FindRenderedTextEnd(label));
    }
    if (right_text && *right_text) {
        FontScope f(FontRole::Label);
        const ImVec2 ts = ImGui::CalcTextSize(right_text);
        dl->AddText(ImVec2(bb.Max.x - ts.x - Dp(6), std::floor(bb.Min.y + (h - ts.y) * 0.5f)), col::text_dim, right_text);
    }
    return open;
}

// --- Property rows ---------------------------------------------------------------------------------------------

float PropRowBegin(const char* label) {
    ImGui::AlignTextToFramePadding();
    const float start = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopStyleColor();
    ImGui::SameLine(start + Dp(kPropLabelWidth));
    return ImGui::GetContentRegionAvail().x;
}

static const char* UnitFormat(char* buf, size_t n, const char* fmt, const char* unit) {
    if (!unit || !*unit) return fmt;
    size_t o = static_cast<size_t>(std::snprintf(buf, n, "%s ", fmt));
    for (const char* u = unit; *u && o + 2 < n; ++u) {
        if (*u == '%') buf[o++] = '%';
        buf[o++] = *u;
    }
    buf[o < n ? o : n - 1] = 0;
    return buf;
}

static bool KeyframeToggle(KeyframeHook* kf, float value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float s = ImGui::GetFrameHeight();
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(Dp(18), s));
    const ImGuiID gid = window->GetID("##kf");
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    const bool has = kf->HasKey();
    const bool animated = kf->Animated();
    const ImVec2 c = bb.GetCenter();
    const float r = Dp(4.5f);
    const ImVec2 pts[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    ImDrawList* dl = window->DrawList;
    const ImU32 color = has ? col::accent : animated ? col::accent_hover : hovered ? col::text : col::text_faint;
    if (has) dl->AddConvexPolyFilled(pts, 4, color);
    else dl->AddPolyline(pts, 4, color, Dp(1.2f), ImDrawFlags_Closed);
    if (hovered) ItemTooltip(has ? "Remove keyframe at playhead" : "Add keyframe at playhead");
    if (pressed) kf->Toggle(value);
    return pressed;
}

bool PropFloat(const char* label, float& v, float min, float max, const char* fmt, const char* unit, KeyframeHook* kf,
               float reset, float speed) {
    ImGui::PushID(label);
    float w = PropRowBegin(label);
    if (kf) {
        KeyframeToggle(kf, v);
        ImGui::SameLine(0, Dp(4));
        w -= Dp(22);
    }
    char fbuf[48];
    const char* f = UnitFormat(fbuf, sizeof(fbuf), fmt ? fmt : "%.2f", unit);
    if (speed <= 0) speed = (max > min && max - min < 1e6f) ? (max - min) / 400.0f : 0.01f;
    ImGui::SetNextItemWidth(w);
    bool changed = ImGui::DragFloat("##v", &v, speed, min, max, f, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt) {
        v = reset;
        changed = true;
        ImGui::ClearActiveID();
    }
    ImGui::PopID();
    return changed;
}

bool PropInt(const char* label, int& v, int min, int max, const char* unit, int reset) {
    ImGui::PushID(label);
    const float w = PropRowBegin(label);
    char fbuf[32];
    const char* f = UnitFormat(fbuf, sizeof(fbuf), "%d", unit);
    ImGui::SetNextItemWidth(w);
    bool changed = ImGui::DragInt("##v", &v, 0.25f, min, max, f, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt) {
        v = reset;
        changed = true;
        ImGui::ClearActiveID();
    }
    ImGui::PopID();
    return changed;
}

bool PropColor(const char* label, ColorF& c, bool alpha) {
    ImGui::PushID(label);
    PropRowBegin(label);
    float v[4] = {c.r, c.g, c.b, c.a};
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex;
    if (!alpha) flags |= ImGuiColorEditFlags_NoAlpha;
    else flags |= ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = alpha ? ImGui::ColorEdit4("##c", v, flags) : ImGui::ColorEdit3("##c", v, flags);
    if (changed) c = ColorF{v[0], v[1], v[2], alpha ? v[3] : c.a};
    ImGui::PopID();
    return changed;
}

bool PropEnum(const char* label, int& v, const char* const* items, int count) {
    ImGui::PushID(label);
    const float w = PropRowBegin(label);
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##e", v >= 0 && v < count ? items[v] : "")) {
        for (int i = 0; i < count; ++i) {
            if (ImGui::Selectable(items[i], i == v) && i != v) {
                v = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

bool PropToggle(const char* label, bool& v) {
    ImGui::PushID(label);
    PropRowBegin(label);
    const bool changed = ToggleSwitch("##t", &v);
    ImGui::PopID();
    return changed;
}

bool PropTime(const char* label, TimeUs& v, TimeUs min, TimeUs max) {
    ImGui::PushID(label);
    const float w = PropRowBegin(label);
    double s = static_cast<double>(v) / 1e6;
    const double smin = static_cast<double>(min) / 1e6, smax = static_cast<double>(max) / 1e6;
    ImGui::SetNextItemWidth(w);
    const bool changed = ImGui::DragScalar("##t", ImGuiDataType_Double, &s, 0.01f, &smin, &smax, "%.3f s",
                                           ImGuiSliderFlags_AlwaysClamp);
    if (changed) v = static_cast<TimeUs>(std::llround(s * 1e6));
    ImGui::PopID();
    return changed;
}

static int ResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(data->UserData);
        s->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = s->data();
    }
    return 0;
}

bool PropText(const char* label, std::string& s, bool multiline) {
    ImGui::PushID(label);
    const float w = PropRowBegin(label);
    bool changed;
    if (multiline)
        changed = ImGui::InputTextMultiline("##s", s.data(), s.capacity() + 1, ImVec2(w, Dp(64)),
                                            ImGuiInputTextFlags_CallbackResize, ResizeCallback, &s);
    else {
        ImGui::SetNextItemWidth(w);
        changed = ImGui::InputText("##s", s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallback, &s);
    }
    ImGui::PopID();
    return changed;
}

void PropValue(const char* label, const char* value) {
    PropRowBegin(label);
    ImGui::TextUnformatted(value);
}

// --- Toasts ----------------------------------------------------------------------------------------------------

int ToastStack::Push(std::string text, ToastKind kind, std::vector<ToastButton> buttons, float seconds) {
    Toast t{m_next_id++, std::move(text), kind, std::move(buttons), seconds, ImGui::GetTime(), 0.0, -1.0, -1.0};
    m_toasts.push_back(std::move(t));
    return m_toasts.back().id;
}

void ToastStack::Dismiss(int id) {
    for (Toast& t : m_toasts)
        if (t.id == id && t.closing_at < 0) t.closing_at = ImGui::GetTime();
}

void ToastStack::Draw(float bottom_inset) {
    if (m_toasts.empty()) return;
    const double now = ImGui::GetTime();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = Dp(340);
    float y = vp->WorkPos.y + vp->WorkSize.y - Dp(16) - Dp(bottom_inset);
    const float motion = kMotionMs / 1000.0f;
    for (int i = static_cast<int>(m_toasts.size()) - 1; i >= 0; --i) {
        Toast& t = m_toasts[static_cast<size_t>(i)];
        const double paused = t.paused_total + (t.hover_since >= 0 ? now - t.hover_since : 0.0);
        if (t.closing_at < 0 && now - t.shown_at - paused > t.seconds) t.closing_at = now;
        if (t.closing_at >= 0 && now - t.closing_at > motion) {
            m_toasts.erase(m_toasts.begin() + i);
            continue;
        }
        float a = EaseOut(static_cast<float>((now - t.shown_at) / motion));
        if (t.closing_at >= 0) a = 1.0f - EaseOut(static_cast<float>((now - t.closing_at) / motion));
        char name[32];
        std::snprintf(name, sizeof(name), "##toast%d", t.id);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - Dp(16) + (1 - a) * Dp(24), y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusCard));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(14, 12));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg2));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_AlwaysAutoResize;
        bool dismiss = false;
        if (ImGui::Begin(name, nullptr, flags)) {
            const char* icon = ICON_INFO;
            ImU32 icol = col::accent;
            switch (t.kind) {
            case ToastKind::Success: icon = ICON_CHECK; icol = col::success; break;
            case ToastKind::Warning: icon = ICON_WARNING; icol = col::warn; break;
            case ToastKind::Error: icon = ICON_ERROR; icol = col::danger; break;
            default: break;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            dl->AddRectFilled(wp + Dp(0, 10), ImVec2(wp.x + Dp(3), wp.y + ImGui::GetWindowHeight() - Dp(10)), icol, Dp(2));
            ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(icol));
            {
                FontScope f(FontRole::Icon);
                ImGui::TextUnformatted(icon);
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(0, Dp(10));
            ImGui::PushTextWrapPos(width - Dp(44));
            ImGui::TextUnformatted(t.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine(width - Dp(40));
            ImGui::PushID(t.id);
            if (IconButton("x", ICON_CLOSE, nullptr, false, nullptr, Dp(22, 22))) dismiss = true;
            if (!t.buttons.empty()) {
                ImGui::Dummy(Dp(0, 2));
                ImGui::Indent(Dp(26));
                for (size_t b = 0; b < t.buttons.size(); ++b) {
                    if (b) ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(b));
                    if (Button(t.buttons[b].label.c_str(), b == 0 ? ButtonKind::Primary : ButtonKind::Normal)) {
                        if (t.buttons[b].on_click) t.buttons[b].on_click();
                        dismiss = true;
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent(Dp(26));
            }
            ImGui::PopID();
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            if (hovered && t.hover_since < 0) t.hover_since = now;
            if (!hovered && t.hover_since >= 0) {
                t.paused_total += now - t.hover_since;
                t.hover_since = -1;
            }
            y -= ImGui::GetWindowHeight() + Dp(8);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        if (dismiss && t.closing_at < 0) t.closing_at = now;
    }
}

const char* FormatDuration(char* buf, size_t n, TimeUs t) {
    const bool neg = t < 0;
    if (neg) t = -t;
    const long long cs = static_cast<long long>(t / 10'000);
    const long long s = cs / 100, m = s / 60, h = m / 60;
    if (h > 0) std::snprintf(buf, n, "%s%lld:%02lld:%02lld", neg ? "-" : "", h, m % 60, s % 60);
    else std::snprintf(buf, n, "%s%lld:%02lld.%02lld", neg ? "-" : "", m, s % 60, cs % 100);
    return buf;
}

const char* FormatTimecode(char* buf, size_t n, TimeUs t, int fps_num, int fps_den) {
    if (t < 0) t = 0;
    if (fps_num <= 0 || fps_den <= 0) {
        fps_num = 30;
        fps_den = 1;
    }
    const long long s = static_cast<long long>(t / kUsPerSecond);
    const TimeUs rem = t - static_cast<TimeUs>(s) * kUsPerSecond;
    const long long frame = static_cast<long long>((rem * fps_num) / (static_cast<long long>(fps_den) * kUsPerSecond));
    std::snprintf(buf, n, "%02lld:%02lld:%02lld.%02lld", s / 3600, (s / 60) % 60, s % 60, frame);
    return buf;
}

} // namespace ct::ui
