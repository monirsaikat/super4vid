#define CT_LOG_CATEGORY "overlay"
#include <ct/recorder/region_overlay.h>

#include "overlay_style.h"

#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/image_io.h>
#include <ct/platform/layered_window.h>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

namespace ct::recorder {

using Microsoft::WRL::ComPtr;
using overlay::Handle;

namespace {

D2D1_RECT_F ToF(const RectI& r) {
    return D2D1::RectF(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.Right()),
                       static_cast<float>(r.Bottom()));
}
D2D1_RECT_F InflateF(D2D1_RECT_F r, float by) { return D2D1::RectF(r.left - by, r.top - by, r.right + by, r.bottom + by); }
RectI Offset(const RectI& r, PointI o) { return RectI{r.x - o.x, r.y - o.y, r.w, r.h}; }
bool Overlaps(const RectI& a, const RectI& b) {
    return a.x < b.Right() && b.x < a.Right() && a.y < b.Bottom() && b.y < a.Bottom();
}

struct EnumCtx { std::vector<overlay::SnapWindow>* out; DWORD pid; };

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    overlay::SnapWindow w;
    w.visible = IsWindowVisible(hwnd) != FALSE;
    if (!w.visible) return TRUE;   // most top-level windows are hidden; skip them early
    DWORD cloaked = 0;
    w.cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // Click-through overlays (WS_EX_TRANSPARENT) are not windows a user means to pick.
    w.tool = (ex & WS_EX_TOOLWINDOW) != 0 || ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED));
    w.minimized = IsIconic(hwnd) != FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.own_process = pid == ctx->pid;
    RECT rc{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) GetWindowRect(hwnd, &rc);
    w.rect = RectI{rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top};
    ctx->out->push_back(w);
    return TRUE;
}

} // namespace

struct RegionOverlay::Impl {
    enum class State { Hover, Pressing, Dragging, Adjust };

    platform::LayeredWindow win;
    std::vector<capture::DisplayInfo> displays;
    std::vector<RectI> display_rects;
    std::vector<overlay::SnapWindow> windows;
    RectI virt{};
    PointI origin{};
    Mode mode = Mode::Region;
    DoneFn done = nullptr;
    void* user = nullptr;
    bool open = false;

    State state = State::Hover;
    PointI cursor{}, press{};
    RectI sel{}, hover{}, drag_start{};
    Handle drag_handle = Handle::None;
    int monitor_preset = -1;

    float scale = 1.f;
    int32_t handle_px = overlay::kHandlePx;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> label_fmt, hint_fmt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ID2D1RenderTarget* brush_owner = nullptr;
    std::vector<RectI> last_decor;
    double last_ms = 0;
    double max_update_ms = 0;

    // ---- geometry helpers ----
    int DisplayAt(PointI p) const { return capture::DisplayIndexAt(displays, p); }
    RectI DisplayRectAt(PointI p) const {
        const int i = DisplayAt(p);
        return i >= 0 ? display_rects[static_cast<size_t>(i)] : (display_rects.empty() ? virt : display_rects[0]);
    }
    RectI Hole() const {
        if (state == State::Hover || state == State::Pressing) return hover;
        return sel;
    }
    bool ShowCrosshair() const { return state == State::Hover || state == State::Pressing; }
    bool ShowHandles() const { return state == State::Adjust && !sel.IsEmpty(); }

    std::wstring LabelText() const {
        const RectI h = Hole();
        if (h.IsEmpty()) return {};
        return Utf8ToWide(overlay::SizeLabel(h.w, h.h));
    }

