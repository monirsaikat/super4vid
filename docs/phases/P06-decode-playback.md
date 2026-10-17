# P06 · Decode and playback (source reader, frame cache, audio engine, clock)

**Goal:** open any MP4/MOV/M4A/MP3/WAV/PNG/JPG, decode video on the GPU into NV12 textures with exact timestamps,
decode audio to float 48 kHz stereo, seek fast, keep a bounded frame cache fed by a prefetch thread, play audio
through WASAPI with an audio-driven clock, and present frames in sync. Verified with a standalone `player_demo`.

**Read first:** `docs/03-architecture.md` sections 3, 4, 6, 8, 9; `docs/07` (Source Reader rows, IAudioClock); `docs/06` section 2 (test media).
**Depends on:** P02 (device, `Nv12Blit` from P05 if present, else create it here), P03 (`WasapiRender`, `Resampler`, `dsp`). **Size:** L (about 4,000 lines).

## Deliverables

```
src/media/include/ct/media/{media_info.h,video_decoder.h,audio_decoder.h,peaks.h,image_io.h}
src/media/src/{media_info.cpp,video_decoder.cpp,audio_decoder.cpp,peaks.cpp,image_io.cpp}
src/playback/include/ct/playback/{frame_cache.h,media_cache.h,cache_budget.h,audio_engine.h,playback_clock.h,player.h}
src/playback/src/*.cpp   tools/player_demo/main.cpp   tests/media/*.cpp  tests/playback/*.cpp  tests/common/frame_index.h
```

## Interfaces

