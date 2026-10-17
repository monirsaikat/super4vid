#pragma once
// ct/capture/desktop_duplicator.h
// DXGI Desktop Duplication of one output, cropped to a region on the GPU (CopySubresourceRegion into the caller's
// texture). No CPU pixel copies, except on the cross-adapter slow path (the output belongs to another GPU).
// Thread model: one owner thread (ct-capture) after Init; the caller holds D3DDevice::Lock around AcquireInto and
// Reinit. Only one IDXGIOutputDuplication may exist per output per process: Reinit recreates, never duplicates twice.
// Lifetime: Init -> AcquireInto* (Reinit on ACCESS_LOST) -> Shutdown (also in the destructor).
//
// Size() is the crop size in native panel orientation (width/height swapped for R90/R270); the ColorConverter
// rotates back. If the display mode changes so the region no longer fits, the visible part is copied to the top-left
// of dst and the rest is cleared to black (the stream keeps its size; docs/03 section 10).

#include <ct/capture/display.h>
#include <ct/platform/d3d_device.h>

#include <cstdint>

namespace ct::capture {

class DesktopDuplicator {
public:
    struct Config {
        DisplayInfo display;
        RectI region;                    // virtual-screen px, already clamped & even
        bool prefer_fp16_when_hdr = true;
    };

    DesktopDuplicator() = default;
    ~DesktopDuplicator();
    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    HRESULT Init(platform::D3DDevice&, const Config&);
    // Acquire a new desktop image into dst (same size as Size(), format Format(), bound as render target so it can be
    // cleared). Caller holds D3DDevice::Lock during the call.
    //   S_OK   = dst updated, present_qpc set
    //   S_FALSE= no new image within timeout (or pointer-only update); dst untouched
    //   DXGI_ERROR_ACCESS_LOST = caller must call Reinit() (keep using the previous frame meanwhile)
    //   other failures = fatal for this session
    HRESULT AcquireInto(ID3D11Texture2D* dst, uint32_t timeout_ms, int64_t& present_qpc);
    // Zero-copy variant: on S_OK, `desktop` is the duplicated image and `src_box` the region inside it (native
    // orientation, Size()). The frame stays acquired until ReleaseDirect(), which the caller calls in the same tick right
    // after queuing its GPU work (ScreenCapturer converts straight from it). Same return codes as AcquireInto, plus
    // E_NOTIMPL when this frame showed the direct path is not possible (frame released; use AcquireInto from now on).
    HRESULT AcquireDirect(uint32_t timeout_ms, ID3D11Texture2D*& desktop, D3D11_BOX& src_box, int64_t& present_qpc);
    void ReleaseDirect();
    // Same device, region fully inside the desktop (no letterbox), desktop image bindable as a shader resource.
    bool CanAcquireDirect() const;
    // Releases and re-creates the duplication. E_ACCESSDENIED (or DXGI_ERROR_UNSUPPORTED / NOT_CURRENTLY_AVAILABLE)
    // while a secure desktop is up: keep retrying every 250 ms. Logs the first failure of a streak only.
    HRESULT Reinit();
    DXGI_FORMAT Format() const { return m_format; }   // B8G8R8A8_UNORM or R16G16B16A16_FLOAT
    SizeI Size() const { return m_size; }
    Rotation DisplayRotation() const { return m_rotation; }
    bool IsCrossAdapter() const { return m_cross_adapter; }
    void Shutdown();

private:
    HRESULT CreateDuplication(bool log_failures);
    HRESULT AcquireFrame(uint32_t timeout_ms, Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex, int64_t& present_qpc);
    void ComputeBox();
    HRESULT CopyCrossAdapter(ID3D11Texture2D* desktop, ID3D11Texture2D* dst);
    HRESULT ClearDst(ID3D11Texture2D* dst);

    platform::D3DDevice* m_dev = nullptr;
    Config m_cfg;
    Microsoft::WRL::ComPtr<IDXGIOutput1> m_output;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dup;
    // Device the duplication runs on: the main device, or a second one on the output's adapter (cross-adapter).
    Microsoft::WRL::ComPtr<ID3D11Device> m_dup_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_dup_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;   // cross-adapter: cropped readback on the second device
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_held;      // AcquireDirect frame until ReleaseDirect
    struct ClearView {
        ID3D11Texture2D* tex = nullptr;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    };
    ClearView m_clear_views[4];
    int m_clear_next = 0;
    D3D11_BOX m_box{};           // crop box in the duplicated (native orientation) texture
    UINT m_dst_x = 0, m_dst_y = 0;   // where the box lands in dst (non-zero only when letterboxing)
    SizeI m_size{};              // dst size (native orientation)
    SizeI m_desktop_native{};    // duplicated texture size
    bool m_partial = false;      // box smaller than m_size after a mode change: clear dst before copying
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    Rotation m_rotation = Rotation::Identity;
    bool m_cross_adapter = false;
    bool m_failing = false;      // a Reinit failure streak is in progress (log once)
    bool m_direct_ok = true;     // cleared when the desktop image cannot be bound as a shader resource
};

} // namespace ct::capture