    SizeI Measure(IDWriteTextFormat* fmt, const std::wstring& text, float pad_x, float height) const {
        if (!fmt || text.empty()) return {};
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 10000.f, 1000.f,
                                            layout.GetAddressOf())))
            return {};
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return SizeI{static_cast<int32_t>(tm.widthIncludingTrailingWhitespace + 2 * pad_x + 1), static_cast<int32_t>(height)};
    }

    RectI LabelRectV(const std::wstring& text) const {
        if (text.empty()) return {};
        const SizeI size = Measure(label_fmt.Get(), text, 10 * scale, 26 * scale);
        const RectI h = Hole();
        const RectI anchor = ShowCrosshair() ? RectI{cursor.x + size.w + static_cast<int32_t>(16 * scale), cursor.y + static_cast<int32_t>(8 * scale), 0, 0} : h;
        return overlay::LabelRect(anchor, size, DisplayRectAt(ShowCrosshair() ? cursor : PointI{h.x + h.w / 2, h.y + h.h / 2}),
                                  static_cast<int32_t>(8 * scale));
    }

    const wchar_t* HintText() const {
        if (mode == Mode::Window) return style::kHintWindow;
        return state == State::Adjust ? style::kHintAdjust : style::kHintRegion;
    }

    RectI HintRectV() const {
        const SizeI size = Measure(hint_fmt.Get(), HintText(), 14 * scale, 30 * scale);
        const PointI anchor = state == State::Adjust && !sel.IsEmpty() ? PointI{sel.x + sel.w / 2, sel.y + sel.h / 2} : cursor;
        const RectI d = DisplayRectAt(anchor);
        return RectI{d.x + (d.w - size.w) / 2, d.y + static_cast<int32_t>(24 * scale), size.w, size.h};
    }

    std::vector<RectI> Decorations() const {
        std::vector<RectI> out;
        const RectI h = Hole();
        if (!h.IsEmpty()) out.push_back(overlay::Inflate(h, handle_px + 4));
        if (const RectI l = LabelRectV(LabelText()); !l.IsEmpty()) out.push_back(overlay::Inflate(l, 2));
        if (ShowCrosshair()) {
            out.push_back(RectI{virt.x, cursor.y - 1, virt.w, 3});
            out.push_back(RectI{cursor.x - 1, virt.y, 3, virt.h});
        }
        out.push_back(overlay::Inflate(HintRectV(), 2));
        return out;
    }

    // ---- drawing ----
    void UpdateScale() {
        const PointI anchor = state == State::Adjust && !sel.IsEmpty() ? PointI{sel.x + sel.w / 2, sel.y + sel.h / 2} : cursor;
        const int i = DisplayAt(anchor);
        const UINT dpi = i >= 0 ? displays[static_cast<size_t>(i)].dpi : 96;
        const float s = static_cast<float>(dpi) / 96.f;
        if (s == scale && label_fmt) return;
        scale = s;
        handle_px = std::max(overlay::kHandlePx, static_cast<int32_t>(overlay::kHandlePx * scale + 0.5f));
        label_fmt = style::MakeTextFormat(dwrite.Get(), style::kFontUi, 13.f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hint_fmt = style::MakeTextFormat(dwrite.Get(), style::kFontUi, 13.f * scale, DWRITE_FONT_WEIGHT_NORMAL);
        for (auto* f : {label_fmt.Get(), hint_fmt.Get()}) {
            if (!f) continue;
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    void Pill(ID2D1RenderTarget* rt, const RectI& r, IDWriteTextFormat* fmt, const std::wstring& text) {
        if (r.IsEmpty() || !fmt) return;
        const D2D1_RECT_F f = ToF(Offset(r, origin));
        const float radius = (f.bottom - f.top) / 2;
        brush->SetColor(D2D1_COLOR_F{style::kBg2.r, style::kBg2.g, style::kBg2.b, 0.92f});
        rt->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.Get());
        brush->SetColor(style::kBorder);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(InflateF(f, -0.5f), radius, radius), brush.Get(), 1.f);
        brush->SetColor(style::kText);
        rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, f, brush.Get());
    }

    void Scene(ID2D1RenderTarget* rt) {
        if (brush_owner != rt || !brush) {
            brush.Reset();
            rt->CreateSolidColorBrush(style::kWhite, brush.GetAddressOf());
            brush_owner = rt;
        }
        if (!brush) return;
        brush->SetColor(style::kDim);
        rt->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(virt.w), static_cast<float>(virt.h)), brush.Get());

        const RectI hole = Hole();
        if (!hole.IsEmpty()) {
            const D2D1_RECT_F h = ToF(Offset(hole, origin));
            rt->PushAxisAlignedClip(h, D2D1_ANTIALIAS_MODE_ALIASED);
            rt->Clear(style::kHole);
            rt->PopAxisAlignedClip();
            if (state == State::Hover || state == State::Pressing) {
                brush->SetColor(style::kAccent);
                rt->DrawRectangle(InflateF(h, 1.f), brush.Get(), 2.f);
            } else {
                brush->SetColor(style::kWhite);
                rt->DrawRectangle(InflateF(h, 0.5f), brush.Get(), 1.f);
                brush->SetColor(style::kBlack);
                rt->DrawRectangle(InflateF(h, 1.5f), brush.Get(), 1.f);
            }
        }
        if (ShowHandles()) {
            static constexpr Handle kHandles[] = {Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right,
                                                  Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left};
            for (Handle hd : kHandles) {
                const D2D1_RECT_F f = ToF(Offset(overlay::HandleRect(sel, hd, handle_px), origin));
                brush->SetColor(style::kWhite);
                rt->FillRectangle(f, brush.Get());
                brush->SetColor(style::kAccent);
                rt->DrawRectangle(InflateF(f, -0.5f), brush.Get(), 1.f);
            }
        }
        if (ShowCrosshair()) {
            const float cx = static_cast<float>(cursor.x - origin.x) + 0.5f, cy = static_cast<float>(cursor.y - origin.y) + 0.5f;
            brush->SetColor(D2D1_COLOR_F{1.f, 1.f, 1.f, 0.55f});
            rt->DrawLine(D2D1::Point2F(0, cy), D2D1::Point2F(static_cast<float>(virt.w), cy), brush.Get(), 1.f);
            rt->DrawLine(D2D1::Point2F(cx, 0), D2D1::Point2F(cx, static_cast<float>(virt.h)), brush.Get(), 1.f);
        }
        const std::wstring label = LabelText();
        Pill(rt, LabelRectV(label), label_fmt.Get(), label);
        Pill(rt, HintRectV(), hint_fmt.Get(), HintText());
    }

    void DrawRect(const RectI& dirty_v) {
        const RectI rel = Offset(dirty_v, origin);
        ID2D1RenderTarget* rt = win.BeginDraw(&rel);
        if (!rt) return;
        Scene(rt);
        win.EndDrawAndPresent();
        last_ms = std::max(last_ms, win.LastDrawMs());
    }

    void Update(bool full = false) {
        const int64_t t0 = Clock::QpcNow();
        UpdateRects(full);
        if (!full) max_update_ms = std::max(max_update_ms, static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1000.0);
    }

    void UpdateRects(bool full) {
        UpdateScale();
        std::vector<RectI> now = Decorations();
        last_ms = 0;
        if (full) {
            DrawRect(virt);
        } else {
            std::vector<RectI> dirty = last_decor;
            dirty.insert(dirty.end(), now.begin(), now.end());
            // Merge overlapping rects when the union is not much larger than the two (redrawing an area twice is
            // harmless; merging the crosshair's full-width and full-height lines would redraw the whole screen).
            auto area = [](const RectI& r) { return static_cast<int64_t>(r.w) * r.h; };
            for (bool merged = true; merged;) {
                merged = false;
                for (size_t i = 0; i < dirty.size() && !merged; ++i) {
                    for (size_t j = i + 1; j < dirty.size(); ++j) {
                        const RectI u = overlay::Union(dirty[i], dirty[j]);
                        if (Overlaps(dirty[i], dirty[j]) && area(u) * 4 <= (area(dirty[i]) + area(dirty[j])) * 5) {
                            dirty[i] = overlay::Union(dirty[i], dirty[j]);
                            dirty.erase(dirty.begin() + static_cast<ptrdiff_t>(j));
                            merged = true;
                            break;
                        }
                    }
                }
            }
            for (const RectI& r : dirty) {
                const RectI clipped = r.Intersect(virt);
                if (!clipped.IsEmpty()) DrawRect(clipped);
            }
        }
        last_decor = std::move(now);
    }

    // ---- behaviour ----
    void RefreshHover() {
        RectI r = overlay::SnapRectAt(windows, cursor, DisplayRectAt(cursor));
        if (r.IsEmpty()) r = overlay::MakeEven(DisplayRectAt(cursor));
        hover = r;
    }

    void Finish(bool confirmed) {
        if (!open) return;
        Result res;
        const RectI pick = (state == State::Hover || state == State::Pressing) ? hover : sel;
        if (confirmed) {
            const overlay::FinalRegion f = overlay::Finalize(pick, display_rects);
            if (f.display >= 0) {
                res.confirmed = true;
                res.region = f.region;
                res.display_index = displays[static_cast<size_t>(f.display)].index;
            }
        }
        open = false;
        if (GetCapture() == win.Hwnd()) ReleaseCapture();
        win.Show(false);
        CT_LOG_INFO("region overlay %s: %dx%d at (%d,%d) display %d; slowest update %.1f ms",
                    res.confirmed ? "confirmed" : "cancelled", res.region.w, res.region.h, res.region.x, res.region.y,
                    res.display_index, max_update_ms);
        if (done) done(user, res);
    }

    PointI ToVirtual(LPARAM lparam) const { return PointI{GET_X_LPARAM(lparam) + origin.x, GET_Y_LPARAM(lparam) + origin.y}; }

    void OnMouseMove(PointI p) {
        if (p.x == cursor.x && p.y == cursor.y && state != State::Pressing) return;
        cursor = p;
        switch (state) {
        case State::Hover: RefreshHover(); break;
        case State::Pressing: {
            const int32_t dx = std::abs(p.x - press.x), dy = std::abs(p.y - press.y);
            const int32_t threshold = static_cast<int32_t>(4 * scale);
            if (mode == Mode::Region && (dx > threshold || dy > threshold)) {
                state = State::Dragging;
                sel = overlay::RectFromDrag(press, p).Intersect(virt);
                sel = overlay::MakeEven(sel);
            }
            break;
        }
        case State::Dragging:
            sel = overlay::MakeEven(overlay::RectFromDrag(press, p).Intersect(virt));
            break;
        case State::Adjust:
            if (drag_handle != Handle::None) {
                sel = overlay::ApplyDrag(drag_start, drag_handle, PointI{p.x - press.x, p.y - press.y}, virt);
            } else {
                return;   // only the cursor shape changes (WM_SETCURSOR)
            }
            break;
        }
        Update();
    }

    void OnButtonDown(PointI p) {
        cursor = p;
        press = p;
        SetCapture(win.Hwnd());
        if (state == State::Adjust) {
            drag_handle = overlay::HitTest(sel, p, handle_px);
            if (drag_handle != Handle::None) {
                drag_start = sel;
                return;
            }
            state = State::Pressing;   // outside the selection: start over with a new drag
            sel = {};
            RefreshHover();
            Update();
            return;
        }
        state = State::Pressing;
    }

    void OnButtonUp(PointI p) {
        cursor = p;
        if (GetCapture() == win.Hwnd()) ReleaseCapture();
        switch (state) {
        case State::Pressing:   // a click: the window under the cursor (or the monitor)
            RefreshHover();
            if (mode == Mode::Window) {
                state = State::Hover;
                Finish(true);
                return;
            }
            sel = hover;
            state = State::Adjust;
            break;
        case State::Dragging:
            state = State::Adjust;
            if (sel.w < 8 || sel.h < 8) sel = hover;   // a tiny drag is treated as a click
            break;
        case State::Adjust:
            drag_handle = Handle::None;
            break;
        case State::Hover:
            break;
        }
        Update();
    }

    void OnKey(WPARAM vk) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        switch (vk) {
        case VK_ESCAPE: Finish(false); return;
        case VK_RETURN:
        case VK_SPACE: Finish(true); return;
        case 'R':
            state = State::Hover;
            sel = {};
            drag_handle = Handle::None;
            RefreshHover();
            break;
        case 'M':
            if (display_rects.empty() || mode == Mode::Window) return;
            monitor_preset = overlay::NextMonitor(monitor_preset, static_cast<int>(display_rects.size()));
            sel = overlay::MakeEven(display_rects[static_cast<size_t>(monitor_preset)]);
            state = State::Adjust;
            break;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
            if (state != State::Adjust || sel.IsEmpty()) return;
            const int32_t step = ctrl ? (shift ? 10 : 2) : (shift ? 10 : 1);
            const int32_t dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
            const int32_t dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
            sel = overlay::Nudge(sel, dx, dy, ctrl, virt);
            break;
        }
        default:
            return;
        }
        Update();
    }

    LPCWSTR CursorFor(PointI p) const {
        if (state != State::Adjust) return IDC_CROSS;
        const Handle h = drag_handle != Handle::None ? drag_handle : overlay::HitTest(sel, p, handle_px);
        switch (h) {
        case Handle::TopLeft: case Handle::BottomRight: return IDC_SIZENWSE;
        case Handle::TopRight: case Handle::BottomLeft: return IDC_SIZENESW;
        case Handle::Top: case Handle::Bottom: return IDC_SIZENS;
        case Handle::Left: case Handle::Right: return IDC_SIZEWE;
        case Handle::Inside: return IDC_SIZEALL;
        default: return IDC_CROSS;
        }
    }

    static LRESULT Handler(void* user_ptr, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled) {
        auto* self = static_cast<Impl*>(user_ptr);
        if (!self->open) return 0;
        switch (msg) {
        case WM_MOUSEMOVE: self->OnMouseMove(self->ToVirtual(lparam)); handled = true; return 0;
        case WM_LBUTTONDOWN: self->OnButtonDown(self->ToVirtual(lparam)); handled = true; return 0;
        case WM_LBUTTONUP: self->OnButtonUp(self->ToVirtual(lparam)); handled = true; return 0;
        case WM_RBUTTONUP:
            // Right click: back one step (adjust -> hover), cancel from hover.
            if (self->state == State::Adjust) self->OnKey('R');
            else self->Finish(false);
            handled = true;
            return 0;
        case WM_KEYDOWN: self->OnKey(wparam); handled = true; return 0;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            SetCursor(LoadCursorW(nullptr, self->CursorFor(PointI{pt.x, pt.y})));
            handled = true;
            return TRUE;
        }
        case WM_MOUSEACTIVATE: handled = true; return MA_ACTIVATE;
        case WM_CLOSE: self->Finish(false); handled = true; return 0;
        default: break;
        }
        (void)hwnd;
        return 0;
    }
};

