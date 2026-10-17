#pragma once
// ct/platform/layered_window.h
// A per-pixel-alpha top-level popup (WS_EX_LAYERED) drawn with Direct2D and pushed to the screen from a 32-bit
// premultiplied top-down DIB with UpdateLayeredWindowIndirect, optionally for a dirty rectangle only, so large
// overlays stay cheap. D2D renders in software into a WIC bitmap; only the dirty rows are copied into the DIB.
// (An ID2D1DCRenderTarget over the DIB costs ~30 ms per EndDraw at 3840x1080 whatever the dirty size, because it
// copies its whole surface to the DC; it remains the fallback when WIC is unavailable, e.g. COM not initialized.)
// Used for the region overlay, the countdown and the recording frame (all excluded from capture).
// Thread model: create, draw and destroy on the UI thread (the thread that pumps its messages).
// Lifetime: the HWND lives from Create() until Destroy() (also called by the destructor).
// Note: pixels with alpha 0 are not hit-testable; an interactive overlay keeps alpha >= 1 where it wants clicks.

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace ct::platform {

class LayeredWindow {
public:
    // Same contract as Window::MessageHandler: set handled = true to return the result instead of the default.
    using MessageHandler = LRESULT (*)(void* user, HWND, UINT, WPARAM, LPARAM, bool& handled);

    LayeredWindow() = default;
    ~LayeredWindow();
    LayeredWindow(const LayeredWindow&) = delete;
    LayeredWindow& operator=(const LayeredWindow&) = delete;

    // virtual_px: window rect in virtual-screen physical pixels. click_through adds WS_EX_TRANSPARENT |
    // WS_EX_NOACTIVATE (mouse goes to the windows below). exclude_from_capture applies WDA_EXCLUDEFROMCAPTURE
    // (failure is logged and ignored: Windows 10 < 2004 and some builds refuse it for layered windows).
    // The window is created hidden.
    bool Create(const RectI& virtual_px, bool click_through, bool topmost, bool exclude_from_capture);
    void SetHandler(MessageHandler handler, void* user) { m_handler = handler; m_user = user; }

    // Starts drawing. dirty (window-relative px) limits clearing, drawing (an axis-aligned clip) and the later
    // present; nullptr = the whole window. The area is cleared to transparent. Units are physical pixels.
    // Returns nullptr when the target is unavailable (nothing to end then).
    ID2D1RenderTarget* BeginDraw(const RectI* dirty = nullptr);
    // Ends drawing and pushes the dirty area with the given constant alpha (multiplied with per-pixel alpha).
    void EndDrawAndPresent(BYTE alpha = 255);
    // Re-pushes the current pixels with a new constant alpha (fades) without drawing.
    void Present(BYTE alpha);

    HWND Hwnd() const { return m_hwnd; }
    RectI Rect() const { return m_rect; }
    ID2D1Factory* Factory() const { return m_factory.Get(); }
    bool ExcludedFromCapture() const { return m_excluded; }
    double LastDrawMs() const { return m_last_draw_ms; }   // BeginDraw -> present done (diagnostics)
    // Copies the surface as straight-alpha RGBA rows (tests and visual checks: the window itself is excluded from
    // screenshots). False when there is no surface.
    bool ReadPixelsRgba(std::vector<uint8_t>& out) const;
    void Move(const RectI& virtual_px);   // resizes the surface when the size changed (next present is full)
    void Show(bool show, bool activate = false);
    bool IsVisible() const;
    void Destroy();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateSurface();
    void ReleaseSurface();
    bool EnsureTarget();
    void PushToScreen(BYTE alpha, const RECT* dirty);
    void CopyToDib(const RECT& r);

    HWND m_hwnd = nullptr;
    RectI m_rect{};
    bool m_excluded = false;
    MessageHandler m_handler = nullptr;
    void* m_user = nullptr;

    HDC m_dc = nullptr;
    HBITMAP m_bmp = nullptr;
    HGDIOBJ m_old = nullptr;
    void* m_bits = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> m_factory;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> m_target;
    Microsoft::WRL::ComPtr<IWICImagingFactory> m_wic_factory;
    Microsoft::WRL::ComPtr<IWICBitmap> m_wic;   // render surface when WIC is available (else nullptr: DC target)
    bool m_drawing = false;
    bool m_clipped = false;
    bool m_need_full = true;   // the first present (and the first after a resize) must cover the whole surface
    RECT m_dirty{};
    int64_t m_draw_start_qpc = 0;
    double m_last_draw_ms = 0;
    BYTE m_alpha = 255;
};

} // namespace ct::platform
