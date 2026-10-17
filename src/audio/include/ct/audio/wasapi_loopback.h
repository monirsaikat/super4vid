#pragma once
// ct/audio/wasapi_loopback.h
// System audio capture: WASAPI loopback on a render endpoint, polled every 5 ms on thread "ct-audio-sys"
// (TIME_CRITICAL, MMCSS "Pro Audio", MTA), plus a silent keep-alive WasapiRender stream on the same endpoint so the
// engine keeps producing loopback packets (silence included) when nothing else plays. Gaps are filled with silence
// by the StreamAligner; output is gapless 48 kHz stereo float on the session clock.
// Thread model and lifetime: as WasapiCapture.

#include <ct/audio/audio_sink.h>
#include <ct/audio/devices.h>
#include <ct/audio/meter.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ct::audio {

namespace detail { class CaptureEngine; }

class WasapiLoopback {
public:
    WasapiLoopback();
    ~WasapiLoopback();
    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    HRESULT Init(const CaptureConfig& config, AudioDevices& devices);   // device_id = render endpoint id
    bool Start(int64_t epoch_qpc, AudioSink& sink);
    void SetPaused(bool paused, int64_t at_qpc = 0);
    void Stop();
    // Moves the running stream to another endpoint (empty = the default, then following default changes) on the
    // capture thread. The switch time becomes silence at the correct position; timestamps never shift.
    void SwitchDevice(const std::wstring& device_id);

    const Meter& Levels() const;
    std::string DeviceName() const;
    HRESULT LastError() const;
    CaptureStats GetStats() const;

private:
    std::unique_ptr<detail::CaptureEngine> m_engine;
};

} // namespace ct::audio
