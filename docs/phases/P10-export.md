# P10 · Export (MP4/HEVC, GIF, PNG, M4A, presets, background job)

**Goal:** render the timeline offline, frame-accurately, into MP4 (H.264/HEVC via the hardware encoder), animated
GIF (own encoder), PNG stills and audio-only M4A, with presets, progress, cancel, and pixel parity with the preview.

**Read first:** `docs/04` section 5 (presets); `docs/03` section 7; P04 `Mp4Writer` sync API; P08 `Compositor`; P09 `TimelinePlayer`.
**Depends on:** P08, P04 (P09 for the dialog). **Size:** M (about 2,500 lines).

## Deliverables

```
src/export/include/ct/export/{preset.h,export_job.h,gif_encoder.h,scaler.h}
src/export/src/*.cpp   src/ui/include/ct/ui/panels/export_dialog.h  src/ui/src/panels/export_dialog.cpp
tests/export/*.cpp (incl. tests/common/gif_reader.h)
```

## Interfaces

```cpp
namespace ct::exportng {   // "export" is a keyword-adjacent name in some tooling; namespace is exportng, directory is export
struct Preset {
    std::string id, name;  enum class Container { Mp4, Gif, Png, M4a } container = Container::Mp4;
    struct { media::VideoCodec codec = media::VideoCodec::H264; int width = 0, height = 0; /* 0 = canvas */ int fps_num = 0, fps_den = 1; /* 0 = canvas */ media::RateControl rc = media::RateControl::Vbr; uint32_t bitrate_kbps = 10000, max_bitrate_kbps = 16000; int quality = 75; int gop_s = 2; bool bframes = true; } video;
    struct { uint32_t bitrate_kbps = 192; int sample_rate = 48000; int channels = 2; } audio;
    struct { int max_colors = 256; bool dither = true; bool per_frame_palette = false; int fps = 20; } gif;
    bool fast_start = true;
    static std::vector<Preset> Builtin();                                  // ids listed in docs/04 section 5
    static std::vector<Preset> LoadUser();  bool SaveUser(Err*) const;       // %APPDATA%\Camtesia\presets\<id>.json
    Json ToJson() const;  static bool FromJson(const Json&, Preset&, Err*);
    uint64_t EstimateBytes(TimeUs duration) const;
};
struct ExportRequest { project::Project project_snapshot; Preset preset; std::string output_path; TimeUs range_start = 0, range_end = 0; /* 0,0 = whole project */ };
class ExportJob {
public:
    HRESULT Start(platform::D3DDevice&, render::RenderContext&, audio::AudioDevices&, ExportRequest);   // "ct-export" thread, BELOW_NORMAL
    struct Progress { std::atomic<double> fraction{0}; std::atomic<int64_t> frames_done{0}, frames_total{0}; std::atomic<double> speed_realtime{0}; std::atomic<int64_t> eta_us{0}; std::atomic<bool> finished{false}, cancelled{false}; HRESULT hr = S_OK; std::string error; };
    const Progress& GetProgress() const;
    void Cancel();  bool Join(uint32_t timeout_ms);
};
class GifEncoder {
public:
    struct Options { int max_colors = 256; bool dither = true; bool per_frame_palette = false; bool frame_diff = true; int loop_count = 0; /* 0 = forever */ };
    bool Open(std::string_view path, int width, int height, const Options&);
    bool AddFrame(const uint8_t* bgra, int stride, int delay_cs);          // delay in 1/100 s, min 2
    bool Close();
    // building blocks (tested separately)
    static void BuildPalette(const uint8_t* bgra, int w, int h, int stride, int max_colors, std::vector<uint32_t>& palette);   // median cut on a 5-bit/channel histogram
    static void Quantize(const uint8_t* bgra, int w, int h, int stride, const std::vector<uint32_t>& palette, bool dither, std::vector<uint8_t>& indices);   // Floyd-Steinberg (serpentine)
    static void LzwEncode(const uint8_t* indices, size_t n, int min_code_size, std::vector<uint8_t>& out);   // 12-bit max, clear/EOI, 255-byte sub-blocks
};
struct Scaler { static HRESULT Downscale(render::RenderContext&, ID3D11ShaderResourceView* src, SizeI src, ID3D11RenderTargetView* dst, SizeI dst_size); };   // repeated 2x box passes then bilinear
}
```

## 10.2 Export loop (frame-accurate, same compositor as preview)
```
thread ct-export:
  fps = preset.video.fps or canvas fps; size = preset size or canvas (even); N = ceil((range_end - range_start) * fps / 1e6)
  TimelinePlayer tp (own instance: own MediaCaches, prefetch depth 8, sequential hints; shares RenderContext)
  Compositor comp; canvas RT (canvas size); if size != canvas: scaled RT (size); ColorConverter conv(size -> NV12); Mp4Writer w(no pool: WriteVideoSync with 2 NV12 textures alternating)
  audio_buf float[frame_frames * 2]
  for n in 0..N-1:
     if cancel: break
     t = range_start + FrameTimeUs(n, fps)
     tp.ResolveExact(t, inputs, 2000 ms) ; { Lock; comp.RenderFrame(project, t, inputs, canvas_rtv); if scaled: Scaler::Downscale; conv.Convert(ctx, rgba_tex, nv12[n&1]); }
     w.WriteVideoSync(nv12[n&1], t - range_start)
     tp.RenderAudio(t, t + frame_dur, audio_buf) ; w.WriteAudioSync(0, audio_buf, frames, t - range_start)   // single mixed stereo stream
     progress: frames_done = n+1; speed = (n+1)/fps / elapsed_s; eta
  if cancel: w.Finalize(); fs::Delete(output); progress.cancelled = true   else: hr = w.Finalize(); progress.hr = hr
  progress.finished = true; Async::PostToUi(toast)
```
Audio: `RenderAudio` mixes every audio-bearing clip exactly like playback (same gain/fade/normalize code path in `TimelinePlayer`), frame-aligned chunks, so A/V alignment is exact by construction. `Mp4Writer::WriteVideoSync` requires the two alternating textures to be free (the writer blocks until the encoder released the previous one; use the reuse predicate on a 2-slot `TexturePool`).