RegionOverlay::RegionOverlay() : m(std::make_unique<Impl>()) {}
RegionOverlay::~RegionOverlay() { Close(); }

bool RegionOverlay::Open(const std::vector<capture::DisplayInfo>& displays, Mode mode, DoneFn done, void* user,
                         const RectI* initial) {
    Close();
    Impl& s = *m;
    if (displays.empty()) return false;
    s.displays = displays;
    s.display_rects.clear();
    for (const auto& d : displays) s.display_rects.push_back(d.rect);
    s.virt = capture::VirtualScreenRect();
    for (const RectI& r : s.display_rects) s.virt = overlay::Union(s.virt, r);
    s.origin = PointI{s.virt.x, s.virt.y};
    s.mode = mode;
    s.done = done;
    s.user = user;
    s.windows.clear();
    EnumCtx ctx{&s.windows, GetCurrentProcessId()};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (!s.dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                reinterpret_cast<IUnknown**>(s.dwrite.GetAddressOf()))))
        return false;
    if (!s.win.Create(s.virt, false, true, true)) return false;
    s.win.SetHandler(&Impl::Handler, &s);
    POINT pt{};
    GetCursorPos(&pt);
    s.cursor = PointI{pt.x, pt.y};
    s.drag_handle = Handle::None;
    s.monitor_preset = -1;
    s.brush.Reset();
    s.brush_owner = nullptr;
    s.label_fmt.Reset();
    s.last_decor.clear();
    s.max_update_ms = 0;
    if (initial && !initial->IsEmpty() && mode == Mode::Region) {
        s.sel = overlay::MakeEven(*initial);
        s.state = Impl::State::Adjust;
    } else {
        s.sel = {};
        s.state = Impl::State::Hover;
    }
    s.RefreshHover();
    s.open = true;
    s.Update(true);
    s.win.Show(true, true);
    SetFocus(s.win.Hwnd());
    CT_LOG_INFO("region overlay open: %dx%d at (%d,%d), %zu windows, first draw %.1f ms", s.virt.w, s.virt.h, s.virt.x,
                s.virt.y, s.windows.size(), s.last_ms);
    return true;
}

void RegionOverlay::Close() {
    if (!m) return;
    if (m->open) m->Finish(false);
    m->win.Destroy();
}

bool RegionOverlay::IsOpen() const { return m->open; }
double RegionOverlay::LastDrawMs() const { return m->last_ms; }
double RegionOverlay::MaxUpdateMs() const { return m->max_update_ms; }

bool RegionOverlay::SaveSnapshot(const char* path) const {
    std::vector<uint8_t> rgba;
    if (!m->win.ReadPixelsRgba(rgba)) return false;
    const RectI r = m->win.Rect();
    return platform::ImageIo::WritePngRgba(path, rgba.data(), r.w, r.h, r.w * 4);
}

RegionOverlay::Result RegionOverlay::RunModal(const std::vector<capture::DisplayInfo>& displays, Mode mode,
                                              const RectI* initial) {
    struct Ctx { bool done = false; Result result; } ctx;
    RegionOverlay overlay;
    if (!overlay.Open(displays, mode, [](void* u, const Result& r) {
            auto* c = static_cast<Ctx*>(u);
            c->result = r;
            c->done = true;
        }, &ctx, initial))
        return {};
    MSG msg;
    while (!ctx.done) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            overlay.Close();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ctx.result;
}

} // namespace ct::recorder
