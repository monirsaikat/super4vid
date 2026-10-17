#pragma once
// ct/audio/dsp.h
// Small audio kernels: mixing, gain ramps, sample format conversion, channel conversion, peak/RMS, noise gate.
// Every kernel has a scalar and an AVX2 path; dsp::Init() picks one set once (AVX2 only when the CPU reports
// AVX2 + FMA and the OS saves YMM state). The AVX2 code lives in dsp_avx2.cpp, the only file built with /arch:AVX2.
// Thread model: Init() once at startup (it is idempotent and thread-safe); afterwards all functions are pure and
// callable from any thread. Calling a kernel before Init() runs the scalar path.

#include <cstddef>
#include <cstdint>

namespace ct::audio {

struct GateState {
    float envelope = 0.0f;   // peak follower, linear
    float gain = 1.0f;       // current gate gain 0..1
};

namespace dsp {

void Init();
bool UsingAvx2();   // true after Init() selected the AVX2 kernels

void MixAdd(float* dst, const float* src, size_t n, float gain);   // dst[i] += src[i] * gain (mul then add)
// Linear ramp over the frames: frame i gets g0 + (g1 - g0) * i / frames.
void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1);
// clamp [-1,1], scale by 32768, round to nearest even, saturate to [-32768, 32767]. TPDF dither optional later.
void FloatToPcm16(const float* in, int16_t* out, size_t n);
void Pcm16ToFloat(const int16_t* in, float* out, size_t n);   // x / 32768 (exact inverse of FloatToPcm16)
void MonoToStereo(const float* in, float* out, size_t frames);
void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames);   // L/R only, others ignored
float PeakAbs(const float* in, size_t n);
float Rms(const float* in, size_t n);
// Downward gate on the per-frame peak across channels. attack = opening time, release = closing time.
void NoiseGate(float* buf, size_t frames, int channels, float threshold_db, float attack_ms, float release_ms,
               float rate, GateState& state);

// The two kernel sets, for tests and benchmarks that compare them directly.
struct Kernels {
    void (*mix_add)(float*, const float*, size_t, float);
    void (*apply_gain_ramp)(float*, size_t, int, float, float);
    void (*float_to_pcm16)(const float*, int16_t*, size_t);
    void (*pcm16_to_float)(const int16_t*, float*, size_t);
    void (*mono_to_stereo)(const float*, float*, size_t);
    void (*downmix_to_stereo)(const float*, int, float*, size_t);
    float (*peak_abs)(const float*, size_t);
    float (*rms)(const float*, size_t);
    void (*noise_gate)(float*, size_t, int, float, float, float, float, GateState&);
};
const Kernels& ScalarKernels();
const Kernels* Avx2Kernels();   // nullptr when the CPU/OS cannot run AVX2 + FMA

} // namespace dsp
} // namespace ct::audio
