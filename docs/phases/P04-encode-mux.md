# P04 · Encode and mux (Media Foundation hardware encoder, MP4 sink writer, AAC)

**Goal:** turn NV12 GPU textures and float audio into a standard MP4 with H.264/HEVC video and up to two AAC
streams, using the hardware encoder through the DXGI device manager (zero CPU copies), with a safe texture
reuse scheme, correct timestamps, software fallback, and clean error handling.

**Read first:** `docs/03-architecture.md` sections 4, 5, 8, 10; `docs/07` (Media Foundation rows); `docs/04` section 1 (stream indices).
**Depends on:** P02 (`TexturePool`, `FrameSink`, `ColorConverter`), P03 (`AudioSink`, `dsp`). **Size:** L (about 2,500 lines incl. tools/tests).

## Deliverables

```
src/media/include/ct/media/{mf_runtime.h,encoder_caps.h,mp4_writer.h,encode_config.h,probe.h}
src/media/src/{mf_runtime.cpp,encoder_caps.cpp,mp4_writer.cpp,probe.cpp}
tools/gen_test_media/main.cpp  tools/avsync_source/main.cpp   tests/media/*.cpp
```

## Interfaces

```cpp
namespace ct::media {

struct MfRuntime { static HRESULT Ensure(); static void Shutdown(); };   // refcounted MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET) / MFShutdown

enum class VideoCodec { H264, Hevc };
enum class RateControl { Quality, Vbr, Cbr };
struct VideoEncodeConfig {
    VideoCodec codec = VideoCodec::H264;  SizeI size;  int fps_num = 60, fps_den = 1;
    RateControl rc = RateControl::Quality;  int quality = 70;          // 0..100 for Quality mode
    uint32_t bitrate_bps = 0;  uint32_t max_bitrate_bps = 0;           // 0 = table default (see below)
    int gop_seconds = 2;  bool bframes = false;  bool low_latency = true;  bool prefer_hardware = true;
};
struct AudioEncodeConfig { int sample_rate = 48000; int channels = 2; uint32_t bitrate_bps = 192000; const char* label = "audio"; };
uint32_t DefaultBitrate(const VideoEncodeConfig&);      // H.264: clamp(w*h*fps*0.09, 2 Mbps, 60 Mbps); HEVC: *0.65; max = 1.5x

struct EncoderInfo { std::string name; bool hardware; VideoCodec codec; bool d3d_aware; };
struct EncoderCaps {
    static std::vector<EncoderInfo> Enumerate(VideoCodec);  // MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, HARDWARE|SORTANDFILTER then SYNCMFT|ASYNCMFT), input NV12, output codec
    static bool HasHardware(VideoCodec);
    static std::string Report();                             // multi-line for logs / About dialog
};

class Mp4Writer final : public capture::FrameSink, public audio::AudioSink {
public:
    struct Config { std::string path; bool has_video = true; VideoEncodeConfig video; std::vector<AudioEncodeConfig> audio; bool fast_start = false; uint32_t video_queue = 8; uint32_t audio_queue = 64; };
    HRESULT Init(platform::D3DDevice&, const Config&, platform::TexturePool* nv12_pool);   // pool != null: writer installs the reuse predicate and releases textures when encoded
    bool    Start();                                              // "ct-encode" thread (ABOVE_NORMAL, MMCSS "Playback")
    // FrameSink (capture thread): queue {texture, index, t_us}; false if the queue is full
    bool    OnFrame(ID3D11Texture2D* nv12, uint32_t pool_index, TimeUs t_us) override;
    // For export (no pool): caller-owned texture; the call blocks until the sample was written (synchronous path)
    HRESULT WriteVideoSync(ID3D11Texture2D* nv12, TimeUs t_us);
    // AudioSink (audio threads; one producer per stream): float -> PCM16 into a pooled buffer; batched to <= 4800 frames
    bool    OnAudio(uint32_t stream, const audio::AudioPacket&) override;
    HRESULT WriteAudioSync(uint32_t stream, const float* interleaved, uint32_t frames, TimeUs t_us);   // export path
    HRESULT Finalize();                                           // stop accepting, drain, IMFSinkWriter::Finalize, join. Blocks (seconds). Call from a worker thread.
    void    Abort();                                              // crash path: best-effort Finalize without joining threads; never blocks > 3 s
    HRESULT LastError() const;  std::string EncoderName() const;  bool IsHardware() const;
    struct Stats { std::atomic<uint64_t> video_written{0}, video_rejected{0}, audio_written[4]{}, queue_depth{0}; std::atomic<int64_t> last_video_us{0}, last_audio_us[4]{}; };
    const Stats& GetStats() const;
};

struct ProbeResult { TimeUs duration_us; int video_width, video_height; int fps_num, fps_den; int video_streams, audio_streams; std::string video_codec; };
HRESULT Probe(std::string_view path, ProbeResult&);   // minimal source-reader probe used by tests here; P06 replaces internals with MediaInfo

} // namespace ct::media
```

