#define CT_LOG_CATEGORY "audio"
#include "capture_engine.h"

#include <ct/audio/dsp.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::audio::detail {

namespace {

constexpr REFERENCE_TIME kBufferHns = 2'000'000;    // 200 ms shared-mode buffer
constexpr int64_t kStarveUs = 100'000;              // no packet for this long -> fill silence by elapsed time
constexpr int64_t kStarveLagUs = 50'000;            // ... up to now - lag, so late packets still fit after it
constexpr int64_t kRetryUs = 500'000;               // reopen attempts while no device is available
constexpr int64_t kPollUs = 5'000;                  // loopback poll period
constexpr uint32_t kPoolPackets = 64;
constexpr uint32_t kPoolFrames = 4800;

int64_t UsToQpcTicks(int64_t us) {
    return us * Clock::QpcFrequency() / 1'000'000;   // small durations only (no overflow below ~15 min at 10 MHz)
}

} // namespace

CaptureEngine::CaptureEngine(EngineMode mode) : m_mode(mode) {}

CaptureEngine::~CaptureEngine() {
    Stop();
    Close();
    if (m_devices) m_devices->RemoveListener(this);
}

std::string CaptureEngine::DeviceName() const {
    SharedGuard guard(m_name_lock);
    return m_name;
}

CaptureStats CaptureEngine::Stats() const {
    CaptureStats s;
    s.device_packets = m_st_device_packets.load(std::memory_order_relaxed);
    s.packets_delivered = m_st_packets.load(std::memory_order_relaxed);
    s.frames_delivered = m_st_frames.load(std::memory_order_relaxed);
    s.leading_pad_frames = m_st_pad.load(std::memory_order_relaxed);
    s.frames_inserted = m_st_inserted.load(std::memory_order_relaxed);
    s.frames_dropped = m_st_dropped.load(std::memory_order_relaxed) + m_st_sink_dropped.load(std::memory_order_relaxed);
    s.discontinuities = m_st_disc.load(std::memory_order_relaxed);
    s.reinits = m_st_reinits.load(std::memory_order_relaxed);
    s.drift_ms = m_st_drift_ms.load(std::memory_order_relaxed);
    s.last_device_end_us = m_st_last_end_us.load(std::memory_order_relaxed);
    s.device_open = m_st_open.load(std::memory_order_relaxed);
    return s;
}

void CaptureEngine::SetError(HRESULT hr) { m_last_error.store(hr, std::memory_order_relaxed); }

void CaptureEngine::SilenceCallback::Render(float* interleaved, uint32_t frames, int channels, int64_t) {
    std::memset(interleaved, 0, sizeof(float) * frames * static_cast<size_t>(channels));
}

void CaptureEngine::StartKeepAlive() {
    if (IsMic() || m_open_id.empty()) return;
    // Started before the loopback client: on some drivers an endpoint with no active render stream delivers no
    // loopback packets at all.
    HRESULT hr = m_keepalive.Init(m_open_id, 50, "ct-audio-sys-keepalive");
    if (SUCCEEDED(hr) && !m_keepalive.Start(m_silence)) {
        hr = m_keepalive.LastError();
        if (SUCCEEDED(hr)) hr = E_FAIL;
    }
    if (FAILED(hr)) CT_LOG_WARN("audio.sys: keep-alive render stream failed: %s", HrToString(hr).c_str());
}

void CaptureEngine::StopKeepAlive() { m_keepalive.Stop(); }

HRESULT CaptureEngine::Init(const CaptureConfig& config, AudioDevices& devices) {
    if (m_running.load()) return E_ILLEGAL_METHOD_CALL;
    dsp::Init();
    m_config = config;
    m_packet_frames = std::clamp<uint32_t>(config.packet_frames, 48, kPoolFrames);
    m_devices = &devices;
    if (!m_stereo) m_stereo = std::make_unique<float[]>(static_cast<size_t>(kScratchFrames) * 2);
    if (m_pool.Count() == 0) {
        m_pool.Init(kPoolPackets, [](size_t) {
            PacketBuffer b;
            b.samples = std::make_unique<float[]>(static_cast<size_t>(kPoolFrames) * kChannels);
            return b;
        });
    }
    m_pool_dropped = &ct::Stats::C(IsMic() ? "audio.mic.dropped" : "audio.sys.dropped");
    if (!IsMic() && !m_timer.Init()) return HRESULT_FROM_WIN32(GetLastError());
    m_timer.SetSpinUs(0);   // a 5 ms poll needs no sub-millisecond accuracy; spinning would cost CPU

    Close();
    const HRESULT hr = Open(config.device_id);
    SetError(hr);
    if (FAILED(hr)) return hr;
    devices.AddListener(this);
    return S_OK;
}

