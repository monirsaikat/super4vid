#pragma once
// ct/platform/tray_icon.h
// Notification-area icon (Shell_NotifyIconW, NOTIFYICON_VERSION_4): icon, tooltip, balloon notifications, a context
// menu helper, the runtime "recording" icon (red dot over the app icon) and re-adding after Explorer restarts.
// Thread model: UI thread only (the callback messages arrive in the owner window's procedure).
// Lifetime: the destructor removes the icon. Icons passed to Add/SetIcon stay owned by the caller.

#include <ct/platform/win.h>

#include <cstddef>
#include <cstdint>

namespace ct::platform {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // callback_msg (e.g. WM_APP + 1) is sent to hwnd with the NOTIFYICON_VERSION_4 layout; use Decode().
    bool Add(HWND hwnd, HICON icon, const wchar_t* tip, UINT callback_msg);
    void SetIcon(HICON icon);
    void SetTip(const wchar_t* tip);                         // truncated to 127 characters
    void Notify(const wchar_t* title, const wchar_t* text);  // balloon / toast ("Saved" while the bar is hidden)
    void Remove();
    bool IsAdded() const { return m_added; }
    // Explorer restarted: the owner window receives TaskbarCreatedMessage() and calls Readd().
    bool Readd();
    static UINT TaskbarCreatedMessage();

    struct Event {
        UINT what = 0;      // WM_LBUTTONUP, WM_RBUTTONUP, WM_CONTEXTMENU, NIN_SELECT, NIN_KEYSELECT, WM_MOUSEMOVE, ...
        POINT anchor{};     // screen position for menus (NOTIFYICON_VERSION_4 passes it in wParam)
    };
    static Event Decode(WPARAM wparam, LPARAM lparam);

    struct MenuItem {
        UINT id = 0;               // 0 = separator
        const wchar_t* text = nullptr;
        bool enabled = true;
        bool checked = false;
    };
    // SetForegroundWindow(owner) + TrackPopupMenuEx(TPM_RETURNCMD) so the menu closes when the user clicks elsewhere.
    // Returns the chosen id or 0.
    static UINT ShowMenu(HWND owner, POINT at, const MenuItem* items, size_t count);

    // The app icon with a red (#FF4D4F) anti-aliased dot in the bottom-right quarter, drawn at runtime into a
    // 32-bit DIB (GDI for the base icon, coverage-blended pixels for the dot) and turned into an icon with
    // CreateIconIndirect. size_px = 0 uses SM_CXSMICON. The caller destroys the result with DestroyIcon.
    static HICON CreateRecordingIcon(HICON base, int size_px = 0);

private:
    bool Send(DWORD message, UINT flags);

    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    UINT m_callback = 0;
    wchar_t m_tip[128] = {};
    bool m_added = false;
    static constexpr UINT kId = 1;
};

} // namespace ct::platform
