#define CT_LOG_CATEGORY "audio"
#include <ct/audio/devices.h>

#include "wasapi_internal.h"

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>

#include <initguid.h>   // PKEY_Device_FriendlyName definition (selectany)
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace ct::audio {

namespace detail {

HRESULT OpenEndpoint(const std::wstring& id, bool capture, ComPtr<IMMDevice>& out) {
    out.Reset();
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    if (id.empty()) return enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &out);
    hr = enumerator->GetDevice(id.c_str(), &out);
    if (FAILED(hr)) return hr;
    DWORD state = 0;
    hr = out->GetState(&state);
    if (FAILED(hr)) return hr;
    if (state != DEVICE_STATE_ACTIVE) {
        out.Reset();
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    return S_OK;
}

std::string FriendlyName(IMMDevice* device) {
    if (!device) return {};
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        name = WideToUtf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::wstring EndpointId(IMMDevice* device) {
    if (!device) return {};
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return {};
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}

SampleType SampleTypeOf(const WAVEFORMATEX* wf) {
    if (!wf) return SampleType::Unsupported;
    WORD tag = wf->wFormatTag;
    WORD valid_bits = wf->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
        if (ext->Samples.wValidBitsPerSample) valid_bits = ext->Samples.wValidBitsPerSample;
    }
    (void)valid_bits;   // 24-in-32 containers are read as int32 (low byte zero), which is exact
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) return SampleType::Float32;
    if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16) return SampleType::Int16;
    if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 24) return SampleType::Int24;
    if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32) return SampleType::Int32;
    return SampleType::Unsupported;
}

WAVEFORMATEXTENSIBLE MakeFloatFormat(uint32_t rate, int channels, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    if (channel_mask == 0) {
        switch (channels) {
        case 1: channel_mask = SPEAKER_FRONT_CENTER; break;
        case 2: channel_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; break;
        case 4: channel_mask = KSAUDIO_SPEAKER_QUAD; break;
        case 6: channel_mask = KSAUDIO_SPEAKER_5POINT1; break;
        case 8: channel_mask = KSAUDIO_SPEAKER_7POINT1_SURROUND; break;
        default: channel_mask = (1u << channels) - 1u; break;
        }
    }
    f.dwChannelMask = channel_mask;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

int64_t QpcToAbsUs(int64_t qpc) {
    // Same exact conversion as Clock::QpcToUs with a zero epoch; qpc is positive.
    uint64_t hi = 0;
    const uint64_t lo = _umul128(static_cast<uint64_t>(qpc), 1'000'000ull, &hi);
    uint64_t rem = 0;
    return static_cast<int64_t>(_udiv128(hi, lo, static_cast<uint64_t>(Clock::QpcFrequency()), &rem));
}

// IMMNotificationClient with manual reference counting (no ATL). Callbacks run on an MMDevAPI thread.
class NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(AudioDevices* owner) : m_owner(owner) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(m_refs.fetch_add(1) + 1); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = m_refs.fetch_sub(1) - 1;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (role != eConsole || (flow != eCapture && flow != eRender)) return S_OK;
        AudioDevices* owner = m_owner.load(std::memory_order_acquire);
        if (owner) owner->DispatchDefaultChanged(flow == eCapture, id ? std::wstring(id) : std::wstring());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        AudioDevices* owner = m_owner.load(std::memory_order_acquire);
        if (owner && id) owner->DispatchRemoved(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        // Unplugging a USB mic changes its state to NOTPRESENT/UNPLUGGED; OnDeviceRemoved fires only on uninstall.
        AudioDevices* owner = m_owner.load(std::memory_order_acquire);
        if (owner && id && state != DEVICE_STATE_ACTIVE) owner->DispatchRemoved(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void Detach() { m_owner.store(nullptr, std::memory_order_release); }

private:
    std::atomic<long> m_refs{1};
    std::atomic<AudioDevices*> m_owner;
};

} // namespace detail

struct AudioDevices::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    detail::NotificationClient* client = nullptr;   // owns one reference
};