## 10.3 MP4 specifics
- `VideoEncodeConfig` from the preset; `low_latency = false`, `bframes` as preset; `fast_start` -> `MF_MPEG4SINK_MOOV_BEFORE_MDAT`.
- HEVC only when `EncoderCaps::HasHardware(Hevc)`; the dialog disables the option otherwise.
- Size presets keep the canvas aspect (height derived, rounded to even); vertical/square outputs come from the canvas aspect-ratio setting (P07/P09), not from the export.

## 10.4 GIF encoder
- Palette: histogram on 15-bit RGB (32,768 bins) weighted by count; median cut splitting the box with the largest `range * count` along its widest channel until `max_colors`; palette entry = weighted mean of the box. Global palette by default from the first frame plus every 30th frame histogram merged (two-pass over a sampled subset); `per_frame_palette` emits a local color table per frame.
- Mapping: nearest color via a 32x32x32 lookup cache filled lazily (exact nearest by squared distance over the palette on miss).
- Dithering: Floyd-Steinberg, serpentine scan, error clamped to [-32, 32] to avoid worms.
- Frame differencing: compare quantized indices with the previous emitted frame; unchanged pixels get the transparent index (reserve 1 palette slot when `frame_diff`), crop the dirty bounding box, disposal method 1 (leave). First frame full.
- Delay: `delay_cs = round(100 / gif.fps)` minimum 2 (50 fps cap); the export loop runs at `gif.fps` (frames sampled from the timeline at that rate).
- Stream: `GIF89a`, LSD with global color table, `NETSCAPE2.0` application extension (loop), per frame GCE (disposal, transparency, delay) + image descriptor (+ local table) + LZW data with `min_code_size = max(2, bits)`.
- Performance: quantization per frame on the IO pool in parallel with rendering the next frame (double buffer the readback: 2 staging textures, `Map` with a 1-frame delay). Target 720p at 20 fps >= 2x real time.

## 10.5 PNG and M4A
- PNG: render the frame at the playhead at canvas size (or preset size via `Scaler`), `Readback::ToBgra8`, `ImageIo::SavePng`. Also JPEG via the same path (quality 92).
- M4A: `Mp4Writer{ has_video = false, audio = {preset.audio} }`, audio loop only, `fast_start`.

## 10.1 Export dialog (`ui::ExportDialog`)
Left: preset list (built-in + user, "Save as preset…"); right: container, codec (HEVC availability), size (canvas/1440p/1080p/720p/custom even), fps (canvas/60/30/24), rate control + bitrate/quality, audio bitrate, GIF options (fps, colors, dither), range (whole / loop region), destination (`IFileSaveDialog` with the right extension, default `%USERPROFILE%\Videos\Camtesia\<project> .mp4`), estimated size, **Export**. Progress modal: bar, `frames_done/total`, `x realtime`, ETA, **Cancel**; completion toast with **Open** / **Show in folder**. The editor stays usable during export (job thread is below normal priority; the dialog can be minimized to a status bar indicator).

## Tests
- Unit: `Preset` JSON round trip and `EstimateBytes`; `LzwEncode` -> `tests/common/gif_reader.h` (own LZW decoder) round trip for random index streams incl. code-size growth and clear codes; `BuildPalette` on a 4-color image returns exactly those colors; `Quantize` with and without dither; full `GifEncoder` on 10 synthetic frames decodes to identical indices with frame diff; `Downscale` (device) PSNR vs a CPU box filter >= 45 dB.
- Device: export a fixture project (3 layers, cursor, annotations, 2 s) to MP4 at quality 90 -> decode frame 30 with `VideoDecoder`, compare against `Compositor` preview of the same frame: PSNR >= 42 dB; PNG export equals the preview readback byte-for-byte; M4A duration 2.0 +- 0.02 s; cancel at 50 % leaves no file.

## Acceptance (10.6)
1080p60 export of a 60 s project with 3 layers + cursor + 2 annotations completes at >= 1.5x real time on the reference machine with the hardware encoder; PNG export is identical to the preview; MP4 export PSNR >= 42 dB vs preview at quality 90; GIF 720p20 >= 2x real time. Numbers in `PROGRESS.md`.

## Pitfalls
- The export must use a **snapshot** of the project (JSON copy) so that editing during export cannot change frames mid-file.
- Never share `MediaCache` instances between the editor and the export job; the hint policies conflict.
- `WriteVideoSync` with only two textures stalls if the encoder holds frames for long (B-frames); use 4 slots when `bframes` is on.
- GIF `min_code_size` must be at least 2 even for 2-color palettes; the clear code is `1 << min_code_size`.
- Staging `Map` immediately after `CopyResource` blocks the GPU pipeline; keep the one-frame delay.
- Downscaling in one bilinear pass from 4K to 720p aliases badly; the 2x box chain is mandatory.