HRESULT CaptureEngine::InitializeClient(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    {
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client3))) client = client3;
        else CT_RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
    }
    WAVEFORMATEX* mix_raw = nullptr;
    CT_RETURN_IF_FAILED(client->GetMixFormat(&mix_raw));
    std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> mix(mix_raw);
    const int channels = std::clamp<int>(mix->nChannels, 1, 8);
    DWORD mask = 0;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix.get())->dwChannelMask;
    if (channels != mix->nChannels) mask = 0;
    WAVEFORMATEXTENSIBLE req = MakeFloatFormat(kSampleRate, channels, mask);

    const DWORD base = IsMic() ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : AUDCLNT_STREAMFLAGS_LOOPBACK;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    base | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    kBufferHns, 0, &req.Format, nullptr);
    const WAVEFORMATEX* format = &req.Format;
    if (FAILED(hr)) {
        if (!m_logged_fallback) {
            CT_LOG_WARN("%s: float 48 kHz request rejected (%s); using the mix format %u Hz %u ch", StatsPrefix(),
                        HrToString(hr).c_str(), static_cast<unsigned>(mix->nSamplesPerSec),
                        static_cast<unsigned>(mix->nChannels));
            m_logged_fallback = true;
        }
        // A failed Initialize leaves the client unusable; activate a fresh one for the mix format.
        client.Reset();
        CT_RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
        CT_RETURN_IF_FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, base, kBufferHns, 0, mix.get(), nullptr));
        format = mix.get();
    }
    const SampleType type = SampleTypeOf(format);
    if (type == SampleType::Unsupported) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (format->nSamplesPerSec != static_cast<DWORD>(kSampleRate)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (IsMic()) CT_RETURN_IF_FAILED(client->SetEventHandle(m_data.Handle()));
    ComPtr<IAudioCaptureClient> capture;
    CT_RETURN_IF_FAILED(client->GetService(IID_PPV_ARGS(&capture)));

    m_dev_rate = format->nSamplesPerSec;
    m_dev_channels = format->nChannels;
    m_dev_block_align = format->nBlockAlign;
    m_dev_type = type;
    m_client = client;
    m_capture = capture;
    return S_OK;
}

HRESULT CaptureEngine::Open(const std::wstring& id) {
    ComPtr<IMMDevice> device;
    HRESULT hr = OpenEndpoint(id, IsMic(), device);
    if (FAILED(hr)) return hr;
    hr = InitializeClient(device.Get());
    if (FAILED(hr)) {
        m_client.Reset();
        m_capture.Reset();
        return hr;
    }
    m_open_id = EndpointId(device.Get());
    {
        ExclusiveGuard guard(m_request_lock);
        m_watched_id = m_open_id;
    }
    {
        ExclusiveGuard guard(m_name_lock);
        m_name = FriendlyName(device.Get());
    }
    m_st_open.store(true, std::memory_order_relaxed);
    CT_LOG_INFO("%s: opened \"%s\" (%u Hz, %d ch)", StatsPrefix(), DeviceName().c_str(), m_dev_rate, m_dev_channels);
    return S_OK;
}

void CaptureEngine::Close() {
    if (m_client) m_client->Stop();
    m_capture.Reset();
    m_client.Reset();
    m_st_open.store(false, std::memory_order_relaxed);
}