```cpp
namespace ct::media {
struct StreamInfo { enum class Kind { Video, Audio } kind; int index; std::string codec; int width = 0, height = 0; int fps_num = 0, fps_den = 1; int rotation_deg = 0; int sample_rate = 0, channels = 0; TimeUs duration_us = 0; };
struct MediaInfo {
    TimeUs duration_us = 0;  bool seekable = true;  std::vector<StreamInfo> streams;  bool is_image = false;
    static HRESULT Load(std::string_view path, MediaInfo& out);   // source reader without D3D (fast); images via WIC
    int FirstVideo() const;  std::vector<int> AudioStreams() const;
};

struct DecodedFrame { ID3D11Texture2D* nv12 = nullptr; uint32_t pool_index = 0; TimeUs pts_us = 0; TimeUs duration_us = 0; SizeI size; };
class VideoDecoder {                       // one thread at a time; the owner serializes Seek/ReadFrame
public:
    struct Config { std::string path; int stream_index = -1; uint32_t pool_size = 8; bool allow_hardware = true; };
    HRESULT Init(platform::D3DDevice&, const Config&);
    HRESULT Seek(TimeUs t_us);             // SetCurrentPosition; the next ReadFrame returns the preceding keyframe; caller decodes forward
    HRESULT ReadFrame(DecodedFrame& out);  // S_OK frame; S_FALSE end of stream; copies decoder output (aperture-cropped) into a pool NV12 texture
    void    ReleaseFrame(uint32_t pool_index);
    SizeI Size() const;  int FpsNum() const;  int FpsDen() const;  int RotationDeg() const;  TimeUs Duration() const;  bool IsHardware() const;
};
class AudioDecoder {
public:
    struct Config { std::string path; int stream_index = -1; };
    HRESULT Init(const Config&);           // output float 48 kHz stereo (reader conversion; falls back to our Resampler + downmix)
    HRESULT Seek(TimeUs t_us);
    HRESULT Read(float* interleaved_out, uint32_t capacity_frames, uint32_t& frames_out, TimeUs& pts_us);   // S_FALSE at EOS
    TimeUs Duration() const;  int SourceRate() const;  int SourceChannels() const;
};
struct Peaks { int per_second = 100; std::vector<float> min, max; TimeUs duration_us; static HRESULT Build(std::string_view path, int stream, Peaks&); static bool LoadCache(std::string_view media_path, int stream, Peaks&); static void SaveCache(std::string_view media_path, int stream, const Peaks&); };
struct ImageIo {
    static HRESULT LoadRgba(std::string_view path, std::vector<uint8_t>& rgba, SizeI&);                         // WIC, converts to 32bppPBGRA
    static HRESULT LoadToTexture(platform::D3DDevice&, std::string_view path, Microsoft::WRL::ComPtr<ID3D11Texture2D>&, SizeI&);
    static HRESULT SavePng(std::string_view path, const uint8_t* bgra, int w, int h, int stride);
    static HRESULT SaveJpeg(std::string_view path, const uint8_t* bgra, int w, int h, int stride, float quality);
};
}

namespace ct::playback {
struct CacheBudget { static void SetBytes(size_t); static size_t Bytes(); static size_t Used(); static void Add(int64_t delta); };   // global across caches
struct FrameRef { ID3D11ShaderResourceView* y = nullptr; ID3D11ShaderResourceView* uv = nullptr; ID3D11Texture2D* nv12 = nullptr; TimeUs pts_us = 0, duration_us = 0; SizeI size; };
class MediaCache {                          // one per media file; owns decoders, "ct-decode-N" prefetch thread, LRU frame cache, peaks, thumbnails
public:
    HRESULT Open(platform::D3DDevice&, std::string_view path);
    const media::MediaInfo& Info() const;
    // Non-blocking. exact=false means a nearest earlier frame was returned (or none) and the exact one is being prefetched; the UI re-requests next frame.
    bool GetVideoFrame(TimeUs t_us, FrameRef& out, bool& exact);
    void Hint(TimeUs playhead_us, double rate, bool playing);        // prefetch window: playing -> [t-0.5 s, t+2 s] in rate direction; paused -> [t-0.25 s, t+0.5 s]
    class AudioReader { public: HRESULT Seek(TimeUs); uint32_t Read(float* out, uint32_t frames, TimeUs& pts); };   // sequential, one per consumer (clip)
    std::unique_ptr<AudioReader> OpenAudio(int stream_index);
    bool Thumbnail(TimeUs t_us, ID3D11ShaderResourceView*& srv);   // 160x90 RGBA; false = not ready yet (requested on the IO pool), cached up to 2,000 per media
    const media::Peaks* GetPeaks(int stream_index);                // nullptr until built (IO pool, cached on disk under %LOCALAPPDATA%\Camtesia\cache\)
    void Close();
};
class AudioEngine {                          // WASAPI render + mixing callback; master clock when playing
public:
    struct Source { virtual uint32_t Pull(float* interleaved, uint32_t frames, TimeUs from_us, double rate) = 0; virtual ~Source() = default; };
    HRESULT Init(audio::AudioDevices&);
    bool Start(Source&);  void Stop();
    void Play(TimeUs from_us, double rate);  void Pause();  bool Playing() const;
    TimeUs PositionUs() const;               // from_us + (device_frames_now - device_frames_at_play) * rate / 48000 - output latency, interpolated with QPC between IAudioClock reads
    void SetVolume(float);  void SetMuted(bool);  float Volume() const;
};
class PlaybackClock {
public:
    void Attach(AudioEngine* engine_or_null);
    void Play(TimeUs from_us, double rate);  void Pause();  void Seek(TimeUs);  void SetRate(double);
    TimeUs Now() const;  bool Playing() const;  double Rate() const;
};
class Player {                               // single-file player used by player_demo and as the reference integration for P09
public:
    HRESULT Open(platform::D3DDevice&, audio::AudioDevices&, std::string_view path);
    void Play();  void Pause();  void TogglePlay();  void Seek(TimeUs);  void StepFrames(int delta);  void SetRate(double);
    bool Tick(FrameRef& frame_to_show, TimeUs& now_us);   // picks the frame with pts <= now < pts+dur; returns true if a frame is available
    const media::MediaInfo& Info() const;  struct Stats { double seek_to_frame_ms; uint64_t frames_shown, frames_late; size_t cache_bytes; } GetStats() const;
};
}
```

## 6.1 `MediaInfo`
`MFCreateSourceReaderFromURL(path, attrs{ MF_SOURCE_READER_DISABLE_DXVA = TRUE, MF_LOW_LATENCY = FALSE }, &r)`; for each stream `i` until `MF_E_INVALIDSTREAMNUMBER`: `GetNativeMediaType(i, 0)` -> major type, subtype name (`H264`, `HEVC`, `AAC`, `MP3`, `PCM`...), size via `MFGetAttributeSize(MF_MT_FRAME_SIZE)`, fps via `MF_MT_FRAME_RATE`, rotation `MF_MT_VIDEO_ROTATION`, audio rate/channels; duration `GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION)`; seekable from `MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS & MFMEDIASOURCE_CAN_SEEK`. Images (`.png .jpg .jpeg .bmp .webp` by extension): WIC decoder for size, `is_image = true`, duration 0.

