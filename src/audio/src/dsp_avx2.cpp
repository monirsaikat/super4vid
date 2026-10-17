// AVX2 kernels. This is the only translation unit built with /arch:AVX2 (see src/audio/CMakeLists.txt); nothing in
// here runs unless dsp::Avx2Kernels() verified AVX2 + FMA + OS YMM support. Results that tests compare bit for bit
// (MixAdd, FloatToPcm16, Pcm16ToFloat, conversions, PeakAbs) use the same operation order as the scalar code:
// separate multiply and add (no FMA contraction), identical min/max operand order.
#include "dsp_internal.h"

#include <immintrin.h>

#include <cstring>

namespace ct::audio::dsp {

namespace {

void MixAdd(float* dst, const float* src, size_t n, float gain) {
    const __m256 g = _mm256_set1_ps(gain);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m256 a0 = _mm256_add_ps(_mm256_loadu_ps(dst + i), _mm256_mul_ps(_mm256_loadu_ps(src + i), g));
        const __m256 a1 = _mm256_add_ps(_mm256_loadu_ps(dst + i + 8), _mm256_mul_ps(_mm256_loadu_ps(src + i + 8), g));
        _mm256_storeu_ps(dst + i, a0);
        _mm256_storeu_ps(dst + i + 8, a1);
    }
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(dst + i), _mm256_mul_ps(_mm256_loadu_ps(src + i), g)));
    scalar::MixAdd(dst + i, src + i, n - i, gain);
}

void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1) {
    if (channels != 2 || frames == 0) {
        scalar::ApplyGainRamp(buf, frames, channels, g0, g1);
        return;
    }
    const float step = (g1 - g0) / static_cast<float>(frames);
    const __m256 vstep = _mm256_set1_ps(step);
    const __m256 vg0 = _mm256_set1_ps(g0);
    __m256 idx = _mm256_setr_ps(0, 0, 1, 1, 2, 2, 3, 3);
    const __m256 four = _mm256_set1_ps(4.0f);
    size_t f = 0;
    for (; f + 4 <= frames; f += 4) {
        const __m256 g = _mm256_add_ps(vg0, _mm256_mul_ps(vstep, idx));
        _mm256_storeu_ps(buf + 2 * f, _mm256_mul_ps(_mm256_loadu_ps(buf + 2 * f), g));
        idx = _mm256_add_ps(idx, four);   // exact for frame indices < 2^24
    }
    for (; f < frames; ++f) {
        const float g = g0 + step * static_cast<float>(f);
        buf[2 * f] *= g;
        buf[2 * f + 1] *= g;
    }
}

void FloatToPcm16(const float* in, int16_t* out, size_t n) {
    const __m256 hi = _mm256_set1_ps(1.0f);
    const __m256 lo = _mm256_set1_ps(-1.0f);
    const __m256 scale = _mm256_set1_ps(32768.0f);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256 a = _mm256_loadu_ps(in + i);
        __m256 b = _mm256_loadu_ps(in + i + 8);
        a = _mm256_max_ps(_mm256_min_ps(a, hi), lo);
        b = _mm256_max_ps(_mm256_min_ps(b, hi), lo);
        const __m256i ia = _mm256_cvtps_epi32(_mm256_mul_ps(a, scale));
        const __m256i ib = _mm256_cvtps_epi32(_mm256_mul_ps(b, scale));
        // packs works per 128-bit lane: [a0..3 b0..3 | a4..7 b4..7] -> reorder the 64-bit quarters.
        const __m256i packed = _mm256_permute4x64_epi64(_mm256_packs_epi32(ia, ib), _MM_SHUFFLE(3, 1, 2, 0));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), packed);
    }
    scalar::FloatToPcm16(in + i, out + i, n - i);
}

void Pcm16ToFloat(const int16_t* in, float* out, size_t n) {
    const __m256 scale = _mm256_set1_ps(1.0f / 32768.0f);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(s)), scale));
    }
    scalar::Pcm16ToFloat(in + i, out + i, n - i);
}

void MonoToStereo(const float* in, float* out, size_t frames) {
    size_t i = 0;
    for (; i + 8 <= frames; i += 8) {
        const __m256 x = _mm256_loadu_ps(in + i);
        const __m256 l = _mm256_unpacklo_ps(x, x);   // x0 x0 x1 x1 | x4 x4 x5 x5
        const __m256 h = _mm256_unpackhi_ps(x, x);   // x2 x2 x3 x3 | x6 x6 x7 x7
        _mm256_storeu_ps(out + 2 * i, _mm256_permute2f128_ps(l, h, 0x20));
        _mm256_storeu_ps(out + 2 * i + 8, _mm256_permute2f128_ps(l, h, 0x31));
    }
    scalar::MonoToStereo(in + i, out + 2 * i, frames - i);
}

void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames) {
    if (in_channels == 2) {
        std::memcpy(out, in, frames * 2 * sizeof(float));
        return;
    }
    if (in_channels == 1) {
        MonoToStereo(in, out, frames);
        return;
    }
    // Strided L/R gather: memory bound, the compiler's scalar loop is as fast as a gather here.
    scalar::DownmixToStereo(in, in_channels, out, frames);
}

float PeakAbs(const float* in, size_t n) {
    const __m256 mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 m = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) m = _mm256_max_ps(m, _mm256_and_ps(_mm256_loadu_ps(in + i), mask));
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, m);
    float peak = scalar::PeakAbs(in + i, n - i);
    for (float v : lanes) peak = v > peak ? v : peak;
    return peak;
}

float Rms(const float* in, size_t n) {
    if (n == 0) return 0.0f;
    __m256d s0 = _mm256_setzero_pd();
    __m256d s1 = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256d a = _mm256_cvtps_pd(_mm_loadu_ps(in + i));
        const __m256d b = _mm256_cvtps_pd(_mm_loadu_ps(in + i + 4));
        s0 = _mm256_add_pd(s0, _mm256_mul_pd(a, a));
        s1 = _mm256_add_pd(s1, _mm256_mul_pd(b, b));
    }
    alignas(32) double lanes[4];
    _mm256_store_pd(lanes, _mm256_add_pd(s0, s1));
    double sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; ++i) sum += static_cast<double>(in[i]) * in[i];
    return static_cast<float>(_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(sum / static_cast<double>(n)))));
}

// The gate's envelope and gain are a per-frame recursion; there is nothing to vectorize across frames.
const Kernels kAvx2 = {
    &MixAdd,       &ApplyGainRamp, &FloatToPcm16,    &Pcm16ToFloat, &MonoToStereo,
    &DownmixToStereo, &PeakAbs,    &Rms,             &scalar::NoiseGate,
};

} // namespace

namespace detail {
const Kernels& Avx2KernelTable() { return kAvx2; }
} // namespace detail

} // namespace ct::audio::dsp