bool CaptureEngine::Start(int64_t epoch_qpc, AudioSink& sink) {
    if (m_running.load() || !m_devices) return false;
    m_sink = &sink;
    m_epoch_us = QpcToAbsUs(epoch_qpc);
    m_pause_offset_us.store(0, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_was_paused = false;
    m_aligner.Reset(kSampleRate, kChannels, 0);
    m_out_frames = 0;
    m_pending = nullptr;
    m_pending_frames = 0;
    m_force_resync = false;
    m_device_lost = !m_client;
    m_error_reported = false;
    m_stop.Reset();
    m_meter.Reset();
    m_running.store(true);
    StartKeepAlive();
    Thread::Options opt;
    opt.name = IsMic() ? "ct-audio-mic" : "ct-audio-sys";
    opt.priority = THREAD_PRIORITY_TIME_CRITICAL;
    opt.mmcss_task = L"Pro Audio";
    opt.com_mta = true;   // STA would break IMMNotificationClient delivery and WASAPI threading
    if (!m_thread.Start(opt, [this] { ThreadMain(); })) {
        m_running.store(false);
        return false;
    }
    return true;
}

void CaptureEngine::Stop() {
    if (!m_running.load()) return;
    m_stop.Set();
    if (!m_thread.Join(5000)) CT_FATAL("audio capture thread did not stop within 5 s");
    StopKeepAlive();
    m_running.store(false);
}

void CaptureEngine::SetPaused(bool paused, int64_t at_qpc) {
    if (at_qpc == 0) at_qpc = Clock::QpcNow();
    const bool was = m_paused.load(std::memory_order_relaxed);
    if (paused && !was) {
        m_pause_begin_qpc = at_qpc;
        m_paused.store(true, std::memory_order_release);
    } else if (!paused && was) {
        const int64_t d_us = QpcToAbsUs(at_qpc) - QpcToAbsUs(m_pause_begin_qpc);
        m_pause_offset_us.fetch_add(d_us > 0 ? d_us : 0, std::memory_order_relaxed);
        m_paused.store(false, std::memory_order_release);
    }
}

TimeUs CaptureEngine::MediaUsFromAbsUs(int64_t abs_us) const {
    return abs_us - m_epoch_us - m_pause_offset_us.load(std::memory_order_relaxed);
}

TimeUs CaptureEngine::MediaNowUs() const { return MediaUsFromAbsUs(QpcToAbsUs(Clock::QpcNow())); }

void CaptureEngine::OnDefaultDeviceChanged(bool capture, const std::wstring& new_id) {
    if (capture != IsMic() || !m_config.follow_default_device || !m_config.device_id.empty()) return;
    {
        ExclusiveGuard guard(m_request_lock);
        m_requested_id = new_id;
    }
    m_reinit_requested.store(true, std::memory_order_release);
    m_wake.Set();
}

void CaptureEngine::OnDeviceRemoved(const std::wstring& id) {
    {
        ExclusiveGuard guard(m_request_lock);
        if (id != m_watched_id) return;
        m_requested_id = m_config.follow_default_device ? std::wstring() : m_config.device_id;
    }
    m_reinit_requested.store(true, std::memory_order_release);
    m_wake.Set();
}

void CaptureEngine::ThreadMain() {
    m_last_packet_qpc = Clock::QpcNow();
    if (m_client) {
        const HRESULT hr = m_client->Start();
        if (FAILED(hr)) {
            LogHr(hr, "IAudioClient::Start", __FILE__, __LINE__);
            SetError(hr);
            m_device_lost = true;
        }
    }
    const int64_t poll_ticks = UsToQpcTicks(kPollUs);
    int64_t next_poll = Clock::QpcNow() + poll_ticks;
    HANDLE handles[3] = {m_stop.Handle(), m_wake.Handle(), IsMic() ? m_data.Handle() : m_timer.Handle()};
    for (;;) {
        if (!IsMic()) {
            const int64_t now = Clock::QpcNow();
            if (next_poll <= now) next_poll = now + poll_ticks;   // fell behind: do not burst
            m_timer.ArmAbsoluteQpc(next_poll);
            next_poll += poll_ticks;
        }
        const DWORD w = WaitForMultipleObjects(3, handles, FALSE, IsMic() ? 20 : 50);
        if (w == WAIT_OBJECT_0) break;
        if (m_capture && !m_device_lost) ReadPackets();
        const bool requested = m_reinit_requested.load(std::memory_order_acquire);
        if (requested || (m_device_lost && Clock::QpcNow() >= m_next_retry_qpc)) Reopen();
        CheckStarvation();
    }
    if (m_client) {
        m_client->Stop();
        m_client->Reset();   // drop buffered data so a later Start begins fresh
    }
    FlushPending();
}

void CaptureEngine::ReadPackets() {
    UINT32 next = 0;
    HRESULT hr = S_OK;
    while (SUCCEEDED(hr = m_capture->GetNextPacketSize(&next)) && next > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 dev_pos = 0;
        UINT64 qpc_hns = 0;
        hr = m_capture->GetBuffer(&data, &frames, &flags, &dev_pos, &qpc_hns);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) [[unlikely]]
            break;
        m_last_packet_qpc = Clock::QpcNow();
        m_st_device_packets.fetch_add(1, std::memory_order_relaxed);
        const bool ts_error = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        const bool disc = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (ts_error || disc) m_st_disc.fetch_add(1, std::memory_order_relaxed);
        // QPCPosition is in 100 ns units, not QPC ticks.
        const TimeUs t = MediaUsFromAbsUs(static_cast<int64_t>(qpc_hns / 10));
        ProcessDeviceFrames(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, !ts_error, t, disc);
        m_capture->ReleaseBuffer(frames);   // always, even for silent packets; data is invalid afterwards
    }
    if (FAILED(hr)) [[unlikely]] {
        // AUDCLNT_E_DEVICE_INVALIDATED (unplugged, format change, exclusive takeover) or a service restart.
        SetError(hr);
        m_device_lost = true;
        m_next_retry_qpc = 0;
        {
            ExclusiveGuard guard(m_request_lock);
            if (!m_reinit_requested.load(std::memory_order_relaxed))
                m_requested_id = m_config.follow_default_device ? std::wstring() : m_config.device_id;
        }
    }
}

