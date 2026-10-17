#pragma once
// Private: the capture pipeline shared by WasapiCapture (event-driven microphone) and WasapiLoopback (polled system
// audio). One engine owns one IAudioClient and one capture thread:
//   GetBuffer -> convert to float stereo -> (resample to 48 kHz) -> Meter -> StreamAligner -> 10 ms pool packets
//   -> AudioSink::OnAudio.
// Thread model: Init/Start/Stop/SetPaused from the owner thread (an MTA thread); the device is reopened on the
// capture thread itself; Levels/DeviceName/LastError/Stats from any thread.

#include <ct/audio/audio_sink.h>
#include <ct/audio/devices.h>
#include <ct/audio/meter.h>
#include <ct/audio/pause_timeline.h>
#include <ct/audio/resampler.h>
#include <ct/audio/stream_aligner.h>
#include <ct/audio/wasapi_render.h>
#include <ct/core/object_pool.h>
#include <ct/core/stats.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>

#include "wasapi_internal.h"

#include <atomic>
#include <memory>
#include <string>

namespace ct::audio::detail {

enum class EngineMode { Mic, Loopback };

class CaptureEngine final : public AudioDevices::Listener {
public:
    explicit CaptureEngine(EngineMode mode);
    ~CaptureEngine() override;
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    HRESULT Init(const CaptureConfig& config, AudioDevices& devices);
    bool Start(int64_t epoch_qpc, AudioSink& sink);
    void SetPaused(bool paused, int64_t at_qpc);
    void Stop();
    void SwitchDevice(const std::wstring& id);

    const Meter& Levels() const { return m_meter; }
    std::string DeviceName() const;
    HRESULT LastError() const { return m_last_error.load(std::memory_order_relaxed); }
    CaptureStats Stats() const;

    void OnDefaultDeviceChanged(bool capture, const std::wstring& new_id) override;
    void OnDeviceRemoved(const std::wstring& id) override;

private:
    struct PacketBuffer {
        std::unique_ptr<float[]> samples;
    };

    bool IsMic() const { return m_mode == EngineMode::Mic; }
    const char* StatsPrefix() const { return IsMic() ? "audio.mic" : "audio.sys"; }
    HRESULT Open(const std::wstring& id);
    HRESULT InitializeClient(IMMDevice* device);
    void Close();
    void ThreadMain();
    void ReadPackets();
    void ProcessDeviceFrames(const BYTE* data, uint32_t frames, bool silent, bool timestamp_valid, int64_t abs_us,
                             bool discontinuity);
    void FeedRun(const float* stereo, uint32_t frames, TimeUs t_us, bool resync);
    void ConvertToStereo(const BYTE* data, uint32_t frames, float* out) const;
    void PushAligned(TimeUs t_us, const float* stereo, uint32_t frames, bool discontinuity);
    void OnAligned(const float* p, uint32_t frames, bool fill);
    void FlushPending();
    void Reopen();
    void CheckStarvation();
    void SetError(HRESULT hr);
    void StartKeepAlive();
    void StopKeepAlive();

    const EngineMode m_mode;
    CaptureConfig m_config;
    AudioDevices* m_devices = nullptr;
    AudioSink* m_sink = nullptr;

    // Device (touched by the owner thread before Start and after Stop, by the capture thread in between).
    Microsoft::WRL::ComPtr<IAudioClient> m_client;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_capture;
    std::wstring m_open_id;           // endpoint id of the open device
    uint32_t m_dev_rate = 0;
    int m_dev_channels = 0;
    uint32_t m_dev_block_align = 0;
    SampleType m_dev_type = SampleType::Unsupported;
    bool m_logged_fallback = false;

    // Loopback only: a silent render stream on the captured endpoint keeps loopback packets flowing.
    struct SilenceCallback final : WasapiRender::Callback {
        void Render(float* interleaved, uint32_t frames, int channels, int64_t) override;
    };
    SilenceCallback m_silence;
    WasapiRender m_keepalive;

    // Scratch (allocated in Init, sized for the largest packet we convert at once).
    static constexpr uint32_t kScratchFrames = 19200;   // 100 ms at 192 kHz; longer packets are split
    std::unique_ptr<float[]> m_stereo;

    // Fallback when the engine rejects our 48 kHz request: device-rate stereo -> 48 kHz.
    static constexpr uint32_t kResampledFrames = 38400;
    std::unique_ptr<float[]> m_resampled;
    Resampler m_resampler;
    bool m_resampling = false;
    uint32_t m_resample_chunk = 0;   // device frames per Process call so the output always fits
    int64_t m_rs_in = 0;             // device frames fed since the last resampler reset
    int64_t m_rs_out = 0;            // 48 kHz frames produced since the last resampler reset
    bool m_rs_resync = false;        // capture thread only

    StreamAligner m_aligner;
    Meter m_meter;
    ObjectPool<PacketBuffer> m_pool;
    uint32_t m_pending_index = 0;
    PacketBuffer* m_pending = nullptr;
    uint32_t m_pending_frames = 0;
    bool m_pending_fill = false;
    int64_t m_pending_first_frame = 0;
    int64_t m_out_frames = 0;   // aligned frames received from the aligner (== its FramesEmitted)
    uint32_t m_packet_frames = 480;

    // Thread and wake-ups.
    Thread m_thread;
    Event m_stop{true, false};
    Event m_wake{false, false};
    Event m_data{false, false};   // WASAPI event (mic)
    HighResTimer m_timer;         // 5 ms poll (loopback)
    std::atomic<bool> m_running{false};

    // Clock: media_us = capture_abs_us - m_epoch_us - (pauses that ended before the frame was captured).
    int64_t m_epoch_us = 0;
    PauseTimeline m_pauses;
    int64_t m_last_packet_qpc = 0;   // capture thread only
    bool m_force_resync = false;     // capture thread only: next device packet resyncs exactly
    bool m_device_lost = false;      // capture thread only
    int64_t m_next_retry_qpc = 0;    // capture thread only
    bool m_error_reported = false;   // capture thread only

    // Device-change requests from the notification thread.
    std::atomic<bool> m_reinit_requested{false};
    SrwLock m_request_lock;
    std::wstring m_requested_id;   // guarded by m_request_lock; empty = default endpoint
    std::wstring m_watched_id;     // guarded by m_request_lock; endpoint id whose removal triggers a reopen
    std::wstring m_target_id;      // guarded by m_request_lock; the endpoint the owner asked for (empty = default)

    mutable SrwLock m_name_lock;
    std::string m_name;   // guarded by m_name_lock

    std::atomic<HRESULT> m_last_error{0};
    std::atomic<uint64_t> m_st_device_packets{0}, m_st_packets{0}, m_st_frames{0}, m_st_pad{0}, m_st_inserted{0},
        m_st_dropped{0}, m_st_sink_dropped{0}, m_st_disc{0}, m_st_reinits{0};
    std::atomic<double> m_st_drift_ms{0};
    std::atomic<int64_t> m_st_last_end_us{0};
    std::atomic<bool> m_st_open{false};
    std::atomic<double> m_st_reopen_ms{0};
    ct::Stats::Counter* m_pool_dropped = nullptr;
};

} // namespace ct::audio::detail