## 4.1 MF runtime and encoder enumeration
- `MfRuntime::Ensure()` increments a refcount and calls `MFStartup` once; `Shutdown()` on the last release. `Mp4Writer::Init` calls it; the app calls `Ensure()` lazily on first use (startup budget).
- `EncoderCaps::Enumerate`: `MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 }, out{ MFMediaType_Video, MFVideoFormat_H264 | MFVideoFormat_HEVC }`; call `MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n)` then again with `MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER` for software. Name: `act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, ...)`; hardware when `MFT_ENUM_HARDWARE_URL_Attribute` is present; `d3d_aware`: activate (`ActivateObject(IID_IMFTransform)`) and check `GetAttributes` -> `MF_SA_D3D11_AWARE`. Release the transform right away. Cache results per process.

## 4.2 Sink writer configuration (do it in this order)
```
MfRuntime::Ensure();  dev.InitMfManager();
MFCreateAttributes(&attr, 8);
attr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dev.MfManager());
attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, cfg.video.prefer_hardware ? TRUE : FALSE);
attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
attr->SetUINT32(MF_LOW_LATENCY, cfg.video.low_latency ? TRUE : FALSE);
attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
if (cfg.fast_start) attr->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, TRUE);          // export only; costs a second pass by the sink
MFCreateSinkWriterFromURL(wide_path, nullptr, attr, &writer);

// ---- video output type
MFCreateMediaType(&out);
out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
out->SetGUID(MF_MT_SUBTYPE, codec == H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);  MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps_num, fps_den);  MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
if (H264) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High); else out->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH265VProfile_Main_420_8);
SetColorAttrs(out);   // MF_MT_VIDEO_NOMINAL_RANGE=MFNominalRange_16_235, MF_MT_YUV_MATRIX=MFVideoTransferMatrix_BT709, MF_MT_VIDEO_PRIMARIES=MFVideoPrimaries_BT709, MF_MT_TRANSFER_FUNCTION=MFVideoTransFunc_709
writer->AddStream(out, &video_index);

// ---- video input type
MFCreateMediaType(&in);  Video / MFVideoFormat_NV12; same FRAME_SIZE, FRAME_RATE, PAR, INTERLACE; SetColorAttrs(in);
in->SetUINT32(MF_MT_DEFAULT_STRIDE, w);  in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
MFCreateAttributes(&enc, 8);
enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, rc == Quality ? eAVEncCommonRateControlMode_Quality : rc == Vbr ? eAVEncCommonRateControlMode_PeakConstrainedVBR : eAVEncCommonRateControlMode_CBR);
if (rc == Quality) enc->SetUINT32(CODECAPI_AVEncCommonQuality, quality);
enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);  enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, max_bitrate);
enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, gop_seconds * fps_num / fps_den);
enc->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, bframes ? 2 : 0);
enc->SetUINT32(CODECAPI_AVEncCommonLowLatency, low_latency);
hr = writer->SetInputMediaType(video_index, in, enc);
if (FAILED(hr)) hr = writer->SetInputMediaType(video_index, in, nullptr);               // some HW MFTs reject unknown params
if (FAILED(hr) && prefer_hardware) { recreate the writer with MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS=FALSE and repeat; log WARN "software encoder" }
CT_RETURN_IF_FAILED(hr);
// identify the encoder: writer->GetServiceForStream(video_index, GUID_NULL, IID_PPV_ARGS(&mft)); mft->GetAttributes(&a); a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute); hardware = a has MFT_ENUM_HARDWARE_URL_Attribute
// optional runtime tweaks: GetServiceForStream(video_index, GUID_NULL, IID_PPV_ARGS(&codec_api)); use IsSupported/IsModifiable before SetValue

// ---- audio streams (per AudioEncodeConfig)
out: MFMediaType_Audio / MFAudioFormat_AAC; MF_MT_AUDIO_BITS_PER_SAMPLE 16; MF_MT_AUDIO_SAMPLES_PER_SECOND 48000; MF_MT_AUDIO_NUM_CHANNELS 2;
     MF_MT_AUDIO_AVG_BYTES_PER_SECOND = bitrate/8 rounded to one of {12000,16000,20000,24000}; MF_MT_AAC_PAYLOAD_TYPE 0; MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION 0x29
writer->AddStream(out, &audio_index[i]);
in:  MFMediaType_Audio / MFAudioFormat_PCM; BITS 16; SPS 48000; CH 2; MF_MT_AUDIO_BLOCK_ALIGNMENT 4; MF_MT_AUDIO_AVG_BYTES_PER_SECOND 192000; MF_MT_ALL_SAMPLES_INDEPENDENT TRUE
writer->SetInputMediaType(audio_index[i], in, nullptr);

writer->BeginWriting();
```
Stream order is therefore video = 0, then audio in config order (mic first, system second) — matches `meta.json` `stream_index`.

