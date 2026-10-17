#define CT_LOG_CATEGORY "platform"
#include <ct/platform/layered_window.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>

#include <algorithm>
#include <cstring>

namespace ct::platform {

namespace {
constexpr wchar_t kClassName[] = L"CamtesiaLayered";

bool RegisterClassOnce(WNDPROC proc) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}
} // namespace

LayeredWindow::~LayeredWindow() { Destroy(); }

bool LayeredWindow::Create(const RectI& r, bool click_through, bool topmost, bool exclude_from_capture) {
    Destroy();
    if (r.IsEmpty() || !RegisterClassOnce(WndProc)) return false;
    if (!m_factory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_factory.GetAddressOf());
        if (FAILED(hr)) {
            LogHr(hr, "D2D1CreateFactory", __FILE__, __LINE__);
            return false;
        }
    }
    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (topmost) ex |= WS_EX_TOPMOST;
    if (click_through) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    m_rect = r;
    m_hwnd = CreateWindowExW(ex, kClassName, L"", WS_POPUP, r.x, r.y, r.w, r.h, nullptr, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (!m_hwnd) {
        CT_LOG_ERROR("CreateWindowExW(layered) failed: %lu", GetLastError());
        return false;
    }
    m_excluded = false;
    if (exclude_from_capture) {
        m_excluded = SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        if (!m_excluded) CT_LOG_WARN("WDA_EXCLUDEFROMCAPTURE refused (%lu); overlay may appear in recordings", GetLastError());
    }
    m_need_full = true;
    return CreateSurface();
}

bool LayeredWindow::CreateSurface() {
    ReleaseSurface();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = m_rect.w;
    bi.bmiHeader.biHeight = -m_rect.h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    m_dc = CreateCompatibleDC(nullptr);
    if (!m_dc) return false;
    m_bmp = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (!m_bmp) {
        CT_LOG_ERROR("CreateDIBSection %dx%d failed", m_rect.w, m_rect.h);
        return false;
    }
    m_old = SelectObject(m_dc, m_bmp);
    m_need_full = true;
    return EnsureTarget();
}

void LayeredWindow::ReleaseSurface() {
    m_target.Reset();
    m_wic.Reset();
    if (m_dc && m_old) SelectObject(m_dc, m_old);
    if (m_bmp) DeleteObject(m_bmp);
    if (m_dc) DeleteDC(m_dc);
    m_dc = nullptr;
    m_bmp = nullptr;
    m_old = nullptr;
    m_bits = nullptr;
}

bool LayeredWindow::EnsureTarget() {
    if (m_target) return true;
    if (!m_dc || !m_factory) return false;
    // Software rasterizer: a hardware target would read the surface back from the GPU on every EndDraw, which
    // costs more than rasterizing a dim fill and a few shapes.
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    HRESULT hr = S_OK;
    if (!m_wic_factory) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_wic_factory.GetAddressOf()));
        if (FAILED(hr)) CT_LOG_WARN("WIC unavailable (%s); layered window uses a DC render target", HrToString(hr).c_str());
    }
    if (m_wic_factory) {
        hr = m_wic_factory->CreateBitmap(static_cast<UINT>(m_rect.w), static_cast<UINT>(m_rect.h),
                                         GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, m_wic.GetAddressOf());
        if (SUCCEEDED(hr)) hr = m_factory->CreateWicBitmapRenderTarget(m_wic.Get(), &props, m_target.GetAddressOf());
        if (FAILED(hr)) {
            LogHr(hr, "CreateWicBitmapRenderTarget", __FILE__, __LINE__);
            m_wic.Reset();
            m_target.Reset();
        }
    }
    if (!m_target) {
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dc_target;
        hr = m_factory->CreateDCRenderTarget(&props, dc_target.GetAddressOf());
        if (FAILED(hr)) {
            LogHr(hr, "CreateDCRenderTarget", __FILE__, __LINE__);
            return false;
        }
        const RECT rc{0, 0, m_rect.w, m_rect.h};
        hr = dc_target->BindDC(m_dc, &rc);
        if (FAILED(hr)) {
            LogHr(hr, "BindDC", __FILE__, __LINE__);
            return false;
        }
        m_target = dc_target;
    }
    m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType needs an opaque background
    return true;
}

ID2D1RenderTarget* LayeredWindow::BeginDraw(const RectI* dirty) {
    if (m_drawing || !m_hwnd || !EnsureTarget()) return nullptr;
    m_draw_start_qpc = Clock::QpcNow();
    RECT d{0, 0, m_rect.w, m_rect.h};
    if (dirty && !m_need_full) {
        d.left = std::max<LONG>(0, dirty->x);
        d.top = std::max<LONG>(0, dirty->y);
        d.right = std::min<LONG>(m_rect.w, dirty->Right());
        d.bottom = std::min<LONG>(m_rect.h, dirty->Bottom());
        if (d.right <= d.left || d.bottom <= d.top) d = RECT{0, 0, 0, 0};
    }
    m_dirty = d;
    m_target->BeginDraw();
    m_drawing = true;
    m_clipped = !(d.left == 0 && d.top == 0 && d.right == m_rect.w && d.bottom == m_rect.h);
    if (m_clipped) {
        m_target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(d.left), static_cast<float>(d.top),
                                                  static_cast<float>(d.right), static_cast<float>(d.bottom)),
                                      D2D1_ANTIALIAS_MODE_ALIASED);
    }
    m_target->Clear(D2D1::ColorF(0, 0, 0, 0));
    return m_target.Get();
}

