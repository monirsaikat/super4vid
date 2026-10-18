#pragma once
// ct/capture/display.h
// The monitor model: every active display with its virtual-screen rect, DPI, refresh rate, HDR state, rotation and
// the DXGI adapter/output that drives it; plus region helpers.
// Thread model: all functions are free functions without shared state; EnumerateDisplays makes blocking display
// configuration calls (a few ms), so the UI calls it from ct::Async or once at startup.
// Coordinates are physical pixels; the process must be per-monitor DPI aware (the app manifest does that).

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace ct::capture {

enum class Rotation { Identity, R90, R180, R270 };

struct DisplayInfo {
    int index = 0;                      // position in the EnumerateDisplays result (primary is 0)
    std::wstring device_name;           // "\\.\DISPLAY1"
    std::string friendly_name;          // "DELL U2723QE" (EDID name; empty for some built-in panels)
    RectI rect, work_rect;              // virtual-screen px
    UINT dpi = 96;
    bool primary = false;
    UINT refresh_hz = 60;
    bool hdr = false;
    float sdr_white_nits = 80.f;
    Rotation rotation = Rotation::Identity;
    LUID adapter_luid{};
    UINT output_index = 0;              // IDXGIAdapter::EnumOutputs index on adapter_luid
    HMONITOR hmonitor = nullptr;
};

// Primary display first, then the others sorted left to right, top to bottom. Displays that DXGI does not report
// (no output found by device name) keep adapter_luid zero and cannot be duplicated.
std::vector<DisplayInfo> EnumerateDisplays(IDXGIFactory6* factory);
RectI VirtualScreenRect();                       // SM_XVIRTUALSCREEN .. SM_CYVIRTUALSCREEN
int DisplayIndexAt(const std::vector<DisplayInfo>&, PointI p);   // -1 when no display contains p
// Intersects region with the display rect, rounds width/height down to even numbers and enforces a 64x64 minimum
// (grown towards the bottom-right, shifted back inside the display if needed). An empty region, or one that does not
// intersect the display, means the whole display.
RectI ClampRegionToDisplay(const DisplayInfo&, RectI region);

const char* RotationName(Rotation r);

// A DXGI factory that knows the current display configuration: null when `factory` is still current (use it), else a
// newly created factory. A factory made before a monitor was plugged in, removed or re-arranged keeps listing the
// outputs it saw at creation (IDXGIFactory1::IsCurrent() is false), so hot-plugged monitors are only found through a
// fresh one: https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent
Microsoft::WRL::ComPtr<IDXGIFactory6> CurrentDxgiFactory(IDXGIFactory6* factory);

} // namespace ct::capture
