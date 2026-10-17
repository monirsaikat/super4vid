#include <ct/audio/meter.h>

#include <cmath>

namespace ct::audio {

Meter::Meter() { Reset(); }

float Meter::ToDb(float linear) {
    const float x = linear > 1e-6f ? linear : 1e-6f;
    return 20.0f * std::log10(x);
}

void Meter::Reset() {
    for (int c = 0; c < kMaxChannels; ++c) {
        m_peak_db[c].store(kFloorDb, std::memory_order_relaxed);
        m_rms_db[c].store(kFloorDb, std::memory_order_relaxed);
    }
}

void Meter::Push(const float* interleaved, size_t frames, int channels) {
    if (!interleaved || frames == 0 || channels <= 0) return;
    const int n = channels < kMaxChannels ? channels : kMaxChannels;
    float peak[kMaxChannels] = {};
    double sum[kMaxChannels] = {};
    const size_t stride = static_cast<size_t>(channels);
    for (size_t f = 0; f < frames; ++f) {
        const float* p = interleaved + f * stride;
        for (int c = 0; c < n; ++c) {
            const float a = std::fabs(p[c]);
            peak[c] = a > peak[c] ? a : peak[c];
            sum[c] += static_cast<double>(p[c]) * p[c];
        }
    }
    for (int c = 0; c < n; ++c) {
        m_peak_db[c].store(ToDb(peak[c]), std::memory_order_relaxed);
        m_rms_db[c].store(ToDb(static_cast<float>(std::sqrt(sum[c] / static_cast<double>(frames)))),
                          std::memory_order_relaxed);
    }
    m_channels.store(n, std::memory_order_relaxed);
}

float Meter::PeakDb(int ch) const {
    if (ch < 0 || ch >= kMaxChannels) return kFloorDb;
    return m_peak_db[ch].load(std::memory_order_relaxed);
}

float Meter::RmsDb(int ch) const {
    if (ch < 0 || ch >= kMaxChannels) return kFloorDb;
    return m_rms_db[ch].load(std::memory_order_relaxed);
}

} // namespace ct::audio