## 6.2 `VideoDecoder`
```
attrs: MF_SOURCE_READER_D3D_MANAGER = dev.MfManager(); MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = TRUE; MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE; MF_SOURCE_READER_DISABLE_DXVA = !allow_hardware
MFCreateSourceReaderFromURL; SetStreamSelection(ALL, FALSE); SetStreamSelection(video, TRUE)
output type: Video / NV12 (partial) -> SetCurrentMediaType(video, nullptr, t)
GetCurrentMediaType -> frame size (may be padded, e.g. 1088), MF_MT_DEFAULT_STRIDE, MF_MT_MINIMUM_DISPLAY_APERTURE (MFVideoArea: the real picture rect), frame rate; pool textures use the aperture size (even).
ReadFrame loop:
  ReadSample(video, 0, &actual, &flags, &ts, &sample)
  ENDOFSTREAM -> S_FALSE; CURRENTMEDIATYPECHANGED -> re-read type, recreate the pool if the aperture size changed; STREAMTICK or null sample -> continue
  buffer = sample->GetBufferByIndex(0); if buffer.As(&dxgi) succeeds: dxgi->GetResource(&tex); dxgi->GetSubresourceIndex(&sub); { Lock; ctx->CopySubresourceRegion(pool[i], 0, 0,0,0, tex, sub, &aperture_box); }
  else (system memory, software decoder): buffer.As(&b2d2); b2d2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch, &start, &len); { Lock; ctx->UpdateSubresource(pool[i], 0, nullptr, scanline0 + aperture offset, pitch, 0); } b2d2->Unlock2D();
  pts = ts / 10; duration = sample->GetSampleDuration()/10 or 1e6*fps_den/fps_num
Seek(t): PROPVARIANT v{ VT_I8, t*10 }; reader->SetCurrentPosition(GUID_NULL, v). Then the caller reads until pts + duration > t. Record Stats::T("decode.seek_to_frame").
```
Pool textures: `DXGI_FORMAT_NV12`, `BIND_SHADER_RESOURCE`, even size. Expose Y/UV SRVs per pool slot (created once: `R8_UNORM` and `R8G8_UNORM` views) through `FrameRef`.

## 6.3 `AudioDecoder`
`SetCurrentMediaType(audio, nullptr, { Audio, MFAudioFormat_Float, MF_MT_AUDIO_SAMPLES_PER_SECOND 48000, NUM_CHANNELS 2, BITS 32, BLOCK_ALIGNMENT 8, AVG_BYTES_PER_SECOND 384000 })`. If it fails: set `{ Audio, Float }` only, read the resulting rate/channels, and convert with `audio::Resampler` + `dsp::DownmixToStereo/MonoToStereo`. `Read` locks the buffer and copies; partial frames carried over. `Seek` + discard until `pts >= t` with sample accuracy (skip `(t - pts) * 48000 / 1e6` frames of the first packet).

## 6.4 `MediaCache`, `FrameCache`, prefetch
- Frame cache: `std::vector<Entry>` sorted by pts with binary search; each entry owns an NV12 texture copied from the decoder pool (so the decoder keeps running); bytes = `w * h * 1.5`; eviction: outside the hint window first, then LRU; `CacheBudget::Used` global.
- Prefetch thread ("ct-decode-N", above normal): wait on a hint event; compute the window; if the decoder position is outside `[window.start - 3 s, window.start]` seek to `window.start`; decode forward while `pts < window.end` and budget allows; between frames check for a new hint (cancel and re-seek on far jumps). On `GetVideoFrame` miss, the UI gets the nearest earlier cached frame (`exact = false`) so scrubbing feels instant, then the exact frame on a later tick.
- Thumbnails: a second software `VideoDecoder` (`allow_hardware=false` to not contend with the main decoder) on the IO pool; seek, decode one frame, `Nv12Blit` -> 160x90 RGBA RT (linear downsample via a 2-pass box filter for quality), keep the SRV. Requests coalesce by time bucket (1 s).
- Peaks: full decode pass with `AudioDecoder` on the IO pool; per 10 ms window store min/max of the mono mix; cached as `<fnv64(path+size+mtime)>.peaks` (header + float arrays).

