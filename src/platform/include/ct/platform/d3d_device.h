#pragma once
// ct/platform/d3d_device.h
// The process-wide D3D11 device (high-performance adapter, WARP fallback) and the flip-model HWND swapchain.
// P02 extends D3DDevice with the MF DXGI device manager and NV12 format checks.
// Thread model: the device is free-threaded; the immediate context is used only under D3DDevice::Lock
// (multithread protection is also on). SwapChain belongs to the UI thread.
// Lifetime: Init() -> ... -> Shutdown(); ReportLiveObjects() after Shutdown() in Debug.

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace ct::platform {

class D3DDevice {
public:
    D3DDevice();
    ~D3DDevice();
    D3DDevice(const D3DDevice&) = delete;
    D3DDevice& operator=(const D3DDevice&) = delete;

    HRESULT Init(bool enable_debug_layer);      // adapter by DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, FL 11_1/11_0, flags BGRA_SUPPORT|VIDEO_SUPPORT (+DEBUG), WARP fallback
    ID3D11Device5* Device() const { return m_device.Get(); }
    ID3D11DeviceContext4* Context() const { return m_context.Get(); }
    IDXGIFactory6* Factory() const { return m_factory.Get(); }
    bool IsWarp() const { return m_is_warp; }
    bool DebugLayerActive() const { return m_debug_layer; }
    std::string AdapterName() const { return m_adapter_name; }
    HRESULT CheckRemoved() const;               // Device()->GetDeviceRemovedReason()
    // Debug only: ReportLiveDeviceObjects(DETAIL | IGNORE_INTERNAL), with the debug-layer messages copied into the log.
    // Call after Shutdown(); it then sees only the device itself.
    void ReportLiveObjects();

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
    void DrainDebugMessages();

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context;
    Microsoft::WRL::ComPtr<ID3D11Debug> m_debug;            // kept past Shutdown() for ReportLiveObjects()
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_info_queue;
    CRITICAL_SECTION m_context_cs;
    std::string m_adapter_name;
    bool m_is_warp = false;
    bool m_debug_layer = false;
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
