#define CT_LOG_CATEGORY "capture"
#include <ct/capture/display.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace ct::capture {

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR hmon, HDC, LPRECT, LPARAM user) {
    reinterpret_cast<std::vector<HMONITOR>*>(user)->push_back(hmon);
    return TRUE;
}

RectI FromRect(const RECT& r) { return RectI{r.left, r.top, r.right - r.left, r.bottom - r.top}; }

Rotation FromDxgi(DXGI_MODE_ROTATION r) {
    switch (r) {
    case DXGI_MODE_ROTATION_ROTATE90: return Rotation::R90;
    case DXGI_MODE_ROTATION_ROTATE180: return Rotation::R180;
    case DXGI_MODE_ROTATION_ROTATE270: return Rotation::R270;
    default: return Rotation::Identity;
    }
}

// Fills adapter/output/rotation/HDR for each display by matching DXGI_OUTPUT_DESC1.DeviceName.
void MatchDxgi(IDXGIFactory6* factory, std::vector<DisplayInfo>& displays) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        if (FAILED(adapter->GetDesc1(&ad))) continue;
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> out6;
            DXGI_OUTPUT_DESC1 od{};
            if (FAILED(output.As(&out6)) || FAILED(out6->GetDesc1(&od))) continue;
            for (DisplayInfo& d : displays) {
                if (d.device_name != od.DeviceName) continue;
                d.adapter_luid = ad.AdapterLuid;
                d.output_index = o;
                d.rotation = FromDxgi(od.Rotation);
                d.hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
        }
    }
}

// Friendly (EDID) names and SDR white levels from the display configuration API, matched by GDI device name.
void MatchDisplayConfig(std::vector<DisplayInfo>& displays) {
    UINT32 path_count = 0, mode_count = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return;
        paths.resize(path_count);
        modes.resize(mode_count);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    }
    if (rc != ERROR_SUCCESS) {
        CT_LOG_WARN("QueryDisplayConfig failed: %ld", rc);
        return;
    }
    paths.resize(path_count);
    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        DisplayInfo* match = nullptr;
        for (DisplayInfo& d : displays)
            if (d.device_name == source.viewGdiDeviceName) match = &d;
        if (!match) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = p.targetInfo.adapterId;
        target.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0])
            match->friendly_name = WideToUtf8(target.monitorFriendlyDeviceName);

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = p.targetInfo.adapterId;
        white.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            match->sdr_white_nits = static_cast<float>(white.SDRWhiteLevel / 1000.0 * 80.0);
    }
}

} // namespace

std::vector<DisplayInfo> EnumerateDisplays(IDXGIFactory6* factory) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors));

    std::vector<DisplayInfo> displays;
    displays.reserve(monitors.size());
    for (HMONITOR hmon : monitors) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hmon, &mi)) continue;
        DisplayInfo d;
        d.hmonitor = hmon;
        d.device_name = mi.szDevice;
        d.friendly_name = WideToUtf8(mi.szDevice);
        d.rect = FromRect(mi.rcMonitor);
        d.work_rect = FromRect(mi.rcWork);
        d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        UINT dpi_x = 96, dpi_y = 96;
        if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) d.dpi = dpi_x;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            d.refresh_hz = dm.dmDisplayFrequency;
        displays.push_back(std::move(d));
    }

    ComPtr<IDXGIFactory6> own;
    if (!factory && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(own.GetAddressOf())))) factory = own.Get();
    if (factory) MatchDxgi(factory, displays);
    MatchDisplayConfig(displays);

    std::stable_sort(displays.begin(), displays.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.x != b.rect.x) return a.rect.x < b.rect.x;
        return a.rect.y < b.rect.y;
    });
    for (size_t i = 0; i < displays.size(); ++i) displays[i].index = static_cast<int>(i);
    return displays;
}

RectI VirtualScreenRect() {
    return RectI{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

int DisplayIndexAt(const std::vector<DisplayInfo>& displays, PointI p) {
    for (const DisplayInfo& d : displays)
        if (d.rect.Contains(p)) return d.index;
    return -1;
}

RectI ClampRegionToDisplay(const DisplayInfo& display, RectI region) {
    const RectI& dr = display.rect;
    RectI r = region.IsEmpty() ? dr : region.Intersect(dr);
    if (r.IsEmpty()) r = dr;
    r.w &= ~1;
    r.h &= ~1;
    constexpr int32_t kMin = 64;
    if (r.w < kMin) r.w = std::min<int32_t>(kMin, dr.w & ~1);
    if (r.h < kMin) r.h = std::min<int32_t>(kMin, dr.h & ~1);
    if (r.Right() > dr.Right()) r.x = dr.Right() - r.w;
    if (r.Bottom() > dr.Bottom()) r.y = dr.Bottom() - r.h;
    return r;
}

const char* RotationName(Rotation r) {
    switch (r) {
    case Rotation::R90: return "90";
    case Rotation::R180: return "180";
    case Rotation::R270: return "270";
    default: return "0";
    }
}

} // namespace ct::capture
