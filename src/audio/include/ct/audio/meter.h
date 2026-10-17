#pragma once
// ct/audio/meter.h
// Live level meter: peak and RMS per channel of the most recent packet, in dBFS with a -120 dB floor.
// Thread model: Push from the producing audio thread; PeakDb/RmsDb from any thread (std::atomic<float>, relaxed).
// Readers (UI) apply their own fall-off, e.g. 20 dB/s: shown = max(new, shown - 20 * dt).

#include <atomic>
#include <cstddef>

namespace ct::audio {

class Meter {
public:
    static constexpr int kMaxChannels = 8;
    static constexpr float kFloorDb = -120.0f;

    Meter();
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;

    void Push(const float* interleaved, size_t frames, int channels);
    float PeakDb(int ch) const;
    float RmsDb(int ch) const;
    int Channels() const { return m_channels.load(std::memory_order_relaxed); }
    void Reset();   // back to the floor

    // 20 * log10(max(x, 1e-6)), i.e. never below -120 dB.
    static float ToDb(float linear);

private:
    std::atomic<float> m_peak_db[kMaxChannels];
    std::atomic<float> m_rms_db[kMaxChannels];
    std::atomic<int> m_channels{0};
};

} // namespace ct::audio
