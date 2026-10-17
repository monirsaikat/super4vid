#pragma once
// Private helpers shared by the WASAPI classes: endpoint lookup, friendly names, wave formats, QPC conversion.
// All functions need COM initialized on the calling thread (MTA on the audio threads).

#include <ct/platform/win.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace ct::audio::detail {

// Creates a fresh IMMDeviceEnumerator in the calling thread's apartment and opens `id` (empty = default eConsole
// endpoint of the given flow). Returns E_NOTFOUND-style HRESULTs when there is no such endpoint.
HRESULT OpenEndpoint(const std::wstring& id, bool capture, Microsoft::WRL::ComPtr<IMMDevice>& out);
std::string FriendlyName(IMMDevice* device);
std::wstring EndpointId(IMMDevice* device);

// Sample layout of a WAVEFORMATEX(TENSIBLE): what our converters understand.
enum class SampleType { Float32, Int16, Int24, Int32, Unsupported };
SampleType SampleTypeOf(const WAVEFORMATEX* wf);

// 32-bit float interleaved, `rate` Hz, `channels` channels, with the given channel mask (0 = derive a default).
WAVEFORMATEXTENSIBLE MakeFloatFormat(uint32_t rate, int channels, DWORD channel_mask);

struct CoTaskMemDeleter { void operator()(void* p) const { CoTaskMemFree(p); } };

// floor(qpc * 1e6 / freq) as an absolute microsecond value (128-bit intermediate).
int64_t QpcToAbsUs(int64_t qpc);

} // namespace ct::audio::detail
