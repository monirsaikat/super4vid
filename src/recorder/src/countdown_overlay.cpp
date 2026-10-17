#define CT_LOG_CATEGORY "overlay"
#include <ct/recorder/countdown_overlay.h>

#include "overlay_style.h"

#include <ct/core/log.h>
#include <ct/platform/layered_window.h>
#include <ct/recorder/overlay_geometry.h>

#include <cwchar>

namespace ct::recorder {

using Microsoft::WRL::ComPtr;

struct CountdownOverlay::Impl {
    static constexpr UINT_PTR kTimerId = 1;

    platform::LayeredWindow win;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> number_fmt;
    DoneFn done = nullptr;
    TickFn tick = nullptr;
    void* user = nullptr;
    int left = 0;
    bool running = false;
    float scale = 1.f;

    void Draw() {
        ID2D1RenderTarget* rt = win.BeginDraw();
        if (!rt) return;
        const RectI r = win.Rect();
        const float w = static_cast<float>(r.w), h = static_cast<float>(r.h);
        ComPtr<ID2D1SolidColorBrush> brush;
        rt->CreateSolidColorBrush(D2D1_COLOR_F{style::kBg2.r, style::kBg2.g, style::kBg2.b, 0.85f}, brush.GetAddressOf());
        if (brush) {
            const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(w / 2, h / 2), w / 2 - 1, h / 2 - 1);
            rt->FillEllipse(circle, brush.Get());
            brush->SetColor(style::kBorder);
            rt->DrawEllipse(D2D1::Ellipse(circle.point, circle.radiusX - 0.5f, circle.radiusY - 0.5f), brush.Get(), 1.f);
            if (number_fmt) {
                wchar_t text[8];
                std::swprintf(text, 8, L"%d", left);
                brush->SetColor(style::kText);
                rt->DrawText(text, static_cast<UINT32>(std::wcslen(text)), number_fmt.Get(), D2D1::RectF(0, 0, w, h),
                             brush.Get());
            }
        }
        win.EndDrawAndPresent();
    }

    void Finish(bool completed) {
        if (!running) return;
        running = false;
        KillTimer(win.Hwnd(), kTimerId);
        win.Show(false);
        if (done) done(user, completed);
    }

    static LRESULT Handler(void* u, HWND hwnd, UINT msg, WPARAM wparam, LPARAM, bool& handled) {
        auto* self = static_cast<Impl*>(u);
        if (msg == WM_TIMER && wparam == kTimerId) {
            handled = true;
            if (!self->running) return 0;
            --self->left;
            if (self->left <= 0) {
                self->Finish(true);
                return 0;
            }
            self->Draw();
            if (self->tick) self->tick(self->user, self->left);
            return 0;
        }
        if (msg == WM_NCHITTEST) {
            handled = true;
            return HTTRANSPARENT;
        }
        (void)hwnd;
        return 0;
    }
};

CountdownOverlay::CountdownOverlay() : m(std::make_unique<Impl>()) {}
CountdownOverlay::~CountdownOverlay() {
    m->done = nullptr;
    Cancel();
    m->win.Destroy();
}

bool CountdownOverlay::Start(const RectI& region, UINT dpi, int seconds, DoneFn done, TickFn tick, void* user) {
    Cancel();
    Impl& s = *m;
    s.done = done;
    s.tick = tick;
    s.user = user;
    if (seconds <= 0) {
        if (done) done(user, true);
        return true;
    }
    s.scale = static_cast<float>(dpi ? dpi : 96) / 96.f;
    const int32_t diameter = static_cast<int32_t>(160.f * s.scale + 0.5f);
    const RectI r = overlay::CenteredSquare(region, diameter);
    if (r.IsEmpty()) return false;
    if (!s.dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                reinterpret_cast<IUnknown**>(s.dwrite.GetAddressOf()))))
        return false;
    s.number_fmt = style::MakeTextFormat(s.dwrite.Get(), style::kFontDisplay, 72.f * s.scale * static_cast<float>(r.w) / diameter,
                                         DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (s.number_fmt) {
        s.number_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        s.number_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (!s.win.Hwnd()) {
        if (!s.win.Create(r, true, true, true)) return false;
        s.win.SetHandler(&Impl::Handler, &s);
    } else {
        s.win.Move(r);
    }
    s.left = seconds;
    s.running = true;
    s.Draw();
    s.win.Show(true, false);
    SetTimer(s.win.Hwnd(), Impl::kTimerId, 1000, nullptr);
    if (tick) tick(user, s.left);
    return true;
}

void CountdownOverlay::Cancel() { m->Finish(false); }
bool CountdownOverlay::IsRunning() const { return m->running; }
int CountdownOverlay::SecondsLeft() const { return m->running ? m->left : 0; }
bool CountdownOverlay::ExcludedFromCapture() const { return m->win.ExcludedFromCapture(); }

bool CountdownOverlay::RunModal(const RectI& region, UINT dpi, int seconds) {
    struct Ctx { bool done = false, completed = false; } ctx;
    CountdownOverlay overlay;
    if (!overlay.Start(region, dpi, seconds, [](void* u, bool completed) {
            auto* c = static_cast<Ctx*>(u);
            c->done = true;
            c->completed = completed;
        }, nullptr, &ctx))
        return false;
    MSG msg;
    while (!ctx.done) {
        // Wake at least every 50 ms to poll Esc (the click-through overlay never has focus).
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                overlay.Cancel();
                PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) overlay.Cancel();
    }
    return ctx.completed;
}

} // namespace ct::recorder
