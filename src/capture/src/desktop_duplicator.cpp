#define CT_LOG_CATEGORY "capture"
#include <ct/capture/desktop_duplicator.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>

#include <algorithm>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::capture {

namespace {

bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

Rotation FromDxgi(DXGI_MODE_ROTATION r) {
    switch (r) {
    case DXGI_MODE_ROTATION_ROTATE90: return Rotation::R90;
    case DXGI_MODE_ROTATION_ROTATE180: return Rotation::R180;
    case DXGI_MODE_ROTATION_ROTATE270: return Rotation::R270;
    default: return Rotation::Identity;
    }
}

bool Swaps(Rotation r) { return r == Rotation::R90 || r == Rotation::R270; }

// Maps a desktop-orientation rect (display-local px, desktop size dw x dh) into the native panel texture.
// Inverse of the converter's uv rotation (color_convert.h): R90 (u,v)->(v,1-u), R180 (1-u,1-v), R270 (1-v,u).
void MapToNative(Rotation rot, int dw, int dh, int x, int y, int w, int h, int& nx, int& ny, int& nw, int& nh) {
    switch (rot) {
    case Rotation::R90:  nx = y;               ny = dw - (x + w);  nw = h; nh = w; break;
    case Rotation::R180: nx = dw - (x + w);    ny = dh - (y + h);  nw = w; nh = h; break;
    case Rotation::R270: nx = dh - (y + h);    ny = x;             nw = h; nh = w; break;
    default:             nx = x;               ny = y;             nw = w; nh = h; break;
    }
}

// Consumes one injected result when the counter is positive (test hooks).
bool TakeInjected(std::atomic<uint32_t>& count, const std::atomic<HRESULT>& value, HRESULT& hr) {
    uint32_t c = count.load(std::memory_order_acquire);
    while (c > 0 && !count.compare_exchange_weak(c, c - 1, std::memory_order_acq_rel)) {}
    if (c == 0) return false;
    hr = value.load(std::memory_order_acquire);
    return true;
}

bool IsRetryable(HRESULT hr) {
    return hr == E_ACCESSDENIED || hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
           hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_MODE_CHANGE_IN_PROGRESS ||
           hr == DXGI_ERROR_NOT_FOUND;
}

} // namespace

DesktopDuplicator::~DesktopDuplicator() { Shutdown(); }

HRESULT DesktopDuplicator::Init(platform::D3DDevice& dev, const Config& cfg) {
    Shutdown();
    if (!dev.Device() || !dev.Factory()) return E_UNEXPECTED;
    if (cfg.region.IsEmpty() || (cfg.region.w & 1) || (cfg.region.h & 1)) {
        CT_LOG_ERROR("DesktopDuplicator: region %dx%d must be non-empty and even", cfg.region.w, cfg.region.h);
        return E_INVALIDARG;
    }
    m_dev = &dev;
    m_cfg = cfg;

    // 1. The output, by GDI device name, through a factory that knows the current displays (one hot-plugged after
    //    the device was created is not listed by the device's factory).
    m_factory = CurrentDxgiFactory(dev.Factory());
    if (!m_factory) m_factory = dev.Factory();
    ComPtr<IDXGIAdapter1> found_adapter;
    const HRESULT find_hr = FindOutput(m_factory.Get(), m_output, found_adapter);
    if (FAILED(find_hr)) {
        CT_LOG_ERROR("DesktopDuplicator: no DXGI output for %ls", cfg.display.device_name.c_str());
        return find_hr;
    }
    DXGI_ADAPTER_DESC1 ad{};
    CT_RETURN_IF_FAILED(found_adapter->GetDesc1(&ad));
    m_adapter_luid = ad.AdapterLuid;

    // Cross-adapter: the output is scanned out by another GPU. Duplicate on a second device there and copy through
    // system memory (slow path; the fast path is PX).
    m_cross_adapter = !SameLuid(ad.AdapterLuid, dev.AdapterLuid());
    if (m_cross_adapter) {
        CT_LOG_WARN("DesktopDuplicator: %ls is on another adapter (%ls); using the slow cross-adapter copy",
                    cfg.display.device_name.c_str(), ad.Description);
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        CT_RETURN_IF_FAILED(D3D11CreateDevice(found_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                              m_dup_device.GetAddressOf(), nullptr, m_dup_context.GetAddressOf()));
    } else {
        CT_RETURN_IF_FAILED(dev.Device()->QueryInterface(IID_PPV_ARGS(m_dup_device.GetAddressOf())));
        m_dup_context = dev.Context();
    }

    m_format = DXGI_FORMAT_UNKNOWN;
    CT_RETURN_IF_FAILED(CreateDuplication(true));

    // Dst size: the region in native orientation. Fixed for the session (mode changes letterbox, see header).
    m_size = Swaps(m_rotation) ? SizeI{cfg.region.h, cfg.region.w} : SizeI{cfg.region.w, cfg.region.h};
    ComputeBox();

    if (m_cross_adapter) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = static_cast<UINT>(m_size.w);
        sd.Height = static_cast<UINT>(m_size.h);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = m_format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CT_RETURN_IF_FAILED(m_dup_device->CreateTexture2D(&sd, nullptr, m_staging.GetAddressOf()));
    }
    CT_LOG_INFO("DesktopDuplicator: %ls region %dx%d at (%d,%d), format %s, rotation %s%s", cfg.display.device_name.c_str(),
                cfg.region.w, cfg.region.h, cfg.region.x, cfg.region.y,
                m_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "RGBA16F" : "BGRA8", RotationName(m_rotation),
                m_cross_adapter ? ", cross-adapter" : "");
    return S_OK;
}

