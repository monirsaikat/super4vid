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
// of dst and the rest is cleared to black (the stream keeps its size; docs/03 section 10). With
// Config::fit_full_display (a whole-display recording) NeedsFit() reports a desktop whose size or rotation no longer
// matches the stream; the capturer then scales the whole desktop image into the stream, letterboxed (ColorConverter::
// ConvertFit), from AcquireDirect.
// Reinit re-finds the output by GDI device name through a current DXGI factory when the old output object stops
// working (monitor unplugged and plugged back, display topology change) and returns DXGI_ERROR_NOT_FOUND while the
// display is gone. DXGI_ERROR_INVALID_CALL from AcquireNextFrame (seen after mode changes on some drivers) is reported
// as DXGI_ERROR_ACCESS_LOST, i.e. recovered by Reinit.

#include <ct/capture/display.h>
#include <ct/platform/d3d_device.h>

#include <atomic>
#include <cstdint>

namespace ct::capture {

class DesktopDuplicator {
public:
    struct Config {
        DisplayInfo display;
        RectI region;                    // virtual-screen px, already clamped & even
        bool prefer_fp16_when_hdr = true;
        // The region is the whole display: after a mode change the whole new desktop is the content (NeedsFit),
        // instead of the region's old pixel rectangle.
        bool fit_full_display = false;
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
    // Same device, desktop image bindable as a shader resource, and either the region fully inside the desktop (no
    // letterbox) or NeedsFit().
    bool CanAcquireDirect() const;
    // fit_full_display and the desktop no longer matches the stream: its size (desktop orientation) differs from the
    // region or its rotation changed. Then the whole image from AcquireDirect is the content (src_box covers it all).
    bool NeedsFit() const;
    Rotation CurrentRotation() const { return m_cur_rotation; }   // rotation of the current duplication
    SizeI DesktopNativeSize() const { return m_desktop_native; }  // size of the duplicated image (native orientation)
    // Releases and re-creates the duplication. E_ACCESSDENIED (or DXGI_ERROR_UNSUPPORTED / NOT_CURRENTLY_AVAILABLE)
    // while a secure desktop is up: keep retrying every 250 ms. Logs the first failure of a streak only.
    HRESULT Reinit();
    DXGI_FORMAT Format() const { return m_format; }   // B8G8R8A8_UNORM or R16G16B16A16_FLOAT
    SizeI Size() const { return m_size; }
    Rotation DisplayRotation() const { return m_rotation; }
    bool IsCrossAdapter() const { return m_cross_adapter; }
    void Shutdown();
    // Releases the duplication without a Reinit failure streak (an idle prepared capturer: DWM keeps no duplication
    // for us while nothing records). The next Reinit re-creates it. Caller holds D3DDevice::Lock.
    void ReleaseDuplication();
    bool HasDuplication() const { return m_dup != nullptr; }

    // Tests (any thread; consumed by the capture thread): the next `count` acquires return `hr` without calling
    // AcquireNextFrame (DXGI_ERROR_ACCESS_LOST and INVALID_CALL drop the duplication as the real errors do); the next
    // `count` Reinit calls fail with `hr` (E_ACCESSDENIED = secure desktop); while `removed` is set the output is
    // treated as unplugged (Reinit returns DXGI_ERROR_NOT_FOUND).
    void InjectAcquireErrorForTest(HRESULT hr, uint32_t count);
    void InjectReinitErrorForTest(HRESULT hr, uint32_t count);
    void SimulateOutputRemovedForTest(bool removed);

private:
    HRESULT FindOutput(IDXGIFactory6* factory, Microsoft::WRL::ComPtr<IDXGIOutput1>& output,
                       Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter) const;
    HRESULT RefindOutput();
    HRESULT CreateDuplication(bool log_failures);
    HRESULT AcquireFrame(uint32_t timeout_ms, Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex, int64_t& present_qpc);
    HRESULT AcceptFrame(const DXGI_OUTDUPL_FRAME_INFO& info, Microsoft::WRL::ComPtr<IDXGIResource>& resource,
                        Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex, int64_t& present_qpc);
    void ComputeBox();
    HRESULT CopyCrossAdapter(ID3D11Texture2D* desktop, ID3D11Texture2D* dst);
    HRESULT ClearDst(ID3D11Texture2D* dst);

    platform::D3DDevice* m_dev = nullptr;
    Config m_cfg;
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;   // the factory m_output was found through (IsCurrent check)
    LUID m_adapter_luid{};                             // adapter that scans out the output
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
    Rotation m_rotation = Rotation::Identity;       // at Init: the stream's orientation (ColorConverter rotation)
    Rotation m_cur_rotation = Rotation::Identity;   // of the current duplication (changes with a display rotation)
    bool m_cross_adapter = false;
    bool m_invalid_call_logged = false;
    // Test injection (set from the test thread, consumed by the capture thread).
    std::atomic<HRESULT> m_inject_acquire_hr{S_OK};
    std::atomic<uint32_t> m_inject_acquire_count{0};
    std::atomic<HRESULT> m_inject_reinit_hr{S_OK};
    std::atomic<uint32_t> m_inject_reinit_count{0};
    std::atomic<bool> m_sim_removed{false};
    bool m_failing = false;      // a Reinit failure streak is in progress (log once)
    bool m_direct_ok = true;     // cleared when the desktop image cannot be bound as a shader resource
};

} // namespace ct::capture