void CaptureEngine::ConvertToStereo(const BYTE* data, uint32_t frames, float* out) const {
    const int ch = m_dev_channels;
    switch (m_dev_type) {
    case SampleType::Float32: {
        const float* in = reinterpret_cast<const float*>(data);
        if (ch == 2) std::memcpy(out, in, sizeof(float) * 2 * frames);
        else dsp::DownmixToStereo(in, ch, out, frames);
        return;
    }
    case SampleType::Int16: {
        const int16_t* in = reinterpret_cast<const int16_t*>(data);
        for (uint32_t f = 0; f < frames; ++f) {
            const int16_t* p = in + static_cast<size_t>(f) * ch;
            out[2 * f] = p[0] * (1.0f / 32768.0f);
            out[2 * f + 1] = p[ch > 1 ? 1 : 0] * (1.0f / 32768.0f);
        }
        return;
    }
    case SampleType::Int24: {
        for (uint32_t f = 0; f < frames; ++f) {
            const BYTE* p = data + static_cast<size_t>(f) * m_dev_block_align;
            for (int c = 0; c < 2; ++c) {
                const BYTE* s = p + 3 * (ch > 1 ? c : 0);
                const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(s[0]) << 8) |
                                                       (static_cast<uint32_t>(s[1]) << 16) |
                                                       (static_cast<uint32_t>(s[2]) << 24));
                out[2 * f + c] = static_cast<float>(v) * (1.0f / 2147483648.0f);
            }
        }
        return;
    }
    case SampleType::Int32: {
        const int32_t* in = reinterpret_cast<const int32_t*>(data);
        for (uint32_t f = 0; f < frames; ++f) {
            const int32_t* p = in + static_cast<size_t>(f) * ch;
            out[2 * f] = static_cast<float>(p[0]) * (1.0f / 2147483648.0f);
            out[2 * f + 1] = static_cast<float>(p[ch > 1 ? 1 : 0]) * (1.0f / 2147483648.0f);
        }
        return;
    }
    case SampleType::Unsupported:
        break;
    }
    std::memset(out, 0, sizeof(float) * 2 * frames);
}

void CaptureEngine::ProcessDeviceFrames(const BYTE* data, uint32_t frames, bool silent, bool timestamp_valid,
                                        TimeUs t_us, bool discontinuity) {
    const bool paused = m_paused.load(std::memory_order_acquire);
    if (m_was_paused && !paused) m_force_resync = true;   // resume: land exactly on the shifted timeline
    m_was_paused = paused;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(frames - done, kScratchFrames);
        float* stereo = m_stereo.get();
        if (silent || !data) std::memset(stereo, 0, sizeof(float) * 2 * n);
        else ConvertToStereo(data + static_cast<size_t>(done) * m_dev_block_align, n, stereo);
        m_meter.Push(stereo, n, 2);
        const TimeUs t = t_us + static_cast<int64_t>(done) * kUsPerSecond / m_dev_rate;
        if (!paused) {
            const bool resync = discontinuity || m_force_resync;
            m_force_resync = false;
            PushAligned(timestamp_valid ? t : m_aligner.NextExpectedUs(), stereo, n, resync);
        }
        done += n;
        discontinuity = false;
    }
    m_st_last_end_us.store(t_us + static_cast<int64_t>(frames) * kUsPerSecond / m_dev_rate, std::memory_order_relaxed);
}