HRESULT DesktopDuplicator::FindOutput(IDXGIFactory6* factory, ComPtr<IDXGIOutput1>& output, ComPtr<IDXGIAdapter1>& adapter_out) const {
    output.Reset();
    adapter_out.Reset();
    if (!factory) return E_UNEXPECTED;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, o.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(o->GetDesc(&od)) || m_cfg.display.device_name != od.DeviceName || !od.AttachedToDesktop) continue;
            CT_RETURN_IF_FAILED(o.As(&output));
            adapter_out = adapter;
            return S_OK;
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

HRESULT DesktopDuplicator::RefindOutput() {
    // A fresh factory every time: this runs only while the duplication cannot be re-created (every 250 ms at most).
    ComPtr<IDXGIFactory6> fresh;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fresh.GetAddressOf())))) return E_FAIL;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT hr = FindOutput(fresh.Get(), output, adapter);
    if (FAILED(hr)) return hr;   // DXGI_ERROR_NOT_FOUND: the display is not attached (unplugged, or turned off)
    DXGI_ADAPTER_DESC1 ad{};
    CT_RETURN_IF_FAILED(adapter->GetDesc1(&ad));
    if (!SameLuid(ad.AdapterLuid, m_adapter_luid)) {
        // Now driven by another GPU: the duplication device would have to change. Treated as gone.
        if (!m_failing) CT_LOG_WARN("DesktopDuplicator: %ls moved to another adapter", m_cfg.display.device_name.c_str());
        return DXGI_ERROR_NOT_FOUND;
    }
    m_output = output;
    m_factory = fresh;
    return S_OK;
}

