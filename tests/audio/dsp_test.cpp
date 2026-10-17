#include <ct/audio/dsp.h>

#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dsp = ct::audio::dsp;

namespace {

struct Lcg {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    float Next(float lo, float hi) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        const float u = static_cast<float>((s >> 40) & 0xFFFFFF) / 16777216.0f;
        return lo + (hi - lo) * u;
    }
};

std::vector<float> Random(size_t n, float lo, float hi, uint64_t seed) {
    Lcg g;
    g.s ^= seed;
    std::vector<float> v(n);
    for (auto& x : v) x = g.Next(lo, hi);
    return v;
}

} // namespace

CT_TEST(dsp_float_to_pcm16_values) {
    dsp::Init();
    const float in[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 1.0f / 65536.0f, 3.0f / 65536.0f, 0.99999f};
    int16_t out[10];
    dsp::FloatToPcm16(in, out, 10);
    CT_CHECK_EQ(out[0], int16_t(0));
    CT_CHECK_EQ(out[1], int16_t(32767));    // saturates
    CT_CHECK_EQ(out[2], int16_t(-32768));
    CT_CHECK_EQ(out[3], int16_t(16384));
    CT_CHECK_EQ(out[4], int16_t(-16384));
    CT_CHECK_EQ(out[5], int16_t(32767));    // clamped
    CT_CHECK_EQ(out[6], int16_t(-32768));
    CT_CHECK_EQ(out[7], int16_t(0));        // 0.5 LSB rounds to even
    CT_CHECK_EQ(out[8], int16_t(2));        // 1.5 LSB rounds to even
    CT_CHECK_EQ(out[9], int16_t(32767));
    // Every int16 survives Pcm16ToFloat -> FloatToPcm16.
    std::vector<int16_t> all(65536), back(65536);
    for (int i = 0; i < 65536; ++i) all[i] = static_cast<int16_t>(i - 32768);
    std::vector<float> f(65536);
    dsp::Pcm16ToFloat(all.data(), f.data(), all.size());
    dsp::FloatToPcm16(f.data(), back.data(), f.size());
    CT_CHECK(std::memcmp(all.data(), back.data(), all.size() * 2) == 0);
}

