#include <ct/audio/resampler.h>

#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <vector>

using ct::audio::Resampler;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> Sine(int rate, double freq, double amp, size_t frames, int channels) {
    std::vector<float> v(frames * channels);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            v[i * channels + c] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return v;
}

// Streams `in` through the resampler in uneven chunks with a small output buffer (exercises partial consumption).
std::vector<float> RunStreaming(Resampler& r, const std::vector<float>& in, int channels) {
    std::vector<float> out;
    std::vector<float> buf(300 * channels);
    const size_t frames = in.size() / channels;
    size_t pos = 0;
    size_t chunk = 1;
    while (pos < frames) {
        const size_t n = std::min(chunk, frames - pos);
        size_t fed = 0;
        while (fed < n) {
            size_t consumed = 0;
            const size_t got = r.Process(in.data() + (pos + fed) * channels, n - fed, buf.data(), 300, &consumed);
            out.insert(out.end(), buf.begin(), buf.begin() + got * channels);
            fed += consumed;
        }
        pos += n;
        chunk = chunk * 7 % 997 + 1;   // 1, 8, 57, 400, ... uneven sizes
    }
    for (;;) {
        const size_t got = r.Flush(buf.data(), 300);
        out.insert(out.end(), buf.begin(), buf.begin() + got * channels);
        if (got < 300) break;
    }
    return out;
}

// Least-squares fit of a*sin + b*cos + c at a known frequency over channel 0 of out[first, last); returns SNR in dB
// (fitted sine power over residual power). *direct_snr compares with the ideal amp*sin(w*j) (no fit, zero delay).
double SnrDb(const std::vector<float>& out, int channels, int rate, double freq, double amp, size_t first,
             size_t last, double* direct_snr) {
    const double w = 2.0 * kPi * freq / rate;
    double ss = 0, sc = 0, cc = 0, s1 = 0, c1 = 0, n = 0, ys = 0, yc = 0, y1 = 0;
    for (size_t j = first; j < last; ++j) {
        const double s = std::sin(w * j), c = std::cos(w * j), y = out[j * channels];
        ss += s * s; sc += s * c; cc += c * c; s1 += s; c1 += c; n += 1; ys += y * s; yc += y * c; y1 += y;
    }
    // Solve the 3x3 normal equations by Cramer's rule.
    const double m[3][3] = {{ss, sc, s1}, {sc, cc, c1}, {s1, c1, n}};
    const double r[3] = {ys, yc, y1};
    auto det3 = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det3(m);
    double coef[3];
    for (int k = 0; k < 3; ++k) {
        double t[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t[i][j] = j == k ? r[i] : m[i][j];
        coef[k] = det3(t) / d;
    }
    double sig = 0, err = 0, derr = 0;
    for (size_t j = first; j < last; ++j) {
        const double fit = coef[0] * std::sin(w * j) + coef[1] * std::cos(w * j) + coef[2];
        const double y = out[j * channels];
        sig += fit * fit;
        err += (y - fit) * (y - fit);
        const double ideal = amp * std::sin(w * j);
        derr += (y - ideal) * (y - ideal);
    }
    const double ideal_power = amp * amp / 2.0 * static_cast<double>(last - first);
    *direct_snr = 10.0 * std::log10(ideal_power / derr);
    return 10.0 * std::log10(sig / err);
}

} // namespace