## 6.5 `AudioEngine`
`WasapiRender` with a 20 ms buffer. Callback: if not playing -> zeros; else `source.Pull(buf, frames, cursor_us, rate)`; `cursor_us += frames * rate * 1e6 / 48000`; apply volume with a 5 ms ramp on changes; measure peak for the UI meter. Rate != 1: the source pulls `frames * rate` native frames and the engine resamples linearly into `frames` (varispeed with pitch change is intended for J/K/L). Clock: `IAudioClock::GetPosition(&pos, &qpc)` -> frames = `pos * 48000 / frequency`; `PositionUs = play_from_us + (frames - frames_at_play) * rate * 1e6 / 48000 - latency_us`, interpolated with QPC between reads; monotonic clamp (never goes backwards while playing).

## 6.6 `PlaybackClock` and `Player`
Clock uses the audio engine position when attached and playing with audible audio; otherwise QPC: `now = seek_base + (qpc_now - qpc_play) * rate`. `Player::Tick`: `now = clock.Now()`; `cache.Hint(now, rate, playing)`; `cache.GetVideoFrame(now, ref, exact)`; frames whose `pts + dur < now - 50 ms` count as late. Step: pause, `Seek(FrameTimeUs(FrameIndexAt(now) + delta))`.

## Tool: `player_demo`
`player_demo <file> [--avsync] [--seek-bench] [--count-frames] [--stats]`: Win32 window + swapchain, draws the current frame with `Nv12Blit` (letterboxed), Space/arrows/J/K/L, overlay with pts, clock, cache bytes, late frames. `--count-frames` decodes everything and prints the count and first/last pts. `--seek-bench` performs 50 random seeks and prints p50/p95 seek-to-exact-frame ms. `--avsync` implements `docs/06` section 4 analysis (flash detection in the video, beep detection in the chosen audio stream, offsets and drift).

## Tests
- `tests/common/frame_index.h`: reads the 7-segment frame counter rendered by `gen_test_media` from an RGBA readback.
- Unit: frame cache LRU/budget/eviction order; pick-frame-for-time with gaps; clock math; aperture box computation.
- Device: `MediaInfo` of `pattern_1080p60_5s.mp4` (duration 5 s +- 1 ms, 1 video 60 fps 1920x1080 H264, 1 audio 48 kHz); decode all frames -> 300 frames, indices 0..299 strictly increasing; seek to 2.5 s -> first exact frame index 150, time < 150 ms (log p95 over 20 seeks); `AudioDecoder` total frames 240000 +- 48, tone 1 kHz +- 1 Hz; `Peaks::Build` length 500; thumbnails return within 1 s; `AudioEngine` plays 500 ms of a tone, `PositionUs` advances 500 +- 30 ms; `ImageIo` PNG round trip exact; MP3/M4A/WAV open and decode (skip files that are missing).

## Acceptance (6.7)
`player_demo pattern_4k60_3s.mp4` plays at 60 fps with `frames_late == 0` and audio in sync; `--seek-bench` p95 < 150 ms on the 1080p pattern; process CPU < 10 % during 4K60 playback on the reference machine; `--avsync` on a recorded clap test reports |offset| < 20 ms.

## Pitfalls
- Decoder output textures are `D3D11_BIND_DECODER` array slices; you cannot create SRVs on them, hence the copy into the pool.
- `MF_MT_MINIMUM_DISPLAY_APERTURE` is often absent; default to the full frame size, but handle 1920x1088 -> 1920x1080 when present.
- `SetCurrentPosition` is asynchronous in spirit: the first `ReadSample` after it may still return a frame from before the seek on some sources; discard frames with `pts < target - 2 s` after a seek.
- Software decoders deliver system-memory buffers even with a D3D manager set; the `IMF2DBuffer2` path must work.
- `IAudioClock::GetFrequency` is in bytes/s for some devices and Hz for others; the docs define position units via `GetFrequency`; divide `pos` by frequency to get seconds, then multiply by 48000.
- Never hold the `D3DDevice::Lock` while calling `ReadSample` (the decoder's internal threads need the device); lock only around the copy.
- The prefetch thread must not seek on every tiny hint change; use a hysteresis of half the window.
