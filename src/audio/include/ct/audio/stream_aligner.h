#pragma once
// ct/audio/stream_aligner.h
// Makes a captured stream gapless and monotonic using the device's QPC timestamps. Pure logic, no OS calls.
// Thread model: one thread (the capture thread). Lifetime: Reset allocates the silence buffer; Push/FillSilenceUntil
// never allocate.
//
// The output stream starts at `start_us` (0 = the session epoch) and its frame n has the time
// start_us + floor(n * 1e6 / rate). Each pushed packet is compared with that position (d = t - expected):
//   - first packet / after a discontinuity: exact resync when |d| >= 1 frame (silence before it, or its early
//     frames dropped);
//   - |d| > kResyncUs (2 ms): a gap is filled with exactly round(d) frames of silence, an overlap drops round(-d)
//     frames;
//   - kDriftUs (1 ms) < |d| <= kResyncUs: slow clock drift; one frame is repeated or dropped at most once per
//     10 ms of output, so the stream converges without audible jumps.

#include <ct/core/types.h>

#include <cstdint>
#include <memory>

namespace ct::audio {

class StreamAligner {
public:
    static constexpr TimeUs kDriftUs = 1000;
    static constexpr TimeUs kResyncUs = 2000;

    // start_us: media time of the stream's first frame (silence is padded from there to the first packet).
    void Reset(int sample_rate, int channels, TimeUs start_us = 0);

    // Input: a device packet with its QPC-derived timestamp (media time). Output via
    // emit(TimeUs t, const float* interleaved, uint32_t frames, bool silence_fill): zero or more packets whose
    // timestamps are exactly contiguous (t_next = t_prev + frames/rate).
    template <class Emit>
    void Push(TimeUs t_us, const float* interleaved, uint32_t frames, bool discontinuity, Emit emit);

    // Starvation (no packets): inserts silence so the stream reaches t_us (rounded down to a frame).
    template <class Emit>
    void FillSilenceUntil(TimeUs t_us, Emit emit);

    TimeUs NextExpectedUs() const { return TimeOfFrame(m_next_frame); }
    int64_t FramesEmitted() const { return m_next_frame; }
    int64_t TotalInserted() const { return m_inserted; }    // excludes the leading pad
    int64_t TotalDropped() const { return m_dropped; }
    int64_t LeadingPadFrames() const { return m_leading_pad; }
    double DriftMs() const { return static_cast<double>(m_drift_us) / 1000.0; }
    bool Started() const { return m_started; }
    int SampleRate() const { return m_rate; }
    int Channels() const { return m_channels; }

private:
    TimeUs TimeOfFrame(int64_t n) const { return m_start_us + n * kUsPerSecond / m_rate; }
    int64_t UsToFramesRounded(TimeUs us) const {
        const int64_t num = us * m_rate;
        return num >= 0 ? (num + kUsPerSecond / 2) / kUsPerSecond : -((-num + kUsPerSecond / 2) / kUsPerSecond);
    }
    template <class Emit>
    void EmitSilence(int64_t frames, Emit& emit);
    template <class Emit>
    void EmitData(const float* p, uint32_t frames, Emit& emit);

    std::unique_ptr<float[]> m_zeros;
    uint32_t m_zero_frames = 0;
    size_t m_zero_samples = 0;
    int m_rate = 48000;
    int m_channels = 2;
    TimeUs m_start_us = 0;
    int64_t m_next_frame = 0;
    int64_t m_inserted = 0;
    int64_t m_dropped = 0;
    int64_t m_leading_pad = 0;
    int64_t m_frames_since_correction = 0;
    TimeUs m_drift_us = 0;
    bool m_started = false;
    bool m_resync_next = false;
};

template <class Emit>
void StreamAligner::EmitSilence(int64_t frames, Emit& emit) {
    while (frames > 0) {
        const uint32_t n = frames > m_zero_frames ? m_zero_frames : static_cast<uint32_t>(frames);
        emit(NextExpectedUs(), m_zeros.get(), n, true);
        m_next_frame += n;
        m_frames_since_correction += n;
        frames -= n;
    }
}

template <class Emit>
void StreamAligner::EmitData(const float* p, uint32_t frames, Emit& emit) {
    if (frames == 0) return;
    emit(NextExpectedUs(), p, frames, false);
    m_next_frame += frames;
    m_frames_since_correction += frames;
}

template <class Emit>
void StreamAligner::Push(TimeUs t_us, const float* interleaved, uint32_t frames, bool discontinuity, Emit emit) {
    if (frames == 0 || !interleaved) return;
    const TimeUs d = t_us - NextExpectedUs();
    m_drift_us = d;
    const int64_t d_frames = UsToFramesRounded(d);
    const bool exact = !m_started || discontinuity || m_resync_next;
    m_resync_next = false;
    if ((exact && d_frames != 0) || d > kResyncUs || d < -kResyncUs) {
        if (d_frames > 0) {
            if (m_started) m_inserted += d_frames;
            else m_leading_pad += d_frames;
            EmitSilence(d_frames, emit);
        } else if (d_frames < 0) {
            const int64_t drop = -d_frames;
            m_started = true;
            if (drop >= frames) {
                m_dropped += frames;
                return;
            }
            m_dropped += drop;
            interleaved += drop * m_channels;
            frames -= static_cast<uint32_t>(drop);
        }
        m_started = true;
        m_drift_us = 0;
        m_frames_since_correction = 0;
        EmitData(interleaved, frames, emit);
        return;
    }
    m_started = true;
    if ((d > kDriftUs || d < -kDriftUs) && m_frames_since_correction >= m_rate / 100) {
        m_frames_since_correction = 0;
        if (d > 0) {
            // Behind the device clock: repeat the first frame once.
            ++m_inserted;
            EmitData(interleaved, 1, emit);
            m_frames_since_correction = 0;
        } else if (frames > 1) {
            // Ahead of the device clock: drop the first frame once.
            ++m_dropped;
            interleaved += m_channels;
            --frames;
        }
    }
    EmitData(interleaved, frames, emit);
}

template <class Emit>
void StreamAligner::FillSilenceUntil(TimeUs t_us, Emit emit) {
    const TimeUs d = t_us - NextExpectedUs();
    if (d <= 0) return;
    const int64_t n = d * m_rate / kUsPerSecond;
    if (n <= 0) return;
    if (m_started) m_inserted += n;
    else m_leading_pad += n;
    m_started = true;
    m_resync_next = true;   // the next real packet lands exactly where it belongs
    EmitSilence(n, emit);
}

} // namespace ct::audio