CT_TEST(dsp_avx2_matches_scalar) {
    dsp::Init();
    const dsp::Kernels* avx = dsp::Avx2Kernels();
    if (!avx) CT_SKIP("CPU without AVX2+FMA");
    CT_CHECK(dsp::UsingAvx2());
    const dsp::Kernels& sc = dsp::ScalarKernels();
    const size_t n = 1'000'003;   // odd: exercises the scalar tails
    const auto a = Random(n, -1.0f, 1.0f, 1);
    const auto b = Random(n, -1.0f, 1.0f, 2);
    {
        auto d1 = a, d2 = a;
        sc.mix_add(d1.data(), b.data(), n, 0.37f);
        avx->mix_add(d2.data(), b.data(), n, 0.37f);
        CT_CHECK(std::memcmp(d1.data(), d2.data(), n * sizeof(float)) == 0);
    }
    {
        const auto wide = Random(n, -1.5f, 1.5f, 3);   // includes out-of-range values
        std::vector<int16_t> p1(n), p2(n);
        sc.float_to_pcm16(wide.data(), p1.data(), n);
        avx->float_to_pcm16(wide.data(), p2.data(), n);
        CT_CHECK(std::memcmp(p1.data(), p2.data(), n * sizeof(int16_t)) == 0);
        std::vector<float> f1(n), f2(n);
        sc.pcm16_to_float(p1.data(), f1.data(), n);
        avx->pcm16_to_float(p1.data(), f2.data(), n);
        CT_CHECK(std::memcmp(f1.data(), f2.data(), n * sizeof(float)) == 0);
    }
    {
        std::vector<float> s1(2 * n), s2(2 * n);
        sc.mono_to_stereo(a.data(), s1.data(), n);
        avx->mono_to_stereo(a.data(), s2.data(), n);
        CT_CHECK(std::memcmp(s1.data(), s2.data(), 2 * n * sizeof(float)) == 0);
        const size_t frames = n / 6;
        std::vector<float> m1(2 * frames), m2(2 * frames);
        sc.downmix_to_stereo(a.data(), 6, m1.data(), frames);
        avx->downmix_to_stereo(a.data(), 6, m2.data(), frames);
        CT_CHECK(std::memcmp(m1.data(), m2.data(), 2 * frames * sizeof(float)) == 0);
        sc.downmix_to_stereo(a.data(), 2, m1.data(), frames);
        avx->downmix_to_stereo(a.data(), 2, m2.data(), frames);
        CT_CHECK(std::memcmp(m1.data(), m2.data(), 2 * frames * sizeof(float)) == 0);
    }
    CT_CHECK_EQ(sc.peak_abs(a.data(), n), avx->peak_abs(a.data(), n));
    CT_CHECK_NEAR(sc.rms(a.data(), n), avx->rms(a.data(), n), 1e-6);
    CT_CHECK_NEAR(sc.rms(a.data(), n), 1.0 / std::sqrt(3.0), 1e-3);   // uniform [-1, 1]
    {
        const size_t frames = n / 2;
        auto g1 = a, g2 = a;
        sc.apply_gain_ramp(g1.data(), frames, 2, 1.0f, 0.0f);
        avx->apply_gain_ramp(g2.data(), frames, 2, 1.0f, 0.0f);
        double max_diff = 0;
        for (size_t i = 0; i < 2 * frames; ++i) max_diff = std::fmax(max_diff, std::fabs(g1[i] - g2[i]));
        CT_CHECK(max_diff < 1e-6);
        CT_CHECK_EQ(g1[0], a[0]);   // frame 0 gets g0
    }
}

CT_TEST(dsp_mix_ramp_gate_behaviour) {
    dsp::Init();
    std::vector<float> dst(10, 1.0f), src(10, 0.5f);
    dsp::MixAdd(dst.data(), src.data(), 10, 2.0f);
    CT_CHECK_EQ(dst[9], 2.0f);
    std::vector<float> ramp(8, 1.0f);   // 4 stereo frames, 0 -> 1
    dsp::ApplyGainRamp(ramp.data(), 4, 2, 0.0f, 1.0f);
    CT_CHECK_EQ(ramp[0], 0.0f);
    CT_CHECK_EQ(ramp[3], 0.25f);
    CT_CHECK_EQ(ramp[7], 0.75f);
    CT_CHECK_NEAR(dsp::PeakAbs(src.data(), 10), 0.5, 0);
    CT_CHECK_NEAR(dsp::Rms(src.data(), 10), 0.5, 1e-7);
    // Gate: a -60 dBFS hiss is muted, a -10 dBFS signal passes.
    const int frames = 48000;
    std::vector<float> quiet(frames * 2, 0.001f), loud(frames * 2, 0.3f);
    ct::audio::GateState st;
    dsp::NoiseGate(quiet.data(), frames, 2, -40.0f, 1.0f, 50.0f, 48000.0f, st);
    CT_CHECK(std::fabs(quiet[2 * (frames - 1)]) < 1e-6f);
    dsp::NoiseGate(loud.data(), frames, 2, -40.0f, 1.0f, 50.0f, 48000.0f, st);
    CT_CHECK_NEAR(loud[2 * (frames - 1)], 0.3, 1e-4);
    CT_CHECK(loud[0] < 0.3f);   // opening is smoothed (attack), not a click
}

CT_BENCH(dsp_mix_add_480_stereo, 200'000) {
    dsp::Init();
    static std::vector<float> a(960, 0.1f), b(960, 0.2f);
    for (int64_t i = 0; i < CT_BENCH_N; ++i) dsp::MixAdd(a.data(), b.data(), 960, 0.5f);
}
