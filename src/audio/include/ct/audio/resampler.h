#pragma once
// ct/audio/resampler.h
// Streaming sample-rate converter: polyphase windowed sinc (Kaiser beta 8, 32 taps, 256 phases, linear
// interpolation between phases), cutoff 0.45 * min(in, out) / in, interleaved multi-channel, rational step (no
// cumulative drift). Output frame j corresponds exactly to input position j * in / out (zero group delay): the
// filter is centred, so it needs Latency() input frames of look-ahead before an output can be produced.
// Thread model: one thread. Lifetime: Init allocates; Process/Flush never allocate.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ct::audio {

class Resampler {
public:
    static constexpr int kTaps = 32;
    static constexpr int kPhases = 256;

    // Rates 1 kHz .. 768 kHz, ratio within [1/8, 8], 1..8 channels.
    bool Init(int in_rate, int out_rate, int channels);
    void Reset();   // forget the stream (history = silence), keep the filter
    // Consumes up to in_frames (*in_consumed tells how many) and writes up to out_capacity_frames. Call again with
    // the rest when *in_consumed < in_frames (the output was full).
    size_t Process(const float* in, size_t in_frames, float* out, size_t out_capacity_frames, size_t* in_consumed);
    // End of stream: pads Latency() frames of silence and returns the remaining outputs. Call until it returns less
    // than out_capacity_frames; the resampler is then Reset() for a new stream.
    size_t Flush(float* out, size_t out_capacity_frames);
    double Ratio() const;   // output frames per input frame (out_rate / in_rate)
    int Latency() const { return kTaps / 2; }   // input frames of look-ahead
    int InRate() const { return m_in_rate; }
    int OutRate() const { return m_out_rate; }

private:
    size_t Produce(float* out, size_t out_capacity_frames);
    void Compact();

    std::vector<float> m_table;   // (kPhases + 1) x kTaps
    std::vector<float> m_buf;     // interleaved input history + look-ahead
    std::vector<float> m_coef;    // kTaps, interpolated per output
    size_t m_buf_capacity = 0;    // frames
    size_t m_buf_frames = 0;      // valid frames in m_buf
    int64_t m_n = 0;              // buffer index of the integer input position
    uint64_t m_frac = 0;          // fractional position, numerator over m_den
    uint64_t m_den = 1;           // out_rate / gcd
    uint64_t m_step_int = 0;      // floor(in / out)
    uint64_t m_step_rem = 0;      // (in / gcd) % m_den
    int64_t m_skip = 0;           // input frames to discard (positions beyond the buffer)
    int m_flush_fed = 0;          // silence frames already fed by Flush
    int m_in_rate = 0;
    int m_out_rate = 0;
    int m_channels = 0;
};

} // namespace ct::audio
