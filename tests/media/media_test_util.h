#pragma once
// Helpers shared by the media device tests: NV12 pools filled with flat colours, a test listener, and a decode check
// (software H.264/HEVC decode through the source reader to NV12 in system memory).

#include <ct/platform/d3d_device.h>
#include <ct/platform/texture_pool.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ct::media_test {

// NV12 texture pool with RT|SR (+UAV when `uav` and the device supports it). Returns false on failure.
bool MakeNv12Pool(platform::D3DDevice& dev, platform::TexturePool& pool, int w, int h, uint32_t count, bool uav, bool* got_uav);
// Clears the texture's Y plane to `y` and its UV plane to (u, v).
bool FillNv12(platform::D3DDevice& dev, ID3D11Texture2D* tex, uint8_t y, uint8_t u = 128, uint8_t v = 128);
// Mean luma of the first `max_frames` decoded frames (in presentation order) of the file's first video stream.
// hw: decode with the hardware decoder through this DXGI device manager (frames are copied back for the check).
bool DecodeLumaMeans(const std::string& path, int max_frames, std::vector<double>& means, IMFDXGIDeviceManager* hw = nullptr);

// Decodes up to max_frames video frames (software decoder) and calls fn(frame, y_plane, pitch, width, height) for each;
// fn returns false to stop.
using LumaFn = bool (*)(void* user, int frame, const uint8_t* y, int pitch, int width, int height);
bool DecodeLuma(const std::string& path, int max_frames, LumaFn fn, void* user);

// Decodes the n-th audio track (file order: 0 = first audio track) to interleaved float at the file's rate; returns the
// first sample's time in first_us.
bool DecodeAudio(const std::string& path, int audio_track, std::vector<float>& pcm, int& channels, int64_t& first_us);

std::string TempPath(const char* name);   // <test exe dir>\media_test_tmp\<name>, folder created
uint64_t FileSize(const std::string& path);
// Interleaved stereo sine at `hz`, amplitude `amp`, starting at frame `start_frame` (phase continuous).
void Sine(float* out, uint32_t frames, double hz, float amp, int64_t start_frame);

} // namespace ct::media_test
