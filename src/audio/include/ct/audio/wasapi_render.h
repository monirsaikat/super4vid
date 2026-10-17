#pragma once
// ct/audio/wasapi_render.h
// WASAPI shared-mode render stream, event-driven, float 48 kHz stereo (AUTOCONVERTPCM converts to the mix format).
// Used by the loopback keep-alive, audio_bench and the editor's audio engine (P06).
// Thread model: Init/Start/Stop from one owner thread with COM initialized (MTA); the callback runs on the render
// thread (MMCSS "Pro Audio", TIME_CRITICAL) and must not block or allocate; GetClock/SampleRate/Channels/LatencyMs/
// LastError from any thread. Lifetime: the callback must outlive Stop(). Start may be called again after Stop.

#include <ct/core/sync.h>
#include <ct/core/thread.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::audio {

class WasapiRender {
public:
    struct Callback {
        // Fill `frames` interleaved frames. device_position_frames = frames written to the stream before this
        // buffer (0 for the prefill done in Start()).
        virtual void Render(float* interleaved, uint32_t frames, int channels, int64_t device_position_frames) = 0;
        virtual ~Callback() = default;
    };

    WasapiRender();
    ~WasapiRender();
    WasapiRender(const WasapiRender&) = delete;
    WasapiRender& operator=(const WasapiRender&) = delete;

    // device_id empty = default render endpoint (eConsole). buffer_ms is clamped to 20..100.
    HRESULT Init(const std::wstring& device_id, uint32_t buffer_ms, const char* thread_name = "ct-audio-out");
    bool Start(Callback& callback);   // prefills one buffer through the callback, then starts the device
    void Stop();
    // IAudioClock::GetPosition converted to frames at SampleRate(), plus the QPC tick value of that reading.
    HRESULT GetClock(int64_t& device_frames, int64_t& qpc) const;
    uint32_t SampleRate() const { return 48000; }
    int Channels() const { return 2; }
    double LatencyMs() const;        // shared buffer + stream latency reported by the engine
    HRESULT LastError() const { return m_last_error.load(std::memory_order_relaxed); }
    std::string DeviceName() const { return m_name; }
    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    void ThreadMain();
    HRESULT FillAvailable();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_thread_name;
    std::string m_name;
    Thread m_thread;
    Event m_stop{true, false};
    Callback* m_callback = nullptr;
    int64_t m_written = 0;   // render thread only (after Start)
    std::atomic<bool> m_running{false};
    std::atomic<HRESULT> m_last_error{0};
};

} // namespace ct::audio
