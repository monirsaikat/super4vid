#define CT_LOG_CATEGORY "d3d"
#include <ct/platform/d3d_device.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include <d3d10.h>   // ID3D10Multithread

#include <vector>

using Microsoft::WRL::ComPtr;

namespace ct::platform {

namespace {

std::string WideToUtf8Local(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

HRESULT CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, UINT flags, ComPtr<ID3D11Device>& device,
                     ComPtr<ID3D11DeviceContext>& context, D3D_FEATURE_LEVEL& level) {
    return D3D11CreateDevice(adapter, type, nullptr, flags, kLevels, static_cast<UINT>(std::size(kLevels)),
                             D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &level,
                             context.ReleaseAndGetAddressOf());
}

} // namespace

// ---------------------------------------------------------------- D3DDevice

D3DDevice::D3DDevice() {
    InitializeCriticalSection(&m_context_cs);
}

D3DDevice::~D3DDevice() {
    Shutdown();
    m_info_queue.Reset();
    m_debug.Reset();
    DeleteCriticalSection(&m_context_cs);
}

HRESULT D3DDevice::Init(bool enable_debug_layer) {
    CT_RETURN_IF_FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf())));

    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = m_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(adapter.GetAddressOf()));
    if (FAILED(hr)) {
        CT_LOG_WARN("EnumAdapterByGpuPreference failed: %s", HrToString(hr).c_str());
        adapter.Reset();
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (enable_debug_layer) flags |= D3D11_CREATE_DEVICE_DEBUG;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    // With an explicit adapter the driver type must be UNKNOWN.
    const D3D_DRIVER_TYPE hw_type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    hr = CreateDevice(adapter.Get(), hw_type, flags, device, context, level);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        CT_LOG_WARN("D3D11 debug layer not installed (Graphics Tools optional feature); continuing without it");
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = CreateDevice(adapter.Get(), hw_type, flags, device, context, level);
    }
    if (FAILED(hr)) {
        CT_LOG_WARN("hardware D3D11 device failed (%s); falling back to WARP", HrToString(hr).c_str());
        hr = CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, flags, device, context, level);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, flags, device, context, level);
        }
        CT_RETURN_IF_FAILED(hr);
        m_is_warp = true;
    }
    m_debug_layer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;

    CT_RETURN_IF_FAILED(device.As(&m_device));
    CT_RETURN_IF_FAILED(context.As(&m_context));

    // The context may be touched from several threads in later phases (always under Lock); belt and braces.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_context.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (m_debug_layer) {
        CT_LOG_IF_FAILED(m_device.As(&m_debug));
        if (SUCCEEDED(m_device.As(&m_info_queue))) {
            m_info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            m_info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    // Adapter description: from the device's actual adapter (covers the WARP path too).
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> actual;
    if (SUCCEEDED(m_device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(actual.GetAddressOf()))) {
        ComPtr<IDXGIAdapter1> actual1;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(actual.As(&actual1)) && SUCCEEDED(actual1->GetDesc1(&desc))) {
            m_adapter_name = WideToUtf8Local(desc.Description);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) m_is_warp = true;
        }
    }
    CT_LOG_INFO("D3D11 device: %s, feature level %x.%x%s%s", m_adapter_name.c_str(), (level >> 12) & 0xF,
                (level >> 8) & 0xF, m_is_warp ? ", WARP" : "", m_debug_layer ? ", debug layer" : "");
    return S_OK;
}

HRESULT D3DDevice::CheckRemoved() const {
    return m_device ? m_device->GetDeviceRemovedReason() : E_POINTER;
}

void D3DDevice::Shutdown() {
    if (m_context) {
        Lock lock(*this);
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_device.Reset();   // m_debug / m_info_queue keep the device alive for ReportLiveObjects()
    m_factory.Reset();
}

void D3DDevice::ReportLiveObjects() {
#if defined(CT_DEBUG)
    if (m_debug) {
        // IGNORE_INTERNAL hides objects the runtime holds itself. A clean exit lists only the device,
        // whose references are the m_debug/m_info_queue interfaces held here.
        m_debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
        DrainDebugMessages();
    }
#endif
    m_info_queue.Reset();
    m_debug.Reset();
}

void D3DDevice::DrainDebugMessages() {
    if (!m_info_queue) return;
    const UINT64 count = m_info_queue->GetNumStoredMessages();
    std::vector<char> storage;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(m_info_queue->GetMessage(i, nullptr, &size)) || size == 0) continue;
        storage.resize(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(m_info_queue->GetMessage(i, message, &size))) continue;
        CT_LOG_INFO("d3d11 debug: %.*s", static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    m_info_queue->ClearStoredMessages();
}

D3DDevice::Lock::Lock(D3DDevice& dev) : m_dev(dev) {
    EnterCriticalSection(&m_dev.m_context_cs);
}

D3DDevice::Lock::~Lock() {
    LeaveCriticalSection(&m_dev.m_context_cs);
}

// ---------------------------------------------------------------- SwapChain

SwapChain::~SwapChain() {
    Shutdown();
}

HRESULT SwapChain::Init(D3DDevice& dev, HWND hwnd) {
    m_dev = &dev;
    m_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 0;    // 0 = take the window's client size
    desc.Height = 0;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = m_flags;

    ComPtr<IDXGISwapChain1> sc1;
    CT_RETURN_IF_FAILED(dev.Factory()->CreateSwapChainForHwnd(dev.Device(), hwnd, &desc, nullptr, nullptr,
                                                               sc1.GetAddressOf()));
    CT_RETURN_IF_FAILED(sc1.As(&m_swapchain));
    CT_RETURN_IF_FAILED(m_swapchain->SetMaximumFrameLatency(1));
    m_waitable = m_swapchain->GetFrameLatencyWaitableObject();
    CT_LOG_IF_FAILED(dev.Factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    return CreateRtv();
}

HRESULT SwapChain::CreateRtv() {
    ComPtr<ID3D11Texture2D> back;
    CT_RETURN_IF_FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())));
    CT_RETURN_IF_FAILED(m_dev->Device()->CreateRenderTargetView(back.Get(), nullptr, m_rtv.ReleaseAndGetAddressOf()));
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    m_size = SizeI{static_cast<int32_t>(td.Width), static_cast<int32_t>(td.Height)};
    return S_OK;
}

HRESULT SwapChain::Resize(SizeI px) {
    if (!m_swapchain || px.IsEmpty()) return S_FALSE;
    if (px.w == m_size.w && px.h == m_size.h) return S_FALSE;
    {
        D3DDevice::Lock lock(*m_dev);
        // Every reference to a back buffer must be gone before ResizeBuffers.
        m_dev->Context()->OMSetRenderTargets(0, nullptr, nullptr);
        m_rtv.Reset();
        m_dev->Context()->Flush();
    }
    CT_RETURN_IF_FAILED(m_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, m_flags));
    return CreateRtv();
}

HRESULT SwapChain::Present(bool vsync) {
    if (!m_swapchain) return E_POINTER;
    return m_swapchain->Present(vsync ? 1 : 0, 0);
}

void SwapChain::Shutdown() {
    m_rtv.Reset();
    m_swapchain.Reset();
    if (m_waitable) {
        CloseHandle(m_waitable);
        m_waitable = nullptr;
    }
    m_dev = nullptr;
}

} // namespace ct::platform
