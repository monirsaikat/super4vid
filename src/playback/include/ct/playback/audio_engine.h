#pragma once
// ct/playback/audio_engine.h
// Editor/player audio output: a WASAPI render stream (20 ms, float 48 kHz stereo) whose callback pulls mixed audio from
// a Source, applies volume (5 ms ramps) and varispeed (rate != 1: linear resampling with pitch change, for J/K/L), and
// measures the output peak. While playing it is the master clock: PositionUs() follows IAudioClock.
// Thread model: Init/Start/Stop on one owner thread (MTA COM); Play/Pause/SetVolume/SetMuted from one control thread
// (UI); PositionUs/Playing/Volume/PeakLevel from any thread. Source::Pull runs on the render thread ("ct-audio-out",
// MMCSS Pro Audio): it must not block, allocate or call MF (feed it from a decoder thread through a ring).
// Lifetime: the Source must outlive Stop().

#include <ct/audio/devices.h>
#include <ct/audio/wasapi_render.h>
#include <ct/core/sync.h>
#include <ct/core/types.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ct::playback {

class AudioEngine {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr double kMinRate = 0.25, kMaxRate = 4.0;

    struct Source {
        // Fill `frames` interleaved stereo frames starting at media time from_us (frames are contiguous between calls
        // until the next Play). Return the frames written; the engine zero-fills the rest. `rate` is informative: the
        // engine already asks for frames * rate source frames.
        virtual uint32_t Pull(float* interleaved, uint32_t frames, TimeUs from_us, double rate) = 0;
        virtual ~Source() = default;
    };

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    HRESULT Init(audio::AudioDevices&);   // default render endpoint (eConsole)
    // Preferred endpoint id (from settings); empty, unknown or unopenable falls back to the default device, which is
    // not an error. DeviceId() is the endpoint actually opened.
    HRESULT Init(audio::AudioDevices&, const std::wstring& preferred_device);
    const std::wstring& DeviceId() const { return m_device_id; }
    bool Start(Source&);
    void Stop();

    // rate is clamped to [0.25, 4]; negative rates (reverse J) are silent: PlaybackClock uses QPC for them.
    void Play(TimeUs from_us, double rate);
    void Pause();
    bool Playing() const { return m_playing_ui.load(std::memory_order_acquire); }
    // from_us + (device_frames_now - device_frames_at_play) * rate / 48000, interpolated with QPC between IAudioClock
    // readings, never going backwards while playing. IAudioClock already reports the sample currently audible, so no
    // extra output latency is subtracted (DECISIONS.md P06 6.5). Paused: the position where playback stopped.
    TimeUs PositionUs() const;

    void SetVolume(float);   // linear gain 0..2
    void SetMuted(bool);
    float Volume() const { return m_volume.load(std::memory_order_relaxed); }
    // Additions.
    bool Muted() const { return m_muted.load(std::memory_order_relaxed); }
    float PeakLevel() const { return m_peak.load(std::memory_order_relaxed); }   // linear peak of the last buffer
    bool DeviceOk() const;      // started and no device error
    double LatencyMs() const { return m_render.LatencyMs(); }
    std::string DeviceName() const { return m_render.DeviceName(); }

private:
    struct Callback final : audio::WasapiRender::Callback {
        AudioEngine* engine = nullptr;
        void Render(float* out, uint32_t frames, int channels, int64_t device_position_frames) override;
    };
    void RenderOnThread(float* out, uint32_t frames, int64_t device_position_frames);
    void RenderChunk(float* out, uint32_t frames);

    audio::WasapiRender m_render;
    std::wstring m_device_id;   // the endpoint Init opened
    Callback m_callback;
    Source* m_source = nullptr;
    bool m_started = false;

    // Control -> render thread command (the render thread only try-locks).
    SrwLock m_cmd_lock;
    struct Command { TimeUs from_us = 0; double rate = 1.0; bool playing = false; uint64_t gen = 0; } m_cmd;

    // Render thread state.
    uint64_t m_applied_gen = 0;
    bool m_playing = false;
    double m_rate = 1.0;
    TimeUs m_from_us = 0;
    int64_t m_pulled = 0;            // source frames pulled since Play
    double m_vs_frac = 0;            // varispeed read position after m_vs[0]
    float m_vs_prev[2] = {0, 0};     // last source frame of the previous chunk
    std::vector<float> m_vs;         // varispeed scratch: prev frame + pulled frames
    float m_gain = 1.0f;             // current (ramped) gain

    // Clock anchor published by the render thread (seqlock: odd = being written).
    std::atomic<uint64_t> m_anchor_seq{0};
    std::atomic<int64_t> m_anchor_from{0};
    std::atomic<double> m_anchor_rate{1.0};
    std::atomic<int64_t> m_anchor_frames{0};
    std::atomic<bool> m_anchor_playing{false};
    std::atomic<uint64_t> m_anchor_gen{0};   // command generation the anchor belongs to
    std::atomic<uint64_t> m_ui_gen{0};       // latest command generation sent
    mutable std::atomic<int64_t> m_last_pos{0};
    mutable std::atomic<uint64_t> m_last_pos_seq{0};

    std::atomic<bool> m_playing_ui{false};
    std::atomic<int64_t> m_paused_pos{0};
    std::atomic<float> m_volume{1.0f};
    std::atomic<bool> m_muted{false};
    std::atomic<float> m_peak{0.0f};
};

} // namespace ct::playback