AudioDevices::AudioDevices() : m_impl(std::make_unique<Impl>()) {}

AudioDevices::~AudioDevices() { Shutdown(); }

HRESULT AudioDevices::Init() {
    ExclusiveGuard guard(m_lock);
    if (m_impl->enumerator) return S_OK;
    ComPtr<IMMDeviceEnumerator> enumerator;
    CT_RETURN_IF_FAILED(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    auto* client = new detail::NotificationClient(this);
    const HRESULT hr = enumerator->RegisterEndpointNotificationCallback(client);
    if (FAILED(hr)) {
        LogHr(hr, "RegisterEndpointNotificationCallback", __FILE__, __LINE__);
        client->Detach();
        client->Release();
        client = nullptr;   // enumeration still works; only notifications are missing
    }
    m_impl->enumerator = enumerator;
    m_impl->client = client;
    return S_OK;
}

void AudioDevices::Shutdown() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    detail::NotificationClient* client = nullptr;
    {
        ExclusiveGuard guard(m_lock);
        enumerator.Swap(m_impl->enumerator);
        client = m_impl->client;
        m_impl->client = nullptr;
    }
    if (client) {
        client->Detach();
        // Unregister waits for callbacks in flight, so no callback touches `this` after it returns.
        if (enumerator) CT_LOG_IF_FAILED(enumerator->UnregisterEndpointNotificationCallback(client));
        client->Release();
    }
    ExclusiveGuard guard(m_listeners_lock);
    m_app_listener = nullptr;
    m_listeners.clear();
}

std::vector<DeviceInfo> AudioDevices::List(bool capture) {
    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    {
        SharedGuard guard(m_lock);
        enumerator = m_impl->enumerator;
    }
    if (!enumerator) return out;
    const std::wstring default_id = DefaultId(capture);
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)))
        return out;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return out;
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        DeviceInfo info;
        info.id = detail::EndpointId(device.Get());
        if (info.id.empty()) continue;
        info.name = detail::FriendlyName(device.Get());
        info.is_default = info.id == default_id;
        out.push_back(std::move(info));
    }
    return out;
}

std::wstring AudioDevices::DefaultId(bool capture) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    {
        SharedGuard guard(m_lock);
        enumerator = m_impl->enumerator;
    }
    if (!enumerator) return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &device))) return {};
    return detail::EndpointId(device.Get());
}

std::string AudioDevices::NameOf(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    {
        SharedGuard guard(m_lock);
        enumerator = m_impl->enumerator;
    }
    if (!enumerator || id.empty()) return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(id.c_str(), &device))) return {};
    return detail::FriendlyName(device.Get());
}

void AudioDevices::SetListener(Listener* listener) {
    ExclusiveGuard guard(m_listeners_lock);
    m_app_listener = listener;
}

void AudioDevices::AddListener(Listener* listener) {
    if (!listener) return;
    ExclusiveGuard guard(m_listeners_lock);
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
        m_listeners.push_back(listener);
}

void AudioDevices::RemoveListener(Listener* listener) {
    ExclusiveGuard guard(m_listeners_lock);
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}

void AudioDevices::DispatchDefaultChanged(bool capture, const std::wstring& id) {
    SharedGuard guard(m_listeners_lock);
    if (m_app_listener) m_app_listener->OnDefaultDeviceChanged(capture, id);
    for (Listener* l : m_listeners) l->OnDefaultDeviceChanged(capture, id);
}

void AudioDevices::DispatchRemoved(const std::wstring& id) {
    SharedGuard guard(m_listeners_lock);
    if (m_app_listener) m_app_listener->OnDeviceRemoved(id);
    for (Listener* l : m_listeners) l->OnDeviceRemoved(id);
}

} // namespace ct::audio