CT_TEST(resampler_snr_44k1_to_48k) {
    for (int channels : {1, 2}) {
        Resampler r;
        CT_REQUIRE(r.Init(44100, 48000, channels));
        CT_CHECK_NEAR(r.Ratio(), 48000.0 / 44100.0, 1e-12);
        CT_CHECK_EQ(r.Latency(), 16);
        const double amp = 0.5011872;   // -6 dBFS
        const auto in = Sine(44100, 1000.0, amp, 88200, channels);
        const auto out = RunStreaming(r, in, channels);
        const size_t frames = out.size() / channels;
        CT_CHECK_NEAR(static_cast<double>(frames), 96000.0, 2.0);
        double direct = 0;
        const double snr = SnrDb(out, channels, 48000, 1000.0, amp, 64, frames - 64, &direct);
        if (ct::test::Verbose()) std::printf("  %d ch: SNR %.1f dB (vs ideal sine without fit: %.1f dB)\n", channels, snr, direct);
        CT_CHECK(snr >= 90.0);
        CT_CHECK(direct >= 80.0);   // zero group delay: output frame j really is at input time j/48000
        if (channels == 2) {
            bool same = true;
            for (size_t j = 0; j < frames; ++j) same = same && out[2 * j] == out[2 * j + 1];
            CT_CHECK(same);
        }
    }
}

CT_TEST(resampler_snr_other_rates) {
    struct Case { int in, out; } cases[] = {{48000, 44100}, {96000, 48000}, {16000, 48000}, {192000, 48000}};
    for (const auto& c : cases) {
        Resampler r;
        CT_REQUIRE(r.Init(c.in, c.out, 1));
        const auto in = Sine(c.in, 1000.0, 0.5, static_cast<size_t>(c.in), 1);
        const auto out = RunStreaming(r, in, 1);
        double direct = 0;
        const double snr = SnrDb(out, 1, c.out, 1000.0, 0.5, 128, out.size() - 128, &direct);
        if (ct::test::Verbose()) std::printf("  %d -> %d: %zu frames, SNR %.1f dB\n", c.in, c.out, out.size(), snr);
        CT_CHECK_NEAR(static_cast<double>(out.size()), static_cast<double>(c.out), 2.0);
        CT_CHECK(snr >= 85.0);
    }
}

CT_TEST(resampler_rejects_aliases_when_downsampling) {
    // 30 kHz at 96 kHz has no place below the 24 kHz output Nyquist: it must be filtered, not folded to 18 kHz.
    Resampler r;
    CT_REQUIRE(r.Init(96000, 48000, 1));
    const auto in = Sine(96000, 30000.0, 0.5, 96000, 1);
    const auto out = RunStreaming(r, in, 1);
    double sum = 0;
    for (size_t j = 256; j < out.size() - 256; ++j) sum += static_cast<double>(out[j]) * out[j];
    const double rms = std::sqrt(sum / static_cast<double>(out.size() - 512));
    const double atten_db = 20.0 * std::log10(rms / (0.5 / std::sqrt(2.0)));
    if (ct::test::Verbose()) std::printf("  30 kHz alias attenuation: %.1f dB\n", atten_db);
    CT_CHECK(atten_db < -60.0);
}

CT_TEST(resampler_init_limits_and_reset) {
    Resampler r;
    CT_CHECK(!r.Init(44100, 48000, 0));
    CT_CHECK(!r.Init(8000, 96000, 2));   // ratio 12 > 8
    CT_CHECK(!r.Init(0, 48000, 2));
    CT_REQUIRE(r.Init(44100, 48000, 2));
    std::vector<float> in(441 * 2, 0.25f);
    std::vector<float> out(1000 * 2);
    size_t consumed = 0;
    const size_t a = r.Process(in.data(), 441, out.data(), 1000, &consumed);
    CT_CHECK_EQ(consumed, size_t(441));
    r.Reset();
    const size_t b = r.Process(in.data(), 441, out.data(), 1000, &consumed);
    CT_CHECK_EQ(a, b);   // same input after Reset gives the same output count
    // DC passes with unity gain once the history is filled.
    CT_CHECK_NEAR(out[(b - 1) * 2], 0.25, 1e-4);
}

CT_BENCH(resampler_44k1_to_48k_stereo_10ms, 20'000) {
    static Resampler r;
    static bool init = r.Init(44100, 48000, 2);
    (void)init;
    static std::vector<float> in(441 * 2, 0.25f), out(600 * 2);
    size_t consumed = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) r.Process(in.data(), 441, out.data(), 600, &consumed);
}