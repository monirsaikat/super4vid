#define CT_LOG_CATEGORY "playback"
#include <ct/playback/audio_engine.h>

#include <ct/audio/dsp.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ct::playback {

namespace {
constexpr uint32_t kMaxChunk = 4800;           // frames rendered per inner step (100 ms)
constexpr float kRampFrames = 240.0f;          // 5 ms gain ramp
constexpr int64_t kMaxExtrapolateUs = 50'000;  // QPC interpolation beyond the last IAudioClock reading
} // namespace

AudioEngine::AudioEngine() { m_callback.engine = this; }
AudioEngine::~AudioEngine() { Stop(); }

HRESULT AudioEngine::Init(audio::AudioDevices& devices) { return Init(devices, std::wstring()); }

HRESULT AudioEngine::Init(audio::AudioDevices& devices, const std::wstring& preferred_device) {
    audio::dsp::Init();
    m_vs.assign(static_cast<size_t>(kMaxChunk * kMaxRate + 8) * kChannels, 0.0f);
    const std::wstring fallback = devices.DefaultId(false);
    // The chosen endpoint may have been unplugged since it was saved: try it, then the default one.
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (!preferred_device.empty() && preferred_device != fallback) {
        hr = m_render.Init(preferred_device, 20, "ct-audio-out");
        if (SUCCEEDED(hr)) m_device_id = preferred_device;
        else CT_LOG_WARN("AudioEngine: the chosen output device is unavailable (%s); using the default one",
                         HrToString(hr).c_str());
    }
    if (FAILED(hr)) {
        if (fallback.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        CT_RETURN_IF_FAILED(m_render.Init(fallback, 20, "ct-audio-out"));
        m_device_id = fallback;
    }
    CT_LOG_INFO("AudioEngine: %s, latency %.1f ms", m_render.DeviceName().c_str(), m_render.LatencyMs());
    return S_OK;
}

bool AudioEngine::Start(Source& source) {
    Stop();
    m_source = &source;
    m_applied_gen = 0;
    m_playing = false;
    m_gain = m_muted.load() ? 0.0f : m_volume.load();
    m_started = m_render.Start(m_callback);
    return m_started;
}

void AudioEngine::Stop() {
    if (m_started) m_render.Stop();
    m_started = false;
    m_playing_ui = false;
    m_anchor_playing = false;
}

bool AudioEngine::DeviceOk() const { return m_started && m_render.IsRunning() && SUCCEEDED(m_render.LastError()); }

void AudioEngine::Play(TimeUs from_us, double rate) {
    rate = rate < 0 ? rate : std::clamp(rate, kMinRate, kMaxRate);
    {
        ExclusiveGuard g(m_cmd_lock);
        m_cmd.from_us = from_us;
        m_cmd.rate = rate;
        m_cmd.playing = rate > 0;
        m_ui_gen.store(++m_cmd.gen, std::memory_order_relaxed);
    }
    m_paused_pos = from_us;
    m_playing_ui.store(rate > 0, std::memory_order_release);
}

void AudioEngine::Pause() {
    const TimeUs pos = PositionUs();
    {
        ExclusiveGuard g(m_cmd_lock);
        m_cmd.playing = false;
        m_ui_gen.store(++m_cmd.gen, std::memory_order_relaxed);
    }
    m_paused_pos = pos;
    m_playing_ui.store(false, std::memory_order_release);
}

void AudioEngine::SetVolume(float v) { m_volume.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed); }
void AudioEngine::SetMuted(bool m) { m_muted.store(m, std::memory_order_relaxed); }

TimeUs AudioEngine::PositionUs() const {
    if (!m_playing_ui.load(std::memory_order_acquire)) return m_paused_pos.load();
    uint64_t seq = 0;
    int64_t from = 0, frames_at_play = 0;
    double rate = 1.0;
    bool playing = false;
    uint64_t gen = 0;
    for (;;) {
        seq = m_anchor_seq.load(std::memory_order_acquire);
        if (seq & 1) continue;
        from = m_anchor_from.load(std::memory_order_relaxed);
        rate = m_anchor_rate.load(std::memory_order_relaxed);
        frames_at_play = m_anchor_frames.load(std::memory_order_relaxed);
        playing = m_anchor_playing.load(std::memory_order_relaxed);
        gen = m_anchor_gen.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_anchor_seq.load(std::memory_order_relaxed) == seq) break;
    }
    // The render thread has not applied the latest Play yet: still at the requested start.
    if (!playing || gen != m_ui_gen.load(std::memory_order_relaxed)) return m_paused_pos.load();
    int64_t dev_frames = 0, qpc = 0;
    if (FAILED(m_render.GetClock(dev_frames, qpc))) return m_paused_pos.load();
    const int64_t now = Clock::QpcNow();
    int64_t since_us = std::clamp<int64_t>((now - qpc) * 1'000'000 / Clock::QpcFrequency(), 0, kMaxExtrapolateUs);
    const double played = static_cast<double>(dev_frames - frames_at_play) + since_us * kRate / 1e6;
    TimeUs pos = from + static_cast<TimeUs>(std::max(played, 0.0) * rate * 1e6 / kRate);
    // Monotonic while the anchor is unchanged.
    if (m_last_pos_seq.load(std::memory_order_relaxed) == seq) pos = std::max(pos, m_last_pos.load(std::memory_order_relaxed));
    m_last_pos.store(pos, std::memory_order_relaxed);
    m_last_pos_seq.store(seq, std::memory_order_relaxed);
    return pos;
}

