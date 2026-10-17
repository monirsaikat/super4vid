#pragma once
// Helpers shared by the capture device tests: a solid-colour topmost window and staging readbacks.

#include <ct/platform/d3d_device.h>
#include <ct/platform/win.h>

#include <cstdint>
#include <vector>

namespace ct::test_util {

// A topmost, non-activating popup filled with `color` (COLORREF), shown and painted. Destroyed by the destructor.
class SolidWindow {
public:
    SolidWindow(int x, int y, int w, int h, COLORREF color);
    ~SolidWindow();
    SolidWindow(const SolidWindow&) = delete;
    SolidWindow& operator=(const SolidWindow&) = delete;
    HWND Hwnd() const { return m_hwnd; }
    void SetColor(COLORREF color);   // repaints
    void Pump();                     // dispatches pending messages

private:
    HWND m_hwnd = nullptr;
    HBRUSH m_brush = nullptr;
};

// Copies a texture (single subresource, or one NV12 texture) into CPU memory through a staging texture.
// rows are tightly packed: width * bytes-per-pixel (NV12: Y plane w*h, then UV plane w*h/2).
bool ReadbackTexture(platform::D3DDevice& dev, ID3D11Texture2D* tex, std::vector<uint8_t>& out, UINT& width, UINT& height);

} // namespace ct::test_util
