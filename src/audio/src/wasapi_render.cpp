#define CT_LOG_CATEGORY "audio"
#include <ct/audio/wasapi_render.h>

#include "wasapi_internal.h"

#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>

#include <algorithm>
#include <intrin.h>

using Microsoft::WRL::ComPtr;

namespace ct::audio {

struct WasapiRender::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    UINT64 clock_freq = 0;
    UINT32 buffer_frames = 0;
    REFERENCE_TIME stream_latency_hns = 0;
    Event data{false, false};
};

WasapiRender::WasapiRender() : m_impl(std::make_unique<Impl>()) {}

WasapiRender::~WasapiRender() { Stop(); }

HRESULT WasapiRender::Init(const std::wstring& device_id, uint32_t buffer_ms, const char* thread_name) {
    if (m_running.load()) return E_ILLEGAL_METHOD_CALL;
    m_thread_name = thread_name ? thread_name : "ct-audio-out";
    Impl& im = *m_impl;
    im.client.Reset();
    im.render.Reset();
    im.clock.Reset();
    ComPtr<IMMDevice> device;
    CT_RETURN_IF_FAILED(detail::OpenEndpoint(device_id, false, device));
    m_name = detail::FriendlyName(device.Get());
    ComPtr<IAudioClient> client;
    CT_RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
    const WAVEFORMATEXTENSIBLE fmt = detail::MakeFloatFormat(SampleRate(), Channels(), 0);
    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(std::clamp<uint32_t>(buffer_ms, 20, 100)) * 10'000;
    CT_RETURN_IF_FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                           duration, 0, &fmt.Format, nullptr));
    CT_RETURN_IF_FAILED(client->SetEventHandle(im.data.Handle()));
    CT_RETURN_IF_FAILED(client->GetBufferSize(&im.buffer_frames));
    CT_RETURN_IF_FAILED(client->GetService(IID_PPV_ARGS(&im.render)));
    CT_RETURN_IF_FAILED(client->GetService(IID_PPV_ARGS(&im.clock)));
    CT_RETURN_IF_FAILED(im.clock->GetFrequency(&im.clock_freq));
    if (FAILED(client->GetStreamLatency(&im.stream_latency_hns))) im.stream_latency_hns = 0;
    im.client = client;
    m_last_error.store(S_OK, std::memory_order_relaxed);
    return S_OK;
}

HRESULT WasapiRender::FillAvailable() {
    Impl& im = *m_impl;
    UINT32 padding = 0;
    CT_RETURN_IF_FAILED(im.client->GetCurrentPadding(&padding));
    const UINT32 frames = im.buffer_frames - padding;
    if (frames == 0) return S_OK;
    BYTE* data = nullptr;
    HRESULT hr = im.render->GetBuffer(frames, &data);
    if (FAILED(hr)) return hr;
    m_callback->Render(reinterpret_cast<float*>(data), frames, Channels(), m_written);
    hr = im.render->ReleaseBuffer(frames, 0);
    if (FAILED(hr)) return hr;
    m_written += frames;
    return S_OK;
}

bool WasapiRender::Start(Callback& callback) {
    Impl& im = *m_impl;
    if (m_running.load() || !im.client) return false;
    m_callback = &callback;
    m_written = 0;
    m_stop.Reset();
    // Prefill so the device starts with a full buffer instead of an underrun.
    HRESULT hr = FillAvailable();
    if (SUCCEEDED(hr)) hr = im.client->Start();
    if (FAILED(hr)) {
        LogHr(hr, "WasapiRender::Start", __FILE__, __LINE__);
        m_last_error.store(hr, std::memory_order_relaxed);
        return false;
    }
    m_running.store(true, std::memory_order_release);
    Thread::Options opt;
    opt.name = m_thread_name.c_str();
    opt.priority = THREAD_PRIORITY_TIME_CRITICAL;
    opt.mmcss_task = L"Pro Audio";
    opt.com_mta = true;
    if (!m_thread.Start(opt, [this] { ThreadMain(); })) {
        im.client->Stop();
        m_running.store(false);
        return false;
    }
    return true;
}

void WasapiRender::ThreadMain() {
    Impl& im = *m_impl;
    HANDLE handles[2] = {m_stop.Handle(), im.data.Handle()};
    bool failed = false;
    for (;;) {
        const DWORD w = WaitForMultipleObjects(failed ? 1 : 2, handles, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (failed) continue;
        const HRESULT hr = FillAvailable();
        if (FAILED(hr)) [[unlikely]] {
            // AUDCLNT_E_DEVICE_INVALIDATED etc.: stop rendering; the owner sees LastError() and re-inits.
            m_last_error.store(hr, std::memory_order_relaxed);
            failed = true;
        }
    }
}

void WasapiRender::Stop() {
    if (!m_running.load()) return;
    m_stop.Set();
    if (!m_thread.Join(5000)) CT_FATAL("audio render thread did not stop within 5 s");
    if (m_impl->client) {
        m_impl->client->Stop();
        m_impl->client->Reset();
    }
    m_running.store(false, std::memory_order_release);
}

HRESULT WasapiRender::GetClock(int64_t& device_frames, int64_t& qpc) const {
    const Impl& im = *m_impl;
    if (!im.clock || im.clock_freq == 0) return E_ILLEGAL_METHOD_CALL;
    UINT64 pos = 0;
    UINT64 qpc_hns = 0;
    CT_RETURN_IF_FAILED(im.clock->GetPosition(&pos, &qpc_hns));
    device_frames = static_cast<int64_t>(pos * SampleRate() / im.clock_freq);
    // qpc_hns is QPC converted to 100 ns units; convert back to ticks (128-bit safe).
    uint64_t hi = 0;
    const uint64_t lo = _umul128(qpc_hns, static_cast<uint64_t>(Clock::QpcFrequency()), &hi);
    uint64_t rem = 0;
    qpc = static_cast<int64_t>(_udiv128(hi, lo, 10'000'000ull, &rem));
    return S_OK;
}

double WasapiRender::LatencyMs() const {
    const Impl& im = *m_impl;
    return static_cast<double>(im.buffer_frames) * 1000.0 / SampleRate() +
           static_cast<double>(im.stream_latency_hns) / 10'000.0;
}

} // namespace ct::audio
