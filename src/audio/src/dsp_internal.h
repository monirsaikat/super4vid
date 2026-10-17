#pragma once
// Private: the scalar kernels (shared with dsp_avx2.cpp for tails and inherently serial kernels) and the AVX2 table.

#include <ct/audio/dsp.h>

namespace ct::audio::dsp {

namespace scalar {
void MixAdd(float* dst, const float* src, size_t n, float gain);
void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1);
void FloatToPcm16(const float* in, int16_t* out, size_t n);
void Pcm16ToFloat(const int16_t* in, float* out, size_t n);
void MonoToStereo(const float* in, float* out, size_t frames);
void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames);
float PeakAbs(const float* in, size_t n);
float Rms(const float* in, size_t n);
void NoiseGate(float* buf, size_t frames, int channels, float threshold_db, float attack_ms, float release_ms,
               float rate, GateState& state);
} // namespace scalar

namespace detail {
// Defined in dsp_avx2.cpp (compiled with /arch:AVX2). Only dereferenced after the runtime CPU check.
const Kernels& Avx2KernelTable();
} // namespace detail

} // namespace ct::audio::dsp