void CaptureEngine::PushAligned(TimeUs t_us, const float* stereo, uint32_t frames, bool discontinuity) {
    m_aligner.Push(t_us, stereo, frames, discontinuity,
                   [this](TimeUs, const float* p, uint32_t n, bool fill) { OnAligned(p, n, fill); });
    m_st_drift_ms.store(m_aligner.DriftMs(), std::memory_order_relaxed);
    m_st_pad.store(static_cast<uint64_t>(m_aligner.LeadingPadFrames()), std::memory_order_relaxed);
    m_st_inserted.store(static_cast<uint64_t>(m_aligner.TotalInserted()), std::memory_order_relaxed);
    m_st_dropped.store(static_cast<uint64_t>(m_aligner.TotalDropped()), std::memory_order_relaxed);
}

void CaptureEngine::OnAligned(const float* p, uint32_t frames, bool fill) {
    while (frames > 0) {
        if (m_pending && m_pending_fill != fill) FlushPending();
        if (!m_pending) {
            m_pending = m_pool.Acquire(&m_pending_index);
            if (!m_pending) [[unlikely]] {
                m_pool_dropped->Add(frames);
                m_st_sink_dropped.fetch_add(frames, std::memory_order_relaxed);
                m_out_frames += frames;
                return;
            }
            m_pending_frames = 0;
            m_pending_fill = fill;
            m_pending_first_frame = m_out_frames;
        }
        const uint32_t n = std::min(frames, m_packet_frames - m_pending_frames);
        std::memcpy(m_pending->samples.get() + static_cast<size_t>(m_pending_frames) * kChannels, p,
                    sizeof(float) * kChannels * n);
        m_pending_frames += n;
        m_out_frames += n;
        p += static_cast<size_t>(n) * kChannels;
        frames -= n;
        if (m_pending_frames == m_packet_frames) FlushPending();
    }
}

void CaptureEngine::FlushPending() {
    if (!m_pending) return;
    if (m_pending_frames > 0) {
        AudioPacket packet;
        packet.t_us = m_pending_first_frame * kUsPerSecond / kSampleRate;
        packet.frames = m_pending_frames;
        packet.data = m_pending->samples.get();
        packet.channels = kChannels;
        packet.silence_fill = m_pending_fill;
        if (m_sink->OnAudio(m_config.stream_id, packet)) {
            m_st_packets.fetch_add(1, std::memory_order_relaxed);
            m_st_frames.fetch_add(m_pending_frames, std::memory_order_relaxed);
        } else {
            m_pool_dropped->Add(m_pending_frames);
            m_st_sink_dropped.fetch_add(m_pending_frames, std::memory_order_relaxed);
        }
    }
    m_pool.Release(m_pending_index);
    m_pending = nullptr;
    m_pending_frames = 0;
}

void CaptureEngine::Reopen() {
    std::wstring id;
    {
        ExclusiveGuard guard(m_request_lock);
        id = m_requested_id;
        m_reinit_requested.store(false, std::memory_order_relaxed);
    }
    Close();
    StopKeepAlive();
    m_st_reinits.fetch_add(1, std::memory_order_relaxed);
    HRESULT hr = Open(id);
    if (SUCCEEDED(hr)) {
        StartKeepAlive();
        hr = m_client->Start();
    }
    if (FAILED(hr)) {
        SetError(hr);
        Close();
        m_device_lost = true;
        m_next_retry_qpc = Clock::QpcNow() + UsToQpcTicks(kRetryUs);
        if (!m_error_reported) {
            m_error_reported = true;
            CT_LOG_WARN("%s: reopening the device failed: %s (retrying every %d ms, filling silence)", StatsPrefix(),
                        HrToString(hr).c_str(), static_cast<int>(kRetryUs / 1000));
            m_sink->OnAudioError(m_config.stream_id, hr);
        }
        return;
    }
    SetError(S_OK);
    m_device_lost = false;
    m_error_reported = false;
    m_force_resync = true;   // the switch gap becomes silence at the correct position
    m_last_packet_qpc = Clock::QpcNow();
}

void CaptureEngine::CheckStarvation() {
    if (m_paused.load(std::memory_order_acquire)) return;
    const int64_t now = Clock::QpcNow();
    if (now - m_last_packet_qpc < UsToQpcTicks(kStarveUs)) return;
    m_aligner.FillSilenceUntil(MediaNowUs() - kStarveLagUs,
                               [this](TimeUs, const float* p, uint32_t n, bool fill) { OnAligned(p, n, fill); });
    m_st_pad.store(static_cast<uint64_t>(m_aligner.LeadingPadFrames()), std::memory_order_relaxed);
    m_st_inserted.store(static_cast<uint64_t>(m_aligner.TotalInserted()), std::memory_order_relaxed);
}

} // namespace ct::audio::detail