## 4.3 Video texture pool and sample creation
At `Init`, for every texture of the NV12 pool create **once**: `MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf)` and store `buf` as the pool slot's user data. Install the reuse predicate:
```cpp
static bool CanReuse(void* user, uint32_t index) {
    auto* buf = static_cast<IMFMediaBuffer*>(static_cast<TexturePool*>(user)->UserData(index));
    buf->AddRef(); ULONG rc = buf->Release();
    return rc == 1;                     // only the pool holds it => the encoder released the sample
}
```
Per frame on the encode thread:
```
buf.As(&buf2d); buf2d->GetContiguousLength(&len); buf->SetCurrentLength(len);
MFCreateSample(&s); s->AddBuffer(buf); s->SetSampleTime(t_us * 10); s->SetSampleDuration(frame_dur_100ns);
hr = writer->WriteSample(video_index, s);     // s released right after; the writer keeps its own reference until encoded
pool->Release(index);                          // slot goes back to the free list; the predicate blocks reuse until rc == 1
```
Video queue: `SpscQueue<VideoItem, 8>` (`{index, t_us}`); `OnFrame` returns false when full. The pacer counts it as `dropped_sink`.

## 4.4 Audio
`OnAudio` converts to PCM16 with `dsp::FloatToPcm16` into a pooled `int16_t[4800*2]` buffer, accumulating consecutive packets of the same stream until 4800 frames or 100 ms elapsed, then enqueues `{stream, buffer, frames, t_us}` to `SpscQueue<AudioItem, 64>`. Encode thread: `MFCreateMemoryBuffer(bytes, &mb)`, `Lock`/`memcpy`/`Unlock`, `SetCurrentLength`, sample time `t_us*10`, duration `frames * 10'000'000 / 48000`, `WriteSample(audio_index[stream], s)`.

Timestamp policy:
- Audio streams always begin at `t = 0`: before the first real packet, the writer emits silence from 0 to the packet's `t_us` (the `StreamAligner` normally does this already; the writer guards it).
- Video begins at the first captured tick (small positive offset is fine; players honour the edit list the sink writes). The test `avsync` verifies the offset is preserved; if a player ignores it, the fallback is to send a black frame at `t = 0` (`Config.pad_video_start = true`).
- Encode thread ordering: always write the item with the smallest timestamp among the queue heads; if one kind of queue is empty, write the other but never let any stream run more than 500 ms ahead of the slowest producing stream unless that producer is stopped.

