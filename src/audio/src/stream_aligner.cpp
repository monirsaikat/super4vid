#include <ct/audio/stream_aligner.h>

#include <cstring>

namespace ct::audio {

void StreamAligner::Reset(int sample_rate, int channels, TimeUs start_us) {
    m_rate = sample_rate > 0 ? sample_rate : 48000;
    m_channels = channels > 0 ? channels : 2;
    // 100 ms of zeros: silence runs longer than that are emitted in several packets.
    const uint32_t zero_frames = static_cast<uint32_t>(m_rate / 10);
    const size_t samples = static_cast<size_t>(zero_frames) * static_cast<size_t>(m_channels);
    if (!m_zeros || samples != m_zero_samples) {
        m_zeros = std::make_unique<float[]>(samples);
        m_zero_samples = samples;
    }
    std::memset(m_zeros.get(), 0, sizeof(float) * samples);
    m_zero_frames = zero_frames;
    m_start_us = start_us;
    m_next_frame = 0;
    m_inserted = 0;
    m_dropped = 0;
    m_leading_pad = 0;
    m_frames_since_correction = 0;
    m_drift_us = 0;
    m_started = false;
    m_resync_next = false;
}

} // namespace ct::audio
