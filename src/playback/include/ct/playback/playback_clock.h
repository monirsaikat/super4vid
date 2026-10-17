#pragma once
// ct/playback/playback_clock.h
// The playhead clock. While playing forward at 0.25x..4x with an attached, working AudioEngine it is the audio
// position (the audio device is the master); otherwise QPC: now = base + (qpc_now - qpc_play) * rate.
// Thread model: one control thread (UI) calls everything; Now() is also safe from other threads (reads atomics of the
// engine) but returns a consistent value only relative to the control thread's last call.

#include <ct/core/types.h>

#include <cstdint>

namespace ct::playback {

class AudioEngine;

class PlaybackClock {
public:
    static constexpr double kMinRate = 0.25, kMaxRate = 4.0;   // |rate|; negative = reverse (QPC only)

    void Attach(AudioEngine* engine_or_null);
    void Play(TimeUs from_us, double rate);
    void Pause();
    void Seek(TimeUs);        // keeps playing if playing
    void SetRate(double);     // keeps the current position
    TimeUs Now() const;
    bool Playing() const { return m_playing; }
    double Rate() const { return m_rate; }
    // Addition: true while Now() comes from the audio engine.
    bool UsingAudio() const { return m_using_audio; }
    static double ClampRate(double rate);

    // Tests: replace QueryPerformanceCounter (nullptr restores it).
    void SetQpcSourceForTest(int64_t (*qpc_now)(), int64_t qpc_frequency);

private:
    int64_t QpcNow() const;
    int64_t QpcFreq() const;
    void Restart(TimeUs from_us);   // re-anchor at from_us with the current rate/playing state

    AudioEngine* m_engine = nullptr;
    bool m_playing = false;
    bool m_using_audio = false;
    double m_rate = 1.0;
    TimeUs m_base_us = 0;
    int64_t m_qpc_play = 0;
    int64_t (*m_test_qpc)() = nullptr;
    int64_t m_test_freq = 0;
};

} // namespace ct::playback
