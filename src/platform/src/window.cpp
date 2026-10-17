#define CT_LOG_CATEGORY "platform"
#include <ct/platform/window.h>

#include <ct/core/log.h>

#include <dwmapi.h>

namespace ct::platform {

namespace {

// DWM attribute values, spelled out because the SDK guards some of them behind NTDDI versions above our minimum.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;     // DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+)
constexpr DWORD kDwmWindowCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE (Win11)
constexpr DWORD kDwmCornerDoNotRound = 1;          // DWMWCP_DONOTROUND
constexpr DWORD kDwmCornerRound = 2;               // DWMWCP_ROUND
constexpr DWORD kWdaExcludeFromCapture = 0x11;     // WDA_EXCLUDEFROMCAPTURE (Win10 2004+)

int DipToPx(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

} // namespace

Window::~Window() {
    Destroy();
}

bool Window::Create(const Desc& desc, MessageHandler handler, void* user) {
    m_handler = handler;
    m_user = user;
    m_quit_on_destroy = desc.quit_on_destroy;
    m_min_client_dip = desc.min_client_dip;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::WndProc;
    wc.hInstance = instance;
    wc.hIcon = desc.icon;
    wc.hIconSm = desc.icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // the swapchain paints everything; no GDI erase flash
    wc.lpszClassName = desc.class_name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CT_LOG_ERROR("RegisterClassExW failed: %lu", GetLastError());
        return false;
    }

    if (desc.frameless) {
        m_style = WS_POPUP;
    } else if (desc.resizable) {
        m_style = WS_OVERLAPPEDWINDOW;
    } else {
        m_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    }
    m_ex_style = (desc.topmost ? WS_EX_TOPMOST : 0u) | (desc.tool_window ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW);

    // Create hidden first, then size it for the DPI of the monitor it actually landed on.
    HWND hwnd = CreateWindowExW(m_ex_style, desc.class_name, desc.title, m_style, CW_USEDEFAULT, CW_USEDEFAULT,
                                desc.w_dip, desc.h_dip, nullptr, nullptr, instance, this);
    if (!hwnd) {
        CT_LOG_ERROR("CreateWindowExW failed: %lu", GetLastError());
        return false;
    }
    m_hwnd = hwnd;
    m_dpi = GetDpiForWindow(hwnd);

    RECT rc{0, 0, DipToPx(desc.w_dip, m_dpi), DipToPx(desc.h_dip, m_dpi)};
    AdjustWindowRectExForDpi(&rc, m_style, FALSE, m_ex_style, m_dpi);
    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& work = mi.rcWork;
    const int x = work.left + ((work.right - work.left) - win_w) / 2;
    const int y = work.top + ((work.bottom - work.top) - win_h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
    m_client_px = ClientSizePx();
    m_resize_pending = false;
    CT_LOG_INFO("window created: %dx%d px at dpi %u", m_client_px.w, m_client_px.h, m_dpi);
    return true;
}

SizeI Window::ClientSizePx() const {
    RECT rc{};
    if (m_hwnd) GetClientRect(m_hwnd, &rc);
    return SizeI{rc.right - rc.left, rc.bottom - rc.top};
}

bool Window::ConsumeResize() {
    const bool pending = m_resize_pending;
    m_resize_pending = false;
    return pending;
}

void Window::Show(int cmd) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, cmd);
        UpdateWindow(m_hwnd);
    }
}

void Window::SetTitle(const wchar_t* title) {
    if (m_hwnd) SetWindowTextW(m_hwnd, title);
}

void Window::SetDarkTitleBar(bool dark) {
    if (!m_hwnd) return;
    const BOOL value = dark ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(m_hwnd, kDwmUseImmersiveDarkMode, &value, sizeof(value));
    if (FAILED(hr)) CT_LOG_DEBUG("dark title bar not supported (0x%08lX)", static_cast<unsigned long>(hr));
}

void Window::SetRoundedCorners(bool rounded) {
    if (!m_hwnd) return;
    const DWORD pref = rounded ? kDwmCornerRound : kDwmCornerDoNotRound;
    // Fails on Windows 10, where corners are always square. That is expected.
    HRESULT hr = DwmSetWindowAttribute(m_hwnd, kDwmWindowCornerPreference, &pref, sizeof(pref));
    if (FAILED(hr)) CT_LOG_DEBUG("corner preference not supported (0x%08lX)", static_cast<unsigned long>(hr));
}

void Window::SetExcludeFromCapture(bool exclude) {
    if (!m_hwnd) return;
    if (!SetWindowDisplayAffinity(m_hwnd, exclude ? kWdaExcludeFromCapture : WDA_NONE))
        CT_LOG_WARN("SetWindowDisplayAffinity failed: %lu (needs Windows 10 2004)", GetLastError());
}

void Window::Destroy() {
    if (m_hwnd) {
        HWND hwnd = m_hwnd;
        DestroyWindow(hwnd);   // WM_NCDESTROY clears m_hwnd
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->HandleMessage(msg, wparam, lparam);
}

LRESULT Window::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND hwnd = m_hwnd;

    // 1. Our own state, so the handler sees the updated size/DPI.
    switch (msg) {
    case WM_DPICHANGED: {
        m_dpi = HIWORD(wparam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        CT_LOG_INFO("dpi changed to %u", m_dpi);
        break;
    }
    case WM_SIZE: {
        m_minimized = (wparam == SIZE_MINIMIZED);
        if (!m_minimized) {
            const SizeI size{static_cast<int32_t>(LOWORD(lparam)), static_cast<int32_t>(HIWORD(lparam))};
            if (size.w != m_client_px.w || size.h != m_client_px.h) {
                m_client_px = size;
                m_resize_pending = true;
            }
        }
        break;
    }
    default:
        break;
    }

    // 2. The owner's handler.
    if (m_handler) {
        bool handled = false;
        const LRESULT result = m_handler(m_user, hwnd, msg, wparam, lparam, handled);
        if (handled) return result;
    }

    // 3. Defaults.
    switch (msg) {
    case WM_GETMINMAXINFO: {
        if (m_min_client_dip.w > 0 && m_min_client_dip.h > 0) {
            RECT rc{0, 0, DipToPx(m_min_client_dip.w, m_dpi), DipToPx(m_min_client_dip.h, m_dpi)};
            AdjustWindowRectExForDpi(&rc, m_style, FALSE, m_ex_style, m_dpi);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
            mmi->ptMinTrackSize.x = rc.right - rc.left;
            mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        }
        return 0;
    }
    case WM_DESTROY:
        if (m_quit_on_destroy) PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace ct::platform