## 4.5 HEVC, fallback, errors
- HEVC: `MFVideoFormat_HEVC`, profile `eAVEncH265VProfile_Main_420_8`; `EncoderCaps::HasHardware(Hevc)` gates the UI option; software HEVC encoding is not offered.
- Any `WriteSample` failure: store `LastError`, stop accepting input (`OnFrame` returns false), drain nothing further, try `Finalize()` once (a file with the data so far is usually playable), report via `OnError` to the owner. `ERROR_DISK_FULL`/`ERROR_HANDLE_DISK_FULL` get a specific message.
- Device removed: the encoder fails with `DXGI_ERROR_DEVICE_REMOVED`; same path.
- `Finalize()` time budget: typically < 1 s, up to several seconds for long files; UI shows "Finalizing...". `Abort()` sets a 3 s watchdog and returns regardless.

## Tools
- `gen_test_media`: renders synthetic frames with D3D11 (moving color bars, a 12-digit 7-segment frame counter drawn as rectangles inside `(32,32)-(432,96)`, white full-frame flash on every 60th frame) -> `ColorConverter` -> `Mp4Writer`; audio: 1 kHz tone -12 dBFS with a 2 kHz 20 ms beep aligned with each flash. Writes `tests/data/pattern_1080p60_5s.mp4`, `pattern_4k60_3s.mp4` (run once per machine; committed files come from the reference machine).
- `avsync_source`: see `docs/06` section 4.

## Tests
- Unit: `DefaultBitrate` table; AAC byte-rate rounding; timestamp interleaving policy (pure function tested with synthetic queues).
- Device: `EncoderCaps::Enumerate(H264)` contains at least the Microsoft software encoder; `Mp4Writer` 2 s 1080p60 synthetic + 2 audio streams -> `Probe`: duration 2.0 +- 0.05 s, 1 video + 2 audio streams, fps 60; reuse predicate turns true for all slots within 500 ms after `Finalize`; `Finalize` on a path in a non-existent directory fails with a clear `LastError`; HEVC round trip (skipped without HW HEVC); software path forced via `prefer_hardware=false` works.

## Acceptance (4.6)
`capture_bench --monitor 0 --fps 60 --seconds 600 --encode out.mp4 --mic --system` (add the `--encode` option to the bench, wiring `Mp4Writer` as the sink and `WasapiCapture/Loopback` as producers): 0 `dropped_*`, file plays in Movies & TV and VLC with audio in sync at start and end (clap test from `docs/06` section 4 gives |offset| < 20 ms), size within 10 % of `bitrate * 600 s / 8` in VBR/CBR mode, encoder name logged as hardware on the reference machine. Numbers into `PROGRESS.md`.

## Pitfalls
- Forgetting `SetCurrentLength` on the DXGI surface buffer makes `WriteSample` fail with `MF_E_BUFFERTOOSMALL` or produces empty frames.
- Do not call `IMFSinkWriter` methods from two threads at once; everything goes through the encode thread (sync export paths post to it and wait).
- `MF_SINK_WRITER_DISABLE_THROTTLING` means **you** must bound the queues; without it the writer blocks `WriteSample` to pace you, which would stall the capture thread.
- Odd width/height, or a frame rate the HW encoder rejects (e.g. 144), fails at `SetInputMediaType`: the region clamp (P02) guarantees even sizes; clamp fps to the encoder by trying 120, 60, 30.
- `GetServiceForStream` may fail for software encoders; treat as optional.
- Hardware encoders need the texture to stay untouched until released; never write into a pool slot whose predicate says busy. Debug builds `CT_ASSERT` this in `TexturePool::Acquire`.
- The AAC encoder expects 16-bit PCM input; float input is rejected on some Windows builds.
