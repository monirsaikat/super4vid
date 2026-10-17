#include "test_util.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::test_util {

namespace {
LRESULT CALLBACK SolidProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        auto brush = reinterpret_cast<HBRUSH>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

SolidWindow::SolidWindow(int x, int y, int w, int h, COLORREF color) {
    static ATOM atom = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &SolidProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CtCaptureTestSolid";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc);
    }();
    (void)atom;
    m_brush = CreateSolidBrush(color);
    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"CtCaptureTestSolid", L"ct capture test",
                             WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m_brush));
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(m_hwnd);
        Pump();
    }
}

SolidWindow::~SolidWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    Pump();
    if (m_brush) DeleteObject(m_brush);
}

void SolidWindow::SetColor(COLORREF color) {
    HBRUSH old = m_brush;
    m_brush = CreateSolidBrush(color);
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m_brush));
    InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateWindow(m_hwnd);
    if (old) DeleteObject(old);
    Pump();
}

void SolidWindow::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool ReadbackTexture(platform::D3DDevice& dev, ID3D11Texture2D* tex, std::vector<uint8_t>& out, UINT& width, UINT& height) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    width = d.Width;
    height = d.Height;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return false;
    platform::D3DDevice::Lock lock(dev);
    dev.Context()->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(dev.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const auto* src = static_cast<const uint8_t*>(m.pData);
    if (d.Format == DXGI_FORMAT_NV12) {
        out.resize(static_cast<size_t>(d.Width) * d.Height * 3 / 2);
        for (UINT y = 0; y < d.Height; ++y) std::memcpy(&out[y * d.Width], src + y * m.RowPitch, d.Width);
        // The UV plane follows the Y plane at RowPitch * Height.
        const uint8_t* uv = src + static_cast<size_t>(m.RowPitch) * d.Height;
        uint8_t* dst_uv = out.data() + static_cast<size_t>(d.Width) * d.Height;
        for (UINT y = 0; y < d.Height / 2; ++y) std::memcpy(dst_uv + y * d.Width, uv + y * m.RowPitch, d.Width);
    } else {
        const UINT bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        out.resize(static_cast<size_t>(d.Width) * d.Height * bpp);
        for (UINT y = 0; y < d.Height; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * d.Width * bpp], src + y * m.RowPitch, static_cast<size_t>(d.Width) * bpp);
    }
    dev.Context()->Unmap(staging.Get(), 0);
    return true;
}

} // namespace ct::test_util
