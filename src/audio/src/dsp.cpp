#include <ct/audio/dsp.h>

#include "dsp_internal.h"

#include <emmintrin.h>
#include <intrin.h>

#include <atomic>
#include <cmath>

namespace ct::audio::dsp {

namespace scalar {

void MixAdd(float* dst, const float* src, size_t n, float gain) {
    for (size_t i = 0; i < n; ++i) dst[i] = dst[i] + src[i] * gain;
}

void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1) {
    if (frames == 0) return;
    const float step = (g1 - g0) / static_cast<float>(frames);
    for (size_t f = 0; f < frames; ++f) {
        const float g = g0 + step * static_cast<float>(f);
        float* p = buf + f * static_cast<size_t>(channels);
        for (int c = 0; c < channels; ++c) p[c] *= g;
    }
}

void FloatToPcm16(const float* in, int16_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        // Same operation order and NaN behaviour as the AVX2 path (_mm256_min_ps/_mm256_max_ps, cvtps, packs).
        float v = in[i];
        v = v < 1.0f ? v : 1.0f;
        v = v > -1.0f ? v : -1.0f;
        int32_t s = _mm_cvtss_si32(_mm_set_ss(v * 32768.0f));   // round to nearest even (MXCSR default)
        s = s > 32767 ? 32767 : s;
        out[i] = static_cast<int16_t>(s);
    }
}

void Pcm16ToFloat(const int16_t* in, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<float>(in[i]) * (1.0f / 32768.0f);
}

void MonoToStereo(const float* in, float* out, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        out[2 * i] = in[i];
        out[2 * i + 1] = in[i];
    }
}

void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames) {
    if (in_channels == 1) {
        MonoToStereo(in, out, frames);
        return;
    }
    const size_t stride = static_cast<size_t>(in_channels);
    for (size_t i = 0; i < frames; ++i) {
        out[2 * i] = in[i * stride];
        out[2 * i + 1] = in[i * stride + 1];
    }
}

float PeakAbs(const float* in, size_t n) {
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(in[i]);
        peak = a > peak ? a : peak;
    }
    return peak;
}

float Rms(const float* in, size_t n) {
    if (n == 0) return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += static_cast<double>(in[i]) * in[i];
    return static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}

void NoiseGate(float* buf, size_t frames, int channels, float threshold_db, float attack_ms, float release_ms,
               float rate, GateState& state) {
    const float threshold = std::pow(10.0f, threshold_db / 20.0f);
    const float attack = attack_ms > 0 ? std::exp(-1.0f / (attack_ms * 0.001f * rate)) : 0.0f;
    const float release = release_ms > 0 ? std::exp(-1.0f / (release_ms * 0.001f * rate)) : 0.0f;
    float env = state.envelope;
    float gain = state.gain;
    for (size_t f = 0; f < frames; ++f) {
        float* p = buf + f * static_cast<size_t>(channels);
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const float a = std::fabs(p[c]);
            peak = a > peak ? a : peak;
        }
        env = peak > env ? peak : env * release;
        const float target = env >= threshold ? 1.0f : 0.0f;
        const float coef = target > gain ? attack : release;
        gain = target + (gain - target) * coef;
        for (int c = 0; c < channels; ++c) p[c] *= gain;
    }
    state.envelope = env;
    state.gain = gain;
}

} // namespace scalar

namespace {

const Kernels kScalar = {
    &scalar::MixAdd,       &scalar::ApplyGainRamp,   &scalar::FloatToPcm16,
    &scalar::Pcm16ToFloat, &scalar::MonoToStereo,    &scalar::DownmixToStereo,
    &scalar::PeakAbs,      &scalar::Rms,             &scalar::NoiseGate,
};

std::atomic<const Kernels*> g_active{&kScalar};

bool CpuHasAvx2Fma() {
    int r[4] = {};
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuidex(r, 1, 0);
    const bool fma = (r[2] & (1 << 12)) != 0;
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    const bool avx = (r[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx) return false;
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;   // OS saves XMM and YMM state
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;           // AVX2
}

const Kernels& Active() { return *g_active.load(std::memory_order_acquire); }

} // namespace

const Kernels& ScalarKernels() { return kScalar; }

const Kernels* Avx2Kernels() {
    static const bool supported = CpuHasAvx2Fma();
    return supported ? &detail::Avx2KernelTable() : nullptr;
}

void Init() {
    const Kernels* avx2 = Avx2Kernels();
    g_active.store(avx2 ? avx2 : &kScalar, std::memory_order_release);
}

bool UsingAvx2() { return g_active.load(std::memory_order_acquire) != &kScalar; }

void MixAdd(float* dst, const float* src, size_t n, float gain) { Active().mix_add(dst, src, n, gain); }
void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1) {
    Active().apply_gain_ramp(buf, frames, channels, g0, g1);
}
void FloatToPcm16(const float* in, int16_t* out, size_t n) { Active().float_to_pcm16(in, out, n); }
void Pcm16ToFloat(const int16_t* in, float* out, size_t n) { Active().pcm16_to_float(in, out, n); }
void MonoToStereo(const float* in, float* out, size_t frames) { Active().mono_to_stereo(in, out, frames); }
void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames) {
    Active().downmix_to_stereo(in, in_channels, out, frames);
}
float PeakAbs(const float* in, size_t n) { return Active().peak_abs(in, n); }
float Rms(const float* in, size_t n) { return Active().rms(in, n); }
void NoiseGate(float* buf, size_t frames, int channels, float threshold_db, float attack_ms, float release_ms,
               float rate, GateState& state) {
    Active().noise_gate(buf, frames, channels, threshold_db, attack_ms, release_ms, rate, state);
}

} // namespace ct::audio::dsp