HRESULT DesktopDuplicator::CreateDuplication(bool log_failures) {
    m_dup.Reset();
    // First Init offers FP16 for HDR displays; Reinit asks for the format the session already uses.
    DXGI_FORMAT formats[2];
    UINT count = 0;
    if (m_format != DXGI_FORMAT_UNKNOWN) {
        formats[count++] = m_format;
    } else {
        if (m_cfg.display.hdr && m_cfg.prefer_fp16_when_hdr) formats[count++] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        formats[count++] = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    HRESULT hr = E_NOINTERFACE;
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(m_output.As(&out5))) hr = out5->DuplicateOutput1(m_dup_device.Get(), 0, count, formats, m_dup.GetAddressOf());
    if (hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED || hr == E_INVALIDARG) {
        // Older path (no DPI awareness, Win10 before 1703, some drivers): always BGRA8.
        if (m_format == DXGI_FORMAT_UNKNOWN || m_format == DXGI_FORMAT_B8G8R8A8_UNORM)
            hr = m_output->DuplicateOutput(m_dup_device.Get(), m_dup.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        if (log_failures) CT_LOG_WARN("DuplicateOutput failed: %s", HrToString(hr).c_str());
        m_dup.Reset();
        return hr;
    }

    DXGI_OUTDUPL_DESC desc{};
    m_dup->GetDesc(&desc);
    if (desc.DesktopImageInSystemMemory) {
        CT_LOG_ERROR("DesktopDuplicator: desktop image in system memory is not supported");
        m_dup.Reset();
        return E_NOTIMPL;
    }
    const DXGI_FORMAT got = desc.ModeDesc.Format;
    if (got != DXGI_FORMAT_B8G8R8A8_UNORM && got != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        CT_LOG_ERROR("DesktopDuplicator: unexpected duplication format %d", static_cast<int>(got));
        m_dup.Reset();
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (m_format == DXGI_FORMAT_UNKNOWN) {
        m_format = got;
        m_rotation = FromDxgi(desc.Rotation);
    } else if (got != m_format) {
        if (log_failures) CT_LOG_WARN("DesktopDuplicator: format changed to %d; retrying", static_cast<int>(got));
        m_dup.Reset();
        return DXGI_ERROR_UNSUPPORTED;
    }
    m_cur_rotation = FromDxgi(desc.Rotation);
    m_desktop_native = SizeI{static_cast<int32_t>(desc.ModeDesc.Width), static_cast<int32_t>(desc.ModeDesc.Height)};
    return S_OK;
}

void DesktopDuplicator::ComputeBox() {
    // Desktop-orientation size of the duplicated output and the region in display-local px.
    const int dw = Swaps(m_rotation) ? m_desktop_native.h : m_desktop_native.w;
    const int dh = Swaps(m_rotation) ? m_desktop_native.w : m_desktop_native.h;
    const RectI full{m_cfg.region.x - m_cfg.display.rect.x, m_cfg.region.y - m_cfg.display.rect.y, m_cfg.region.w,
                     m_cfg.region.h};
    const RectI vis = full.Intersect(RectI{0, 0, dw, dh});
    int fx, fy, fw, fh;
    MapToNative(m_rotation, dw, dh, full.x, full.y, full.w, full.h, fx, fy, fw, fh);
    m_partial = vis.w != full.w || vis.h != full.h;
    if (vis.IsEmpty()) {
        m_box = D3D11_BOX{0, 0, 0, 0, 0, 1};
        m_dst_x = m_dst_y = 0;
    } else {
        int vx, vy, vw, vh;
        MapToNative(m_rotation, dw, dh, vis.x, vis.y, vis.w, vis.h, vx, vy, vw, vh);
        m_box = D3D11_BOX{static_cast<UINT>(vx), static_cast<UINT>(vy), 0, static_cast<UINT>(vx + vw),
                          static_cast<UINT>(vy + vh), 1};
        // Rotation is rigid, so the visible part keeps its offset inside the full region's native box.
        m_dst_x = static_cast<UINT>(std::clamp(vx - fx, 0, m_size.w));
        m_dst_y = static_cast<UINT>(std::clamp(vy - fy, 0, m_size.h));
    }
    if (m_partial)
        CT_LOG_WARN("DesktopDuplicator: region %dx%d only partly inside the %dx%d desktop; letterboxing", full.w, full.h, dw, dh);
}

HRESULT DesktopDuplicator::ClearDst(ID3D11Texture2D* dst) {
    ClearView* slot = nullptr;
    for (ClearView& cv : m_clear_views)
        if (cv.tex == dst) slot = &cv;
    if (!slot) {
        slot = &m_clear_views[m_clear_next];
        m_clear_next = (m_clear_next + 1) % static_cast<int>(std::size(m_clear_views));
        slot->tex = nullptr;
        CT_RETURN_IF_FAILED(m_dev->Device()->CreateRenderTargetView(dst, nullptr, slot->rtv.ReleaseAndGetAddressOf()));
        slot->tex = dst;
    }
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    m_dev->Context()->ClearRenderTargetView(slot->rtv.Get(), black);
    return S_OK;
}

HRESULT DesktopDuplicator::AcquireFrame(uint32_t timeout_ms, ComPtr<ID3D11Texture2D>& tex, int64_t& present_qpc) {
    if (!m_dup) return DXGI_ERROR_ACCESS_LOST;
    HRESULT hr;
    if (TakeInjected(m_inject_acquire_count, m_inject_acquire_hr, hr)) [[unlikely]] {
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
    } else {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        hr = m_dup->AcquireNextFrame(timeout_ms, &info, resource.GetAddressOf());
        if (hr == S_OK) return AcceptFrame(info, resource, tex, present_qpc);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) return hr;   // do not ReleaseFrame
    if (hr == DXGI_ERROR_INVALID_CALL) {
        // Some drivers report a duplication invalidated by a mode change this way; re-creating it recovers.
        if (!m_invalid_call_logged) CT_LOG_WARN("AcquireNextFrame returned DXGI_ERROR_INVALID_CALL; re-creating the duplication");
        m_invalid_call_logged = true;
        return DXGI_ERROR_ACCESS_LOST;
    }
    LogHr(hr, "AcquireNextFrame", __FILE__, __LINE__);
    return hr;
}

HRESULT DesktopDuplicator::AcceptFrame(const DXGI_OUTDUPL_FRAME_INFO& info, ComPtr<IDXGIResource>& resource,
                                       ComPtr<ID3D11Texture2D>& tex, int64_t& present_qpc) {
    HRESULT hr = S_OK;
    if (info.LastPresentTime.QuadPart == 0) {      // only the pointer moved
        m_dup->ReleaseFrame();
        return S_FALSE;
    }
    if (info.AccumulatedFrames > 1) [[unlikely]] {
        static ct::Stats::Counter& coalesced = ct::Stats::C("capture.coalesced");
        coalesced.Add(info.AccumulatedFrames - 1);
    }
    hr = resource.As(&tex);
    if (FAILED(hr)) {
        m_dup->ReleaseFrame();
        LogHr(hr, "desktop resource As(ID3D11Texture2D)", __FILE__, __LINE__);
        return hr;
    }
    // The box must lie inside the source or CopySubresourceRegion silently does nothing: re-check the size.
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (static_cast<int32_t>(td.Width) != m_desktop_native.w || static_cast<int32_t>(td.Height) != m_desktop_native.h) [[unlikely]] {
        m_desktop_native = SizeI{static_cast<int32_t>(td.Width), static_cast<int32_t>(td.Height)};
        ComputeBox();
    }
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) [[unlikely]] {
        if (m_direct_ok) CT_LOG_INFO("DesktopDuplicator: desktop image is not a shader resource; using the copy path");
        m_direct_ok = false;
    }
    present_qpc = info.LastPresentTime.QuadPart;
    return S_OK;
}

HRESULT DesktopDuplicator::AcquireInto(ID3D11Texture2D* dst, uint32_t timeout_ms, int64_t& present_qpc) {
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = AcquireFrame(timeout_ms, tex, present_qpc);
    if (hr != S_OK) return hr;
    if (m_cross_adapter) {
        hr = CopyCrossAdapter(tex.Get(), dst);
    } else {
        if (m_partial) hr = ClearDst(dst);
        if (SUCCEEDED(hr) && m_box.right > m_box.left)
            m_dev->Context()->CopySubresourceRegion(dst, 0, m_dst_x, m_dst_y, 0, tex.Get(), 0, &m_box);
    }
    m_dup->ReleaseFrame();
    if (FAILED(hr)) {
        LogHr(hr, "DesktopDuplicator copy", __FILE__, __LINE__);
        return hr;
    }
    return S_OK;
}

bool DesktopDuplicator::CanAcquireDirect() const {
    return m_dup && m_direct_ok && !m_cross_adapter && (NeedsFit() || (!m_partial && m_box.right > m_box.left));
}

bool DesktopDuplicator::NeedsFit() const {
    if (!m_cfg.fit_full_display || m_desktop_native.IsEmpty()) return false;
    if (m_cur_rotation != m_rotation) return true;
    const int dw = Swaps(m_cur_rotation) ? m_desktop_native.h : m_desktop_native.w;
    const int dh = Swaps(m_cur_rotation) ? m_desktop_native.w : m_desktop_native.h;
    // The region of a whole-display recording is the display rounded down to even sizes.
    return (dw & ~1) != m_cfg.region.w || (dh & ~1) != m_cfg.region.h;
}

HRESULT DesktopDuplicator::AcquireDirect(uint32_t timeout_ms, ID3D11Texture2D*& desktop, D3D11_BOX& src_box,
                                         int64_t& present_qpc) {
    ComPtr<ID3D11Texture2D> tex;
    const HRESULT hr = AcquireFrame(timeout_ms, tex, present_qpc);
    if (hr != S_OK) return hr;
    if (!CanAcquireDirect()) {   // learned on this frame (mode change, no SRV binding): caller switches to AcquireInto
        m_dup->ReleaseFrame();
        return E_NOTIMPL;
    }
    m_held = tex;
    desktop = tex.Get();
    src_box = NeedsFit() ? D3D11_BOX{0, 0, 0, static_cast<UINT>(m_desktop_native.w), static_cast<UINT>(m_desktop_native.h), 1}
                         : m_box;
    return S_OK;
}

void DesktopDuplicator::ReleaseDirect() {
    if (!m_held) return;
    m_held.Reset();
    m_dup->ReleaseFrame();
}

HRESULT DesktopDuplicator::CopyCrossAdapter(ID3D11Texture2D* desktop, ID3D11Texture2D* dst) {
    // Copy on the duplication device into staging, map (waits for that GPU), upload on the main device.
    if (m_box.right > m_box.left)
        m_dup_context->CopySubresourceRegion(m_staging.Get(), 0, m_dst_x, m_dst_y, 0, desktop, 0, &m_box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CT_RETURN_IF_FAILED(m_dup_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    if (m_partial) {
        // Staging keeps old pixels outside the box: clear them on the CPU (slow path only).
        const UINT bpp = m_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        auto* base = static_cast<uint8_t*>(mapped.pData);
        const UINT bw = m_box.right - m_box.left, bh = m_box.bottom - m_box.top;
        for (int y = 0; y < m_size.h; ++y) {
            uint8_t* row = base + static_cast<size_t>(y) * mapped.RowPitch;
            const bool in_rows = static_cast<UINT>(y) >= m_dst_y && static_cast<UINT>(y) < m_dst_y + bh;
            if (!in_rows) {
                std::memset(row, 0, static_cast<size_t>(m_size.w) * bpp);
                continue;
            }
            std::memset(row, 0, static_cast<size_t>(m_dst_x) * bpp);
            const UINT right = m_dst_x + bw;
            if (right < static_cast<UINT>(m_size.w)) std::memset(row + right * bpp, 0, (m_size.w - right) * bpp);
        }
    }
    m_dev->Context()->UpdateSubresource(dst, 0, nullptr, mapped.pData, mapped.RowPitch, 0);
    m_dup_context->Unmap(m_staging.Get(), 0);
    return S_OK;
}

HRESULT DesktopDuplicator::Reinit() {
    if (!m_output) return E_UNEXPECTED;
    m_held.Reset();
    m_dup.Reset();
    HRESULT hr = S_OK;
    if (m_sim_removed.load(std::memory_order_acquire)) [[unlikely]] {
        hr = DXGI_ERROR_NOT_FOUND;
    } else if (!TakeInjected(m_inject_reinit_count, m_inject_reinit_hr, hr)) {
        // The display configuration changed since the output was found (hot-plug, topology): the output object may be
        // stale even if DuplicateOutput still succeeds on it, so look it up again first.
        if (!m_factory || !m_factory->IsCurrent()) hr = RefindOutput();
        if (SUCCEEDED(hr)) {
            hr = CreateDuplication(!m_failing);
            // A failure other than the secure desktop's E_ACCESSDENIED may be a stale output: retry on a fresh one.
            if (FAILED(hr) && hr != E_ACCESSDENIED && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
                const HRESULT fr = RefindOutput();
                if (SUCCEEDED(fr)) hr = CreateDuplication(false);
                else if (fr == DXGI_ERROR_NOT_FOUND) hr = fr;
            }
        }
    }
    if (FAILED(hr)) {
        m_dup.Reset();
        if (!m_failing && IsRetryable(hr)) CT_LOG_WARN("DesktopDuplicator: duplication unavailable (%s); retrying", HrToString(hr).c_str());
        m_failing = true;
        return hr;
    }
    ComputeBox();
    if (m_failing) CT_LOG_INFO("DesktopDuplicator: duplication restored (%dx%d, rotation %s)", m_desktop_native.w,
                               m_desktop_native.h, RotationName(m_cur_rotation));
    m_failing = false;
    return S_OK;
}

void DesktopDuplicator::ReleaseDuplication() {
    m_held.Reset();
    m_dup.Reset();
}

void DesktopDuplicator::InjectAcquireErrorForTest(HRESULT hr, uint32_t count) {
    m_inject_acquire_hr.store(hr, std::memory_order_release);
    m_inject_acquire_count.store(count, std::memory_order_release);
}

void DesktopDuplicator::InjectReinitErrorForTest(HRESULT hr, uint32_t count) {
    m_inject_reinit_hr.store(hr, std::memory_order_release);
    m_inject_reinit_count.store(count, std::memory_order_release);
}

void DesktopDuplicator::SimulateOutputRemovedForTest(bool removed) { m_sim_removed.store(removed, std::memory_order_release); }

void DesktopDuplicator::Shutdown() {
    m_held.Reset();
    m_dup.Reset();
    m_output.Reset();
    m_factory.Reset();
    m_staging.Reset();
    for (ClearView& cv : m_clear_views) {
        cv.tex = nullptr;
        cv.rtv.Reset();
    }
    m_dup_context.Reset();
    m_dup_device.Reset();
    m_dev = nullptr;
    m_cross_adapter = false;
    m_failing = false;
}

} // namespace ct::capture