void LayeredWindow::EndDrawAndPresent(BYTE alpha) {
    if (!m_drawing) return;
    if (m_clipped) m_target->PopAxisAlignedClip();
    const HRESULT hr = m_target->EndDraw();
    m_drawing = false;
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        m_target.Reset();
        m_need_full = true;
        return;
    }
    if (FAILED(hr)) LogHr(hr, "ID2D1RenderTarget::EndDraw", __FILE__, __LINE__);
    if (m_dirty.right > m_dirty.left && m_dirty.bottom > m_dirty.top) {
        if (m_wic) CopyToDib(m_dirty);
        PushToScreen(alpha, &m_dirty);
    }
    m_last_draw_ms = static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), m_draw_start_qpc)) / 1000.0;
}

void LayeredWindow::CopyToDib(const RECT& r) {
    const WICRect wr{r.left, r.top, r.right - r.left, r.bottom - r.top};
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    if (FAILED(m_wic->Lock(&wr, WICBitmapLockRead, lock.GetAddressOf()))) return;
    UINT stride = 0, size = 0;
    BYTE* src = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &src)) || !src) return;
    auto* dst = static_cast<uint8_t*>(m_bits);
    const size_t dst_stride = static_cast<size_t>(m_rect.w) * 4;
    const size_t row_bytes = static_cast<size_t>(wr.Width) * 4;
    for (int y = 0; y < wr.Height; ++y)
        std::memcpy(dst + (static_cast<size_t>(wr.Y) + y) * dst_stride + static_cast<size_t>(wr.X) * 4,
                    src + static_cast<size_t>(y) * stride, row_bytes);
}

void LayeredWindow::Present(BYTE alpha) {
    if (!m_hwnd || m_drawing) return;
    PushToScreen(alpha, nullptr);
}

void LayeredWindow::PushToScreen(BYTE alpha, const RECT* dirty) {
    GdiFlush();
    POINT dst{m_rect.x, m_rect.y};
    SIZE size{m_rect.w, m_rect.h};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = m_dc;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    // A dirty rect is honoured only after the window has its full content and its constant alpha is unchanged.
    info.prcDirty = (m_need_full || alpha != m_alpha) ? nullptr : dirty;
    if (!UpdateLayeredWindowIndirect(m_hwnd, &info)) {
        CT_LOG_WARN("UpdateLayeredWindowIndirect failed: %lu", GetLastError());
        return;
    }
    m_alpha = alpha;
    m_need_full = false;
}

bool LayeredWindow::ReadPixelsRgba(std::vector<uint8_t>& out) const {
    if (!m_bits || m_drawing) return false;
    GdiFlush();
    const size_t n = static_cast<size_t>(m_rect.w) * static_cast<size_t>(m_rect.h);
    out.resize(n * 4);
    const auto* src = static_cast<const uint8_t*>(m_bits);   // premultiplied BGRA
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2], a = src[i * 4 + 3];
        auto un = [a](uint8_t c) { return a ? static_cast<uint8_t>(std::min(255, (c * 255 + a / 2) / a)) : uint8_t{0}; };
        out[i * 4] = un(r);
        out[i * 4 + 1] = un(g);
        out[i * 4 + 2] = un(b);
        out[i * 4 + 3] = a;
    }
    return true;
}

void LayeredWindow::Move(const RectI& r) {
    if (!m_hwnd || r.IsEmpty()) return;
    const bool resized = r.w != m_rect.w || r.h != m_rect.h;
    m_rect = r;
    if (resized) {
        CreateSurface();
    } else {
        SetWindowPos(m_hwnd, nullptr, r.x, r.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void LayeredWindow::Show(bool show, bool activate) {
    if (!m_hwnd) return;
    if (!show) {
        ShowWindow(m_hwnd, SW_HIDE);
        return;
    }
    ShowWindow(m_hwnd, activate ? SW_SHOW : SW_SHOWNA);
    if (activate) SetForegroundWindow(m_hwnd);
}

bool LayeredWindow::IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd); }

void LayeredWindow::Destroy() {
    if (m_drawing && m_target) {
        if (m_clipped) m_target->PopAxisAlignedClip();
        m_target->EndDraw();
        m_drawing = false;
    }
    ReleaseSurface();
    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK LayeredWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<LayeredWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->m_handler) {
        bool handled = false;
        const LRESULT r = self->m_handler(self->m_user, hwnd, msg, wparam, lparam, handled);
        if (handled) return r;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace ct::platform
