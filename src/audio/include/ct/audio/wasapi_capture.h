#pragma once
// ct/audio/wasapi_capture.h
// Microphone capture: WASAPI shared mode, event-driven, on thread "ct-audio-mic" (TIME_CRITICAL, MMCSS
// "Pro Audio", MTA). Delivers gapless 48 kHz stereo float packets on the session clock to an AudioSink.
// Thread model: Init/Start/SetPaused/Stop from one owner thread with COM initialized as MTA (not the UI's STA);
// Levels/DeviceName/LastError/GetStats from any thread. Lifetime: the AudioDevices passed to Init must outlive
// this object. Start may be called again after Stop.

#include <ct/audio/audio_sink.h>
#include <ct/audio/devices.h>
#include <ct/audio/meter.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ct::audio {

namespace detail { class CaptureEngine; }

class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    HRESULT Init(const CaptureConfig& config, AudioDevices& devices);
    // epoch_qpc: session epoch (QPC ticks). The first packet may arrive up to 30 ms later; the stream is padded
    // with silence from t = 0.
    bool Start(int64_t epoch_qpc, AudioSink& sink);
    // Packets are consumed but not delivered while paused; on resume the stream continues at the media time where
    // it paused (the pause duration is subtracted). at_qpc = 0 means now; pass the session's pause/resume QPC so all
    // streams shift by the same amount.
    void SetPaused(bool paused, int64_t at_qpc = 0);
    void Stop();

    const Meter& Levels() const;
    std::string DeviceName() const;
    HRESULT LastError() const;
    CaptureStats GetStats() const;

private:
    std::unique_ptr<detail::CaptureEngine> m_engine;
};

} // namespace ct::audio
