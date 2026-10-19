#pragma once
// ct/platform/window.h
// Thin Win32 top-level window: Per-Monitor-V2 DPI handling, size tracking, DWM attributes.
// Thread model: create, use and destroy on the thread that pumps its messages (the UI thread).
// Lifetime: the HWND lives from Create() until Destroy() or WM_DESTROY. The object must outlive the HWND.

#include <ct/core/types.h>
#include <ct/platform/win.h>

namespace ct::platform {

class Window {
public:
    struct Desc {
        const wchar_t* class_name = L"super4vidWindow";
        const wchar_t* title = L"super4vid";
        int w_dip = 1280, h_dip = 800;       // client size in DIPs
        bool frameless = false;
        bool topmost = false;
        bool tool_window = false;
        bool resizable = true;
        HICON icon = nullptr;
        // Additions to the P00 spec (see DECISIONS.md):
        bool quit_on_destroy = false;        // main window: WM_DESTROY posts WM_QUIT
        SizeI min_client_dip{};              // WM_GETMINMAXINFO minimum client size (editor: 640x400)
    };
    // Called for every message after the window updated its own state. Set handled=true to return the result
    // instead of the default processing. ImGui_ImplWin32_WndProcHandler belongs at the top of this handler.
    using MessageHandler = LRESULT (*)(void* user, HWND, UINT, WPARAM, LPARAM, bool& handled);

    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const Desc&, MessageHandler, void* user);
    HWND Hwnd() const { return m_hwnd; }
    UINT Dpi() const { return m_dpi; }
    float DpiScale() const { return static_cast<float>(m_dpi) / 96.0f; }
    SizeI ClientSizePx() const;
    bool IsMinimized() const { return m_minimized; }
    // True once after each WM_SIZE that changed the client size; the render loop resizes the swapchain.
    bool ConsumeResize();

    void Show(int cmd = SW_SHOW);
    void SetTitle(const wchar_t*);
    void SetDarkTitleBar(bool);
    void SetRoundedCorners(bool);
    void SetExcludeFromCapture(bool);           // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE); ignore failure on < 2004
    void Destroy();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    SizeI m_client_px{};
    SizeI m_min_client_dip{};
    DWORD m_style = 0;
    DWORD m_ex_style = 0;
    bool m_resize_pending = false;
    bool m_minimized = false;
    bool m_quit_on_destroy = false;
    MessageHandler m_handler = nullptr;
    void* m_user = nullptr;
};

} // namespace ct::platform