void AudioEngine::Callback::Render(float* out, uint32_t frames, int /*channels*/, int64_t device_position_frames) {
    engine->RenderOnThread(out, frames, device_position_frames);
}

void AudioEngine::RenderOnThread(float* out, uint32_t frames, int64_t device_position_frames) {
    if (m_cmd_lock.TryLockExclusive()) {
        const Command c = m_cmd;
        m_cmd_lock.UnlockExclusive();
        if (c.gen != m_applied_gen) {
            m_applied_gen = c.gen;
            m_playing = c.playing;
            if (c.playing) {
                m_from_us = c.from_us;
                m_rate = c.rate;
                m_pulled = 0;
                m_vs_frac = 0;
                m_vs_prev[0] = m_vs_prev[1] = 0;
            }
            m_anchor_seq.fetch_add(1, std::memory_order_acq_rel);   // odd: writing
            m_anchor_from.store(m_from_us, std::memory_order_relaxed);
            m_anchor_rate.store(m_rate, std::memory_order_relaxed);
            m_anchor_frames.store(device_position_frames, std::memory_order_relaxed);
            m_anchor_playing.store(m_playing, std::memory_order_relaxed);
            m_anchor_gen.store(c.gen, std::memory_order_relaxed);
            m_anchor_seq.fetch_add(1, std::memory_order_release);   // even: done
        }
    }
    float peak = 0;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(frames - done, kMaxChunk);
        float* chunk = out + static_cast<size_t>(done) * kChannels;
        if (!m_playing || !m_source) std::memset(chunk, 0, sizeof(float) * n * kChannels);
        else RenderChunk(chunk, n);
        // Volume with a 5 ms ramp toward the target.
        const float target = m_muted.load(std::memory_order_relaxed) ? 0.0f : m_volume.load(std::memory_order_relaxed);
        const float step = n / kRampFrames;
        const float next = m_gain + std::clamp(target - m_gain, -step, step);
        if (m_gain != 1.0f || next != 1.0f) audio::dsp::ApplyGainRamp(chunk, n, kChannels, m_gain, next);
        m_gain = next;
        peak = std::max(peak, audio::dsp::PeakAbs(chunk, static_cast<size_t>(n) * kChannels));
        done += n;
    }
    m_peak.store(peak, std::memory_order_relaxed);
}

void AudioEngine::RenderChunk(float* out, uint32_t frames) {
    const TimeUs from = m_from_us + m_pulled * 1'000'000 / kRate;
    if (m_rate == 1.0) {
        const uint32_t got = std::min(m_source->Pull(out, frames, from, 1.0), frames);
        if (got < frames) std::memset(out + static_cast<size_t>(got) * kChannels, 0, sizeof(float) * (frames - got) * kChannels);
        m_pulled += frames;
        return;
    }
    // Varispeed: output frame i reads source position m_vs_frac + i * rate, where index 0 is the previous chunk's last
    // source frame and 1..n the frames pulled now.
    const double end_pos = m_vs_frac + frames * m_rate;
    const int64_t need = std::max<int64_t>(static_cast<int64_t>(std::floor(m_vs_frac + (frames - 1) * m_rate)) + 1,
                                           static_cast<int64_t>(std::floor(end_pos)));
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(need, static_cast<int64_t>(m_vs.size() / kChannels) - 1));
    float* vs = m_vs.data();
    vs[0] = m_vs_prev[0];
    vs[1] = m_vs_prev[1];
    const uint32_t got = std::min(m_source->Pull(vs + kChannels, n, from, m_rate), n);
    if (got < n) std::memset(vs + static_cast<size_t>(1 + got) * kChannels, 0, sizeof(float) * (n - got) * kChannels);
    for (uint32_t i = 0; i < frames; ++i) {
        const double pos = m_vs_frac + i * m_rate;
        const uint32_t k = std::min(static_cast<uint32_t>(pos), n - 1);
        const float f = static_cast<float>(pos - k);
        for (int c = 0; c < kChannels; ++c)
            out[i * kChannels + c] = vs[k * kChannels + c] * (1.0f - f) + vs[(k + 1) * kChannels + c] * f;
    }
    const uint32_t consumed = static_cast<uint32_t>(std::min<double>(std::floor(end_pos), n));
    m_vs_prev[0] = vs[consumed * kChannels];
    m_vs_prev[1] = vs[consumed * kChannels + 1];
    m_vs_frac = end_pos - consumed;
    m_pulled += n;
}

} // namespace ct::playback
