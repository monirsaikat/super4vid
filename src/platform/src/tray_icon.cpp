#define CT_LOG_CATEGORY "platform"
#include <ct/platform/tray_icon.h>

#include <ct/core/log.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

namespace ct::platform {

TrayIcon::~TrayIcon() { Remove(); }

UINT TrayIcon::TaskbarCreatedMessage() {
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

bool TrayIcon::Send(DWORD message, UINT flags) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kId;
    nid.uFlags = flags;
    nid.uCallbackMessage = m_callback;
    nid.hIcon = m_icon;
    wcsncpy_s(nid.szTip, m_tip, _TRUNCATE);
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIconW(message, &nid) != FALSE;
}

bool TrayIcon::Add(HWND hwnd, HICON icon, const wchar_t* tip, UINT callback_msg) {
    if (m_added) Remove();
    m_hwnd = hwnd;
    m_icon = icon;
    m_callback = callback_msg;
    wcsncpy_s(m_tip, tip ? tip : L"", _TRUNCATE);
    return Readd();
}

bool TrayIcon::Readd() {
    if (!m_hwnd) return false;
    // NIF_SHOWTIP: with version 4 the standard tooltip is shown only when asked for.
    if (!Send(NIM_ADD, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP)) {
        // Left over from a crashed run with the same hwnd/id, or Explorer not ready yet: modify instead.
        if (!Send(NIM_MODIFY, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP)) {
            CT_LOG_WARN("Shell_NotifyIconW(NIM_ADD) failed");
            m_added = false;
            return false;
        }
    }
    m_added = true;
    Send(NIM_SETVERSION, 0);
    return true;
}

void TrayIcon::SetIcon(HICON icon) {
    m_icon = icon;
    if (m_added) Send(NIM_MODIFY, NIF_ICON);
}

void TrayIcon::SetTip(const wchar_t* tip) {
    wcsncpy_s(m_tip, tip ? tip : L"", _TRUNCATE);
    if (m_added) Send(NIM_MODIFY, NIF_TIP | NIF_SHOWTIP);
}

void TrayIcon::Notify(const wchar_t* title, const wchar_t* text) {
    if (!m_added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title ? title : L"", _TRUNCATE);
    wcsncpy_s(nid.szInfo, text ? text : L"", _TRUNCATE);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = m_icon;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) CT_LOG_WARN("tray notification failed");
}

void TrayIcon::Remove() {
    if (!m_added) return;
    Send(NIM_DELETE, 0);
    m_added = false;
}

TrayIcon::Event TrayIcon::Decode(WPARAM wparam, LPARAM lparam) {
    Event e;
    e.what = LOWORD(lparam);
    e.anchor.x = static_cast<short>(LOWORD(wparam));
    e.anchor.y = static_cast<short>(HIWORD(wparam));
    return e;
}

UINT TrayIcon::ShowMenu(HWND owner, POINT at, const MenuItem* items, size_t count) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    for (size_t i = 0; i < count; ++i) {
        const MenuItem& it = items[i];
        if (it.id == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (!it.enabled) flags |= MF_GRAYED;
        if (it.checked) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, it.id, it.text);
    }
    // Without the foreground switch the menu would not close when the user clicks elsewhere (KB135788).
    SetForegroundWindow(owner);
    UINT align = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    align |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, align | TPM_BOTTOMALIGN, at.x, at.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return cmd;
}

namespace {

struct Dib32 {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    bool Create(int w, int h) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        void* p = nullptr;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!bmp) return false;
        bits = static_cast<uint32_t*>(p);
        old = SelectObject(dc, bmp);
        return true;
    }
    ~Dib32() {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

} // namespace

HICON TrayIcon::CreateRecordingIcon(HICON base, int size_px) {
    const int s = size_px > 0 ? size_px : GetSystemMetrics(SM_CXSMICON);
    if (s <= 0 || s > 256) return nullptr;
    const size_t n = static_cast<size_t>(s) * s;
    // Straight-alpha BGRA result. The base icon is drawn over black and over white; the difference gives its alpha
    // (DrawIconEx does not reliably write alpha into a DIB).
    std::vector<float> r(n, 0), g(n, 0), b(n, 0), a(n, 0);
    if (base) {
        Dib32 black, white;
        if (!black.Create(s, s) || !white.Create(s, s)) return nullptr;
        std::memset(black.bits, 0x00, n * 4);
        std::memset(white.bits, 0xFF, n * 4);
        DrawIconEx(black.dc, 0, 0, base, s, s, 0, nullptr, DI_NORMAL);
        DrawIconEx(white.dc, 0, 0, base, s, s, 0, nullptr, DI_NORMAL);
        GdiFlush();
        for (size_t i = 0; i < n; ++i) {
            const uint32_t pb = black.bits[i], pw = white.bits[i];
            const float bb = static_cast<float>(pb & 0xFF), bg = static_cast<float>((pb >> 8) & 0xFF), br = static_cast<float>((pb >> 16) & 0xFF);
            const float wg = static_cast<float>((pw >> 8) & 0xFF);
            const float al = std::clamp(255.f - (wg - bg), 0.f, 255.f) / 255.f;
            a[i] = al;
            // Over black the pixel is color * alpha (premultiplied); keep it premultiplied for the blend below.
            r[i] = br / 255.f;
            g[i] = bg / 255.f;
            b[i] = bb / 255.f;
        }
    }
    // The dot: record color #FF4D4F, centred in the bottom-right quarter, with a transparent gap ring so it reads
    // on any icon. 4x4 supersampled coverage.
    const float cx = s * 0.70f, cy = s * 0.70f, radius = s * 0.27f, gap = std::max(1.0f, s * 0.07f);
    const float dot_r = 1.0f, dot_g = 0x4D / 255.f, dot_b = 0x4F / 255.f;
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            float cover_dot = 0, cover_gap = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4.f - cx, py = y + (sy + 0.5f) / 4.f - cy;
                    const float d = std::sqrt(px * px + py * py);
                    if (d <= radius) cover_dot += 1.f / 16.f;
                    else if (d <= radius + gap) cover_gap += 1.f / 16.f;
                }
            }
            const size_t i = static_cast<size_t>(y) * s + x;
            const float keep = 1.f - cover_dot - cover_gap;   // part of the base icon that stays
            r[i] = r[i] * keep + dot_r * cover_dot;
            g[i] = g[i] * keep + dot_g * cover_dot;
            b[i] = b[i] * keep + dot_b * cover_dot;
            a[i] = a[i] * keep + cover_dot;
        }
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = s;
    bi.bV5Height = -s;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    for (size_t i = 0; i < n; ++i) {
        const float al = std::clamp(a[i], 0.f, 1.f);
        auto chan = [&](float premul) {
            const float straight = al > 0.f ? std::clamp(premul / al, 0.f, 1.f) : 0.f;   // icons use straight alpha
            return static_cast<uint32_t>(std::lround(straight * 255.f));
        };
        px[i] = (static_cast<uint32_t>(std::lround(al * 255.f)) << 24) | (chan(r[i]) << 16) | (chan(g[i]) << 8) | chan(b[i]);
    }
    GdiFlush();
    std::vector<uint8_t> mask_bits(static_cast<size_t>((s + 15) / 16 * 2) * s, 0);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, mask_bits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = mask ? CreateIconIndirect(&ii) : nullptr;
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    if (!icon) CT_LOG_WARN("CreateIconIndirect failed: %lu", GetLastError());
    return icon;
}

} // namespace ct::platform
