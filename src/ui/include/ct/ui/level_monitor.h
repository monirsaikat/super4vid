#pragma once
// ct/ui/level_monitor.h
// Live input level for the UI (P11 11.6 "meters test"; the recorder bar's idle mic/system meters): opens a WASAPI
// microphone or loopback stream with a null sink on the IO pool and exposes its peak level. Nothing is recorded.
// Thread model: Start/Stop/PeakDb/State on the UI thread; the device is opened and closed on the IO pool (opening
// and Stop() block on WASAPI). Lifetime: the destructor hands the stream to the IO pool to close it.

#include <ct/platform/win.h>

#include <memory>
#include <string>

namespace ct::audio { class AudioDevices; }

namespace ct::ui {

class LevelMonitor {
public:
    enum class Source { Microphone, SystemAudio };
    enum class Status { Stopped, Starting, Running, Failed };

    LevelMonitor();
    ~LevelMonitor();
    LevelMonitor(const LevelMonitor&) = delete;
    LevelMonitor& operator=(const LevelMonitor&) = delete;

    // device_id empty = the default endpoint. `devices` may be null: the monitor creates its own on the IO pool.
    void Start(Source source, std::wstring device_id, std::shared_ptr<audio::AudioDevices> devices = nullptr);
    void Stop();
    Status GetStatus() const;
    bool Active() const { return GetStatus() == Status::Starting || GetStatus() == Status::Running; }
    Source GetSource() const { return m_source; }
    HRESULT LastError() const;   // why it Failed
    // Peak dBFS of the most recent packet (floor -120 while not running).
    float PeakDb() const;
    // The meter value to draw: the peak with a 20 dB/s fall-off (docs/05), advanced by dt seconds.
    float Shown(float dt_s);
    static float Falloff(float shown_db, float new_db, float dt_s);

private:
    struct State;
    std::shared_ptr<State> m_state;
    Source m_source = Source::Microphone;
    float m_shown = -120.0f;
};

} // namespace ct::ui
