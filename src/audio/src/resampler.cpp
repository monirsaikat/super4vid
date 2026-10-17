#include <ct/audio/resampler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace ct::audio {

namespace {

constexpr int kHalf = Resampler::kTaps / 2;      // 16 look-ahead frames
constexpr int kHistory = kHalf - 1;              // 15 frames before the integer position
constexpr double kBeta = 8.0;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kChunkFrames = 1024;            // input appended per refill

double BesselI0(double x) {
    // Power series; converges quickly for the arguments a Kaiser window uses (x <= beta).
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

} // namespace

bool Resampler::Init(int in_rate, int out_rate, int channels) {
    if (in_rate < 1000 || out_rate < 1000 || in_rate > 768000 || out_rate > 768000) return false;
    if (channels < 1 || channels > 8) return false;
    if (in_rate > out_rate * 8 || out_rate > in_rate * 8) return false;
    m_in_rate = in_rate;
    m_out_rate = out_rate;
    m_channels = channels;
    const uint64_t g = std::gcd(static_cast<uint64_t>(in_rate), static_cast<uint64_t>(out_rate));
    const uint64_t in_g = static_cast<uint64_t>(in_rate) / g;
    m_den = static_cast<uint64_t>(out_rate) / g;
    m_step_int = in_g / m_den;
    m_step_rem = in_g % m_den;

    // Windowed sinc, cutoff relative to the input rate (cycles per input sample).
    const double fc = 0.45 * static_cast<double>(std::min(in_rate, out_rate)) / in_rate;
    const double i0_beta = BesselI0(kBeta);
    m_table.assign(static_cast<size_t>(kPhases + 1) * kTaps, 0.0f);
    for (int ph = 0; ph <= kPhases; ++ph) {
        double row[kTaps];
        double sum = 0.0;
        for (int i = 0; i < kTaps; ++i) {
            // Tap i multiplies input sample n - 15 + i; its distance from the output position n + f is f + 15 - i.
            const double x = static_cast<double>(ph) / kPhases + kHistory - i;
            const double t = x / kHalf;
            double h = 0.0;
            if (t > -1.0 && t < 1.0) {
                const double arg = 2.0 * fc * x;
                const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
                h = 2.0 * fc * sinc * BesselI0(kBeta * std::sqrt(1.0 - t * t)) / i0_beta;
            }
            row[i] = h;
            sum += h;
        }
        for (int i = 0; i < kTaps; ++i)   // unity DC gain for every phase
            m_table[static_cast<size_t>(ph) * kTaps + i] = static_cast<float>(row[i] / sum);
    }
    m_buf_capacity = kChunkFrames + kTaps + 8;
    m_buf.assign(m_buf_capacity * static_cast<size_t>(channels), 0.0f);
    m_coef.assign(kTaps, 0.0f);
    Reset();
    return true;
}

void Resampler::Reset() {
    std::fill(m_buf.begin(), m_buf.end(), 0.0f);
    m_buf_frames = kHistory;   // silence before the first input frame
    m_n = kHistory;            // output 0 sits on input frame 0
    m_frac = 0;
    m_skip = 0;
    m_flush_fed = 0;
}

double Resampler::Ratio() const {
    return m_in_rate > 0 ? static_cast<double>(m_out_rate) / static_cast<double>(m_in_rate) : 0.0;
}

size_t Resampler::Produce(float* out, size_t cap) {
    size_t produced = 0;
    const int ch = m_channels;
    const double phase_scale = static_cast<double>(kPhases) / static_cast<double>(m_den);
    while (produced < cap && m_n + kHalf < static_cast<int64_t>(m_buf_frames)) {
        const double pf = static_cast<double>(m_frac) * phase_scale;
        int ph = static_cast<int>(pf);
        if (ph >= kPhases) ph = kPhases - 1;
        const float w = static_cast<float>(pf - ph);
        const float* a = m_table.data() + static_cast<size_t>(ph) * kTaps;
        const float* b = a + kTaps;
        for (int i = 0; i < kTaps; ++i) m_coef[i] = a[i] + (b[i] - a[i]) * w;
        const float* x = m_buf.data() + static_cast<size_t>(m_n - kHistory) * ch;
        float* y = out + produced * ch;
        for (int c = 0; c < ch; ++c) {
            float acc = 0.0f;
            for (int i = 0; i < kTaps; ++i) acc += m_coef[i] * x[static_cast<size_t>(i) * ch + c];
            y[c] = acc;
        }
        ++produced;
        m_n += static_cast<int64_t>(m_step_int);
        m_frac += m_step_rem;
        if (m_frac >= m_den) {
            m_frac -= m_den;
            ++m_n;
        }
    }
    return produced;
}

void Resampler::Compact() {
    const int64_t keep_from = m_n - kHistory;
    if (keep_from <= 0) return;
    const int64_t frames = static_cast<int64_t>(m_buf_frames);
    if (keep_from >= frames) {
        m_skip += keep_from - frames;
        m_buf_frames = 0;
    } else {
        const size_t remain = static_cast<size_t>(frames - keep_from);
        std::memmove(m_buf.data(), m_buf.data() + static_cast<size_t>(keep_from) * m_channels,
                     remain * m_channels * sizeof(float));
        m_buf_frames = remain;
    }
    m_n -= keep_from;
}

size_t Resampler::Process(const float* in, size_t in_frames, float* out, size_t out_capacity_frames,
                          size_t* in_consumed) {
    size_t consumed = 0;
    size_t produced = 0;
    if (m_channels == 0) {
        if (in_consumed) *in_consumed = 0;
        return 0;
    }
    for (;;) {
        produced += Produce(out + produced * m_channels, out_capacity_frames - produced);
        if (produced == out_capacity_frames || consumed == in_frames) break;
        Compact();
        if (m_skip > 0) {
            const size_t s = std::min(static_cast<size_t>(m_skip), in_frames - consumed);
            consumed += s;
            m_skip -= static_cast<int64_t>(s);
            if (consumed == in_frames) break;
        }
        const size_t space = m_buf_capacity - m_buf_frames;
        const size_t k = std::min(space, in_frames - consumed);
        std::memcpy(m_buf.data() + m_buf_frames * m_channels, in + consumed * m_channels, k * m_channels * sizeof(float));
        m_buf_frames += k;
        consumed += k;
    }
    if (in_consumed) *in_consumed = consumed;
    return produced;
}

size_t Resampler::Flush(float* out, size_t out_capacity_frames) {
    float zeros[kHalf * 8] = {};
    size_t produced = 0;
    while (produced < out_capacity_frames) {
        if (m_flush_fed < kHalf) {
            // Feed the look-ahead as silence so every pending output position can be computed.
            size_t consumed = 0;
            produced += Process(zeros, static_cast<size_t>(kHalf - m_flush_fed), out + produced * m_channels,
                                out_capacity_frames - produced, &consumed);
            m_flush_fed += static_cast<int>(consumed);
            continue;
        }
        const size_t got = Produce(out + produced * m_channels, out_capacity_frames - produced);
        produced += got;
        if (got == 0) {
            Reset();
            break;
        }
    }
    return produced;
}
} // namespace ct::audio
