# P03 · Audio capture (WASAPI mic + system loopback, resampler, meters, render)

**Goal:** sample-accurate, gapless 48 kHz stereo float streams for the microphone and for system audio, timestamped
on the session clock, surviving device changes; plus a WASAPI render client (needed for the loopback keep-alive now
and for the editor's audio engine in P06).

**Read first:** `docs/03-architecture.md` sections 3, 4, 10; `docs/02` section 6; `docs/07` (Core Audio rows).
**Depends on:** P01 (independent of P02). **Size:** M (about 2,500 lines incl. tests and bench).

## Deliverables

```
src/audio/include/ct/audio/{devices.h,wasapi_capture.h,wasapi_loopback.h,wasapi_render.h,stream_aligner.h,resampler.h,dsp.h,meter.h,wav.h,audio_sink.h}
src/audio/src/*.cpp   tests/audio/*.cpp   tools/audio_bench/main.cpp (target audio_bench)
```

## Interfaces

```cpp
namespace ct::audio {

struct DeviceInfo { std::wstring id; std::string name; bool is_default = false; };
class AudioDevices {                                   // owns IMMDeviceEnumerator; thread-safe
public:
    HRESULT Init();
    std::vector<DeviceInfo> List(bool capture /* true = microphones, false = render endpoints */);
    std::wstring DefaultId(bool capture);              // eConsole role
    struct Listener { virtual void OnDefaultDeviceChanged(bool capture, const std::wstring& new_id) = 0; virtual void OnDeviceRemoved(const std::wstring& id) = 0; virtual ~Listener() = default; };
    void SetListener(Listener*);                       // IMMNotificationClient impl; callbacks arrive on a COM thread, implementers only set flags/post
    void Shutdown();
};

// A packet of interleaved float frames at 48 kHz stereo. data points into a pool slot owned by the producer; valid until the sink returns.
struct AudioPacket { TimeUs t_us; uint32_t frames; const float* data; int channels; bool silence_fill; };
struct AudioSink { virtual bool OnAudio(uint32_t stream, const AudioPacket&) = 0; virtual void OnAudioError(uint32_t stream, HRESULT) {} virtual ~AudioSink() = default; };

struct CaptureConfig { std::wstring device_id; /* empty = default */ uint32_t stream_id = 0; uint32_t packet_frames = 480; /* 10 ms */ bool follow_default_device = true; };

class WasapiCapture {                                  // microphone, event-driven, thread "ct-audio-mic", MMCSS "Pro Audio"
public:
    HRESULT Init(const CaptureConfig&, AudioDevices&);
    bool Start(int64_t epoch_qpc, AudioSink&);         // first packet may arrive up to 30 ms later; aligner pads from t=0
    void SetPaused(bool);                              // packets are consumed but not delivered; timestamps keep running
    void Stop();
    const Meter& Levels() const;  std::string DeviceName() const;  HRESULT LastError() const;
};
class WasapiLoopback {                                 // system audio, polled every 5 ms, thread "ct-audio-sys", plus a silent keep-alive render stream
public:
    HRESULT Init(const CaptureConfig&, AudioDevices&);
    bool Start(int64_t epoch_qpc, AudioSink&);  void SetPaused(bool);  void Stop();
    const Meter& Levels() const;  std::string DeviceName() const;  HRESULT LastError() const;
};
class WasapiRender {                                   // shared mode, event-driven, float 48 kHz stereo (AUTOCONVERTPCM); used by the keep-alive, audio_bench and P06
public:
    struct Callback { virtual void Render(float* interleaved, uint32_t frames, int channels, int64_t device_position_frames) = 0; };
    HRESULT Init(const std::wstring& device_id, uint32_t buffer_ms /* 20..100 */);
    bool Start(Callback&);  void Stop();
    HRESULT GetClock(int64_t& device_frames, int64_t& qpc) const;   // IAudioClock::GetPosition converted to frames + QPC of the reading
    uint32_t SampleRate() const;  int Channels() const;  double LatencyMs() const;
};

// Pure logic, fully unit-testable: makes a stream gapless and monotonic using device QPC positions.
class StreamAligner {
public:
    void Reset(int sample_rate, int channels);
    // Input: a device packet with its QPC-derived timestamp (media time). Output via callback: zero or more packets whose
    // timestamps are exactly contiguous (t_next = t_prev + frames/rate). Inserts silence for gaps > 1 ms, drops
    // overlapping frames, and trims single frames every 10 ms while |drift| > 1 ms so the stream converges.
    template <class Emit> void Push(TimeUs t_us, const float* interleaved, uint32_t frames, bool discontinuity, Emit emit);
    TimeUs NextExpectedUs() const;  int64_t TotalInserted() const;  int64_t TotalDropped() const;  double DriftMs() const;
};

class Resampler {                                      // polyphase windowed-sinc (Kaiser beta 8, 32 taps, 256 phases, linear phase interpolation)
public:
    bool Init(int in_rate, int out_rate, int channels);
    size_t Process(const float* in, size_t in_frames, float* out, size_t out_capacity_frames, size_t* in_consumed);
    size_t Flush(float* out, size_t out_capacity_frames);
    double Ratio() const;  int Latency() const;
};
namespace dsp {                                        // all functions have scalar + AVX2 paths chosen once at startup (dsp::Init())
    void Init();
    void MixAdd(float* dst, const float* src, size_t n, float gain);
    void ApplyGainRamp(float* buf, size_t frames, int channels, float g0, float g1);
    void FloatToPcm16(const float* in, int16_t* out, size_t n);          // clamp [-1,1], round-to-nearest, TPDF dither optional later
    void Pcm16ToFloat(const int16_t* in, float* out, size_t n);
    void MonoToStereo(const float* in, float* out, size_t frames);
    void DownmixToStereo(const float* in, int in_channels, float* out, size_t frames);   // L/R only, others ignored
    float PeakAbs(const float* in, size_t n);  float Rms(const float* in, size_t n);
    void NoiseGate(float* buf, size_t frames, int channels, float threshold_db, float attack_ms, float release_ms, float rate, struct GateState&);
}
class Meter { public: void Push(const float* interleaved, size_t frames, int channels); float PeakDb(int ch) const; float RmsDb(int ch) const; /* std::atomic<float>, -120 dB floor, readers apply 20 dB/s decay */ };
struct WavWriter { bool Open(std::string_view path, int rate, int channels, bool float32); bool Write(const float* interleaved, size_t frames); bool Close(); };   // RIFF/WAVE, data size patched on Close
struct WavReader { bool Open(std::string_view path); int Rate() const; int Channels() const; size_t Frames() const; size_t Read(float* out, size_t frames); };

} // namespace ct::audio
```

## 3.1 Devices
`CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enum))`; `EnumAudioEndpoints(eCapture|eRender, DEVICE_STATE_ACTIVE, &col)`; name from `IPropertyStore::GetValue(PKEY_Device_FriendlyName)`; id from `IMMDevice::GetId`. `IMMNotificationClient` implemented as a private class with manual refcounting (no ATL); `RegisterEndpointNotificationCallback`. Only forward `eConsole` role changes.

## 3.2 Microphone (`WasapiCapture`)
Init:
1. `device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client)`.
2. `client->GetMixFormat(&mix)`; build the requested `WAVEFORMATEXTENSIBLE`: 32-bit float, 48000 Hz, **channels = mix channels** (clamped 1..8), `SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`, `dwChannelMask` from mix.
3. `client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2'000'000 /* 200 ms */, 0, &req, nullptr)`.
   If it fails with `AUDCLNT_E_UNSUPPORTED_FORMAT`: initialize with `mix` instead and insert our `Resampler` + `Pcm16ToFloat` as needed (log once).
4. `client->SetEventHandle(m_event)`; `client->GetService(IID_PPV_ARGS(&capture))`; `client->GetBufferSize(&buffer_frames)`.
5. Allocate: packet pool (`ObjectPool<PacketBuffer>` of 64 x 4800 frames x 2 ch), `StreamAligner`, `Meter`.

Thread loop (`Thread::Options{ "ct-audio-mic", THREAD_PRIORITY_TIME_CRITICAL, L"Pro Audio" }`):
```
client->Start();
while (WaitForMultipleObjects({stop, event}, FALSE, 2000) != stop) {
    UINT32 n; while (SUCCEEDED(capture->GetNextPacketSize(&n)) && n > 0) {
        BYTE* data; UINT32 frames; DWORD flags; UINT64 dev_pos, qpc_hns;
        hr = capture->GetBuffer(&data, &frames, &flags, &dev_pos, &qpc_hns);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { schedule reinit (default device); break; }
        TimeUs t = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? aligner.NextExpectedUs() : clock.HnsToMediaUs(qpc_hns);
        const float* src = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? zeros : (const float*)data;
        convert channels to stereo into scratch; meter.Push(scratch); 
        aligner.Push(t, scratch, frames, flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, [&](TimeUs tt, const float* p, uint32_t f, bool fill){ if (!paused) deliver(tt, p, f, fill); });
        capture->ReleaseBuffer(frames);
    }
    if (reinit_requested) Reinit();      // tear down, re-Init with the new default id, client->Start(); aligner stays (fills the switch gap with silence)
}
client->Stop();
```
`clock.HnsToMediaUs(qpc_hns)`: `qpc_hns / 10 - epoch_us - pause_offset_us` where `epoch_us = epoch_qpc * 1e6 / freq` (128-bit math). Deliver batches up to `packet_frames` by copying into pool buffers; if the pool is empty count `Stats::C("audio.mic.dropped")`.

## 3.3 System audio (`WasapiLoopback`)
Same as 3.2 with: endpoint `eRender` default (or given id); flags `AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`, **no event flag** (event-driven loopback is unreliable); if `Initialize` rejects the flags, retry with `AUDCLNT_STREAMFLAGS_LOOPBACK` + mix format + our resampler. Thread "ct-audio-sys" polls with `HighResTimer` every 5 ms. Keep-alive: a `WasapiRender` on the same endpoint with a callback that writes zeros, started before the loopback client; it keeps the engine's render path active so loopback packets arrive continuously (silence included). The `StreamAligner` still handles any gap.

## 3.4 Resampler, dsp, Meter
- Resampler: sinc table generated at `Init` (`256 phases x 32 taps`), Kaiser window beta 8.0, cutoff `0.45 * min(in, out)/in`; per output frame: phase = fractional position; interpolate between two phase tables; multi-channel interleaved. Target: SNR >= 90 dB for a -6 dBFS 1 kHz sine 44.1 kHz -> 48 kHz (test computes SNR against an ideal sine after discarding latency).
- `dsp::Init` checks `__cpuidex` for AVX2+FMA; each function has `_scalar` and `_avx2` versions behind a function pointer. The AVX2 TU is compiled with `/arch:AVX2` **only for that file** (`set_source_files_properties(dsp_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)`); it is never called without the runtime check.
- Meter: per packet compute peak and RMS per channel, store `20*log10(max(x, 1e-6))` in atomics.

## 3.5 Device hot-swap and format change
`AudioDevices::Listener` sets an atomic `m_reinit_requested` and the new id; the capture thread performs the switch. The aligner keeps time continuous: a switch that takes 80 ms yields 80 ms of silence at the correct position, never a shift. Tested manually by unplugging a USB mic mid-recording and by changing the default output device while recording system audio.

## Tool: `audio_bench`
`audio_bench --seconds 60 [--tone] [--mic] [--out dir]`: starts `WasapiRender` playing a 1 kHz sine at -12 dBFS (`--tone`), records loopback (and mic with `--mic`) through the real pipeline into `loopback.wav` / `mic.wav` with `WavWriter`, prints per second: packets, inserted/dropped frames, drift ms, meter dB, CPU %. Final analysis of `loopback.wav`: zero-run detection (> 5 ms of |x| < 1e-4 while the tone played = gap, must be 0), frequency estimate via zero-crossings (1000 +- 1 Hz), total frames vs elapsed QPC time (|delta| < 5 ms).

## Tests
- Unit: `StreamAligner` scenarios: perfect stream (no change), 15 ms gap (silence inserted exactly), 3 ms overlap (frames dropped), slow drift of +50 ppm over 10 min (converges, inserted count ~ 30 frames, never a jump > 1 frame per 10 ms), discontinuity flag; `Resampler` SNR; `dsp` scalar vs AVX2 bit-exactness for `MixAdd`/`FloatToPcm16` on 1 M random values; `Meter` dB values; `WavWriter`/`WavReader` round trip.
- Device: enumerate devices (>= 1 render endpoint, skip otherwise); `WasapiRender` plays 500 ms silence without error; `WasapiLoopback` 2 s while `WasapiRender` plays a tone: delivered frames within 48000*2 +- 480, no gaps (aligner inserted == 0 after the first packet), peak dB within +-1 dB of -12; `WasapiCapture` default mic 1 s (skip if none).

## Acceptance (3.6)
`audio_bench --seconds 60 --tone`: gaps 0, frequency 1000 +- 1 Hz, |total frames - elapsed| < 5 ms, process CPU < 1 %. Numbers into `PROGRESS.md`.

## Pitfalls
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` needs `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` when the rate differs; without it you get `AUDCLNT_E_UNSUPPORTED_FORMAT`.
- `GetBuffer` must be followed by `ReleaseBuffer` even for silent packets. Do not keep the pointer after release.
- Loopback on an endpoint with **no active render stream** may deliver no packets at all on some drivers: the keep-alive render stream is mandatory, not an optimization.
- `u64 QPCPosition` from `GetBuffer` is in **100 ns units**, not raw QPC ticks.
- Calling `CoInitializeEx` with STA on the audio thread breaks `IMMNotificationClient` delivery; use MTA on all audio threads.
- Set the thread to MMCSS **inside** the thread after it starts; `AvSetMmThreadCharacteristicsW` returns a handle to revert in `AvRevertMmThreadCharacteristics` before exit.
- Never log at INFO or allocate inside the capture loop; meters and stats are atomics.
