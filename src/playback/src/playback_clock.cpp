#include <ct/playback/playback_clock.h>

#include <ct/core/time.h>
#include <ct/playback/audio_engine.h>

#include <cmath>

namespace ct::playback {

double PlaybackClock::ClampRate(double rate) {
    if (rate == 0 || std::isnan(rate)) return 1.0;
    const double mag = std::fmin(std::fmax(std::fabs(rate), kMinRate), kMaxRate);
    return rate < 0 ? -mag : mag;
}

void PlaybackClock::SetQpcSourceForTest(int64_t (*qpc_now)(), int64_t qpc_frequency) {
    m_test_qpc = qpc_now;
    m_test_freq = qpc_frequency;
}

int64_t PlaybackClock::QpcNow() const { return m_test_qpc ? m_test_qpc() : Clock::QpcNow(); }
int64_t PlaybackClock::QpcFreq() const { return m_test_qpc ? m_test_freq : Clock::QpcFrequency(); }

void PlaybackClock::Attach(AudioEngine* engine) {
    const TimeUs now = Now();
    if (m_engine && m_using_audio) m_engine->Pause();
    m_engine = engine;
    Restart(now);
}

void PlaybackClock::Restart(TimeUs from_us) {
    m_base_us = from_us;
    m_qpc_play = QpcNow();
    const bool audio = m_playing && m_engine && m_rate > 0 && m_engine->DeviceOk();
    if (audio) m_engine->Play(from_us, m_rate);
    else if (m_engine && m_using_audio) m_engine->Pause();
    m_using_audio = audio;
}

void PlaybackClock::Play(TimeUs from_us, double rate) {
    m_rate = ClampRate(rate);
    m_playing = true;
    Restart(from_us);
}

void PlaybackClock::Pause() {
    if (!m_playing) return;
    const TimeUs now = Now();
    m_playing = false;
    Restart(now);
}

void PlaybackClock::Seek(TimeUs t) { Restart(t); }

void PlaybackClock::SetRate(double rate) {
    const TimeUs now = Now();
    m_rate = ClampRate(rate);
    Restart(now);
}

TimeUs PlaybackClock::Now() const {
    if (!m_playing) return m_base_us;
    if (m_using_audio && m_engine) return m_engine->PositionUs();
    const double elapsed_us = static_cast<double>(QpcNow() - m_qpc_play) * 1e6 / static_cast<double>(QpcFreq());
    return m_base_us + static_cast<TimeUs>(elapsed_us * m_rate);
}

} // namespace ct::playback
