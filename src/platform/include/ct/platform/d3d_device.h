#pragma once
// ct/platform/d3d_device.h
// The process-wide D3D11 device (on the adapter that drives the primary monitor, then high-performance, then WARP)
// and the flip-model HWND swapchain. P02 added the MF DXGI device manager, NV12/FP16 checks and the video interfaces.
// Thread model: the device is free-threaded; the immediate context is used only under D3DDevice::Lock
// (multithread protection is also on). SwapChain belongs to the UI thread.
// Lifetime: Init() -> ... -> Shutdown(); ReportLiveObjects() after Shutdown() in Debug.

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <string>

namespace ct::platform {

class D3DDevice {
public:
    D3DDevice();
    ~D3DDevice();
    D3DDevice(const D3DDevice&) = delete;
    D3DDevice& operator=(const D3DDevice&) = delete;

    // Adapter that owns the primary monitor (output attached to the desktop whose rect contains (0,0)), else
    // DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, else WARP. FL 11_1/11_0, flags BGRA_SUPPORT|VIDEO_SUPPORT (+DEBUG).
    HRESULT Init(bool enable_debug_layer);
    ID3D11Device5* Device() const { return m_device.Get(); }
    ID3D11DeviceContext4* Context() const { return m_context.Get(); }
    IDXGIFactory6* Factory() const { return m_factory.Get(); }
    bool IsWarp() const { return m_is_warp; }
    bool DebugLayerActive() const { return m_debug_layer; }
    std::string AdapterName() const { return m_adapter_name; }
    HRESULT CheckRemoved() const;               // Device()->GetDeviceRemovedReason()

    IDXGIAdapter4* Adapter() const { return m_adapter.Get(); }
    LUID AdapterLuid() const { return m_adapter_luid; }
    // MFCreateDXGIDeviceManager + ResetDevice(device, token). Called lazily by ct_media; safe to call twice
    // (the second call returns S_OK and keeps the existing manager). Thread-safe.
    HRESULT InitMfManager();
    IMFDXGIDeviceManager* MfManager() const { return m_mf_mgr.Get(); }
    UINT MfToken() const { return m_mf_token; }
    // CheckFormatSupport has RENDER_TARGET | TEXTURE2D | SHADER_SAMPLE (NV12: per-plane R8/R8G8 RTVs work).
    bool SupportsNv12RenderTarget() const { return m_nv12_rt; }
    bool SupportsFp16RenderTarget() const { return m_fp16_rt; }
    // QueryInterface results; may be null (WARP without video support).
    ID3D11VideoDevice* VideoDevice() const { return m_video_device.Get(); }
    ID3D11VideoContext* VideoContext() const { return m_video_context.Get(); }
    // Debug only: ReportLiveDeviceObjects(DETAIL | IGNORE_INTERNAL), with the debug-layer messages copied into the log.
    // Call after Shutdown(); it then sees only the device itself. Also logs (and prints to stderr when not clean) a
    // summary: errors/warnings the debug layer raised during the run, and live objects other than the device.
    struct DebugReport {
        uint32_t corruption = 0, errors = 0, warnings = 0, infos = 0;
        uint32_t live_objects = 0;     // "Live ID3D11..." lines other than the device itself
        uint64_t discarded = 0;        // messages lost to the info queue's count limit
        bool available = false;        // false in Release or without the debug layer
        bool Clean() const { return corruption == 0 && errors == 0 && warnings == 0 && live_objects == 0; }
    };
    DebugReport ReportLiveObjects();

    // CRITICAL_SECTION around the immediate context.
    class Lock {
    public:
        explicit Lock(D3DDevice&);
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    private:
        D3DDevice& m_dev;
    };

    void Shutdown();

private:
    void DrainDebugMessages(DebugReport* report = nullptr);

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter4> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context;
    Microsoft::WRL::ComPtr<ID3D11Debug> m_debug;            // kept past Shutdown() for ReportLiveObjects()
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_info_queue;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_video_device;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_video_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_mf_mgr;
    CRITICAL_SECTION m_context_cs;
    CRITICAL_SECTION m_mf_cs;
    std::string m_adapter_name;
    LUID m_adapter_luid{};
    UINT m_mf_token = 0;
    bool m_is_warp = false;
    bool m_debug_layer = false;
    bool m_nv12_rt = false;
    bool m_fp16_rt = false;
};

class SwapChain {
public:
    SwapChain() = default;
    ~SwapChain();
    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    HRESULT Init(D3DDevice&, HWND);             // CreateSwapChainForHwnd: BGRA8, 2 buffers, FLIP_DISCARD, SCALING_NONE, ALPHA_IGNORE, FRAME_LATENCY_WAITABLE_OBJECT; SetMaximumFrameLatency(1); MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)
    HRESULT Resize(SizeI px);                   // release RTV, ResizeBuffers(0,0,0,UNKNOWN,flags), recreate RTV
    ID3D11RenderTargetView* Rtv() const { return m_rtv.Get(); }
    HANDLE WaitableObject() const { return m_waitable; }
    SizeI Size() const { return m_size; }
    HRESULT Present(bool vsync);               // Present(vsync?1:0, 0); DXGI_ERROR_DEVICE_REMOVED/RESET go to the caller
    void Shutdown();

private:
    HRESULT CreateRtv();

    D3DDevice* m_dev = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    HANDLE m_waitable = nullptr;
    SizeI m_size{};
    UINT m_flags = 0;
};

} // namespace ct::platform
