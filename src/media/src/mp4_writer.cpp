#define CT_LOG_CATEGORY "media"
#include <ct/media/mp4_writer.h>

#include <ct/audio/dsp.h>
#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/object_pool.h>
#include <ct/core/spsc_queue.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/media/encoder_caps.h>
#include <ct/media/mf_runtime.h>
#include <ct/platform/win.h>

#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {

constexpr uint32_t kQueueCapacity = 64;            // SpscQueue storage; Config limits the depth used
constexpr int kAudioRate = 48000;
constexpr int64_t kGapFillUs = 2000;               // audio gaps larger than this are filled with silence
constexpr uint32_t kSyncWaitMs = 5000;             // WriteVideoSync/WriteAudioSync back-pressure limit

TimeUs FramesToUs(int64_t frames) { return frames * 1'000'000 / kAudioRate; }
int64_t FramesTo100ns(int64_t frames) { return frames * 10'000'000 / kAudioRate; }
int64_t UsToFrames(TimeUs us) { return (us * kAudioRate + 500'000) / 1'000'000; }

struct VideoItem {
    uint32_t index = 0;
    TimeUs t_us = 0;
    int64_t qpc = 0;   // when it was queued (latency stat)
};

struct AudioItem {
    uint32_t buf = 0;
    uint32_t frames = 0;
    TimeUs t_us = 0;
};

HRESULT SetColorAttrs(IMFMediaType* t) {
    CT_RETURN_IF_FAILED(t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    CT_RETURN_IF_FAILED(t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    CT_RETURN_IF_FAILED(t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    CT_RETURN_IF_FAILED(t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    return S_OK;
}

HRESULT SetVideoFrameAttrs(IMFMediaType* t, const VideoEncodeConfig& v) {
    CT_RETURN_IF_FAILED(MFSetAttributeSize(t, MF_MT_FRAME_SIZE, static_cast<UINT32>(v.size.w), static_cast<UINT32>(v.size.h)));
    CT_RETURN_IF_FAILED(MFSetAttributeRatio(t, MF_MT_FRAME_RATE, static_cast<UINT32>(v.fps_num), static_cast<UINT32>(v.fps_den)));
    CT_RETURN_IF_FAILED(MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    CT_RETURN_IF_FAILED(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    return SetColorAttrs(t);
}

bool IsDiskFull(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL);
}

} // namespace

struct Mp4Writer::Impl {
    struct AudioStream {
        AudioEncodeConfig cfg;
        DWORD index = 0;
        uint32_t limit = 0;
        SpscQueue<AudioItem, kQueueCapacity> queue;
        ObjectPool<int16_t*> pool;
        std::unique_ptr<int16_t[]> storage;
        // Producer side (OnAudio / WriteAudioSync thread; Finalize after the producers stopped).
        int64_t acc_buf = -1;
        uint32_t acc_frames = 0;
        TimeUs acc_t = 0;
        std::atomic<int> in_call{0};
        std::atomic<int64_t> frontier_us{0};
        std::atomic<int64_t> last_push_qpc{0};
        // Encode-thread side.
        bool staged = false;
        AudioItem head;
        bool started = false;
        TimeUs next_us = 0;   // end of the data written so far
    };

    Mp4Writer* self = nullptr;
    platform::D3DDevice* dev = nullptr;
    Config cfg;
    bool mf_ok = false;
    bool init_ok = false;
    ComPtr<IMFSinkWriter> writer;
    std::string encoder_name;

    // Video.
    DWORD video_index = 0;
    platform::TexturePool* pool = nullptr;   // external (capture) or &internal_pool
    bool external_pool = false;
    platform::TexturePool internal_pool;
    std::vector<ComPtr<IMFMediaBuffer>> vbufs;
    SpscQueue<VideoItem, kQueueCapacity> vqueue;
    uint32_t vlimit = 8;
    int64_t frame_dur_100ns = 166'666;
    std::atomic<int> video_in_call{0};
    std::atomic<int64_t> video_frontier_us{0};
    std::atomic<int64_t> video_last_push_qpc{0};
    bool vstaged = false;
    VideoItem vhead;

    std::vector<std::unique_ptr<AudioStream>> audio;
    std::vector<int16_t> silence;

    bool started = false;
    Event wake{false, false};
    Event progress{false, false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> abort{false};
    std::atomic<bool> failed{false};
    int64_t start_qpc = 0;
    bool finalized = false;
    HRESULT finalize_hr = S_OK;
    uint64_t samples_written = 0;
    uint64_t inject_after = UINT64_MAX;
    HRESULT inject_hr = S_OK;
    uint32_t finalize_delay_ms = 0;   // test hook: simulates a slow IMFSinkWriter::Finalize

    // State ct-encode publishes. It lives here, not in Mp4Writer, so a leaked Impl (crash path, see ~Mp4Writer) never
    // writes into a destroyed writer.
    Stats stats;
    std::atomic<HRESULT> last_error{0};
    std::atomic<bool> accepting{false};

    // Declared last: destroyed first, so ~Thread's join finishes before any state the thread uses is destroyed.
    Thread thread;

    static bool CanReuse(void* user, uint32_t index) {
        auto* impl = static_cast<Impl*>(user);
        if (index >= impl->vbufs.size()) return true;
        IMFMediaBuffer* buf = impl->vbufs[index].Get();
        buf->AddRef();
        const ULONG rc = buf->Release();
        return rc == 1;   // only the writer's slot table holds it: every sample with this texture is gone
    }

    // ---- setup ------------------------------------------------------------------------------------------------

    HRESULT CreateWriter(bool hardware) {
        writer.Reset();
        ComPtr<IMFAttributes> attr;
        CT_RETURN_IF_FAILED(MFCreateAttributes(attr.GetAddressOf(), 8));
        if (cfg.has_video) CT_RETURN_IF_FAILED(attr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dev->MfManager()));
        CT_RETURN_IF_FAILED(attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE));
        CT_RETURN_IF_FAILED(attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        CT_RETURN_IF_FAILED(attr->SetUINT32(MF_LOW_LATENCY, cfg.video.low_latency ? TRUE : FALSE));
        CT_RETURN_IF_FAILED(attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
        // Config::fast_start is ignored (DECISIONS P04 4.3): MF_MPEG4SINK_MOOV_BEFORE_MDAT fails Finalize with
        // E_ACCESSDENIED on the default (write-only) byte stream and writes a corrupt file (mdat header lost, data
        // duplicated) on a read/write one. The moov stays at the end; P10 can relocate it after Finalize if needed.
        if (cfg.fast_start) CT_LOG_WARN("Mp4Writer: fast_start is not supported; the moov box is written at the end");
        const std::wstring wpath = Utf8ToWide(cfg.path);
        const HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attr.Get(), writer.GetAddressOf());
        if (FAILED(hr)) CT_LOG_ERROR("MFCreateSinkWriterFromURL(%s) failed: %s", cfg.path.c_str(), HrToString(hr).c_str());
        return hr;
    }

    HRESULT EncoderParams(ComPtr<IMFAttributes>& enc) {
        const VideoEncodeConfig& v = cfg.video;
        CT_RETURN_IF_FAILED(MFCreateAttributes(enc.GetAddressOf(), 8));
        const UINT32 mode = v.rc == RateControl::Quality ? eAVEncCommonRateControlMode_Quality
                            : v.rc == RateControl::Vbr   ? eAVEncCommonRateControlMode_PeakConstrainedVBR
                                                         : eAVEncCommonRateControlMode_CBR;
        CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, mode));
        if (v.rc == RateControl::Quality) CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncCommonQuality, static_cast<UINT32>(std::clamp(v.quality, 0, 100))));
        CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, EffectiveBitrate(v)));
        if (v.rc != RateControl::Cbr) CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, EffectiveMaxBitrate(v)));
        CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(std::max(1, v.gop_seconds * v.fps_num / v.fps_den))));
        CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, v.bframes ? 2 : 0));
        CT_RETURN_IF_FAILED(enc->SetUINT32(CODECAPI_AVEncCommonLowLatency, v.low_latency ? TRUE : FALSE));
        return S_OK;
    }

    // The encoder refused the parameter set as a whole: apply each value through ICodecAPI where it is supported.
    void ApplyCodecApiOneByOne() {
        ComPtr<ICodecAPI> api;
        if (FAILED(writer->GetServiceForStream(video_index, GUID_NULL, IID_PPV_ARGS(api.GetAddressOf()))) || !api) {
            CT_LOG_WARN("Mp4Writer: encoder has no ICodecAPI; using its default rate control");
            return;
        }
        const VideoEncodeConfig& v = cfg.video;
        struct P { const GUID* id; UINT32 value; const char* name; bool wanted; };
        const UINT32 mode = v.rc == RateControl::Quality ? eAVEncCommonRateControlMode_Quality
                            : v.rc == RateControl::Vbr   ? eAVEncCommonRateControlMode_PeakConstrainedVBR
                                                         : eAVEncCommonRateControlMode_CBR;
        const P params[] = {
            {&CODECAPI_AVEncCommonRateControlMode, mode, "RateControlMode", true},
            {&CODECAPI_AVEncCommonQuality, static_cast<UINT32>(std::clamp(v.quality, 0, 100)), "Quality", v.rc == RateControl::Quality},
            {&CODECAPI_AVEncCommonMeanBitRate, EffectiveBitrate(v), "MeanBitRate", true},
            {&CODECAPI_AVEncCommonMaxBitRate, EffectiveMaxBitrate(v), "MaxBitRate", v.rc != RateControl::Cbr},
            {&CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(std::max(1, v.gop_seconds * v.fps_num / v.fps_den)), "GOPSize", true},
            {&CODECAPI_AVEncMPVDefaultBPictureCount, v.bframes ? 2u : 0u, "BPictureCount", true},
            {&CODECAPI_AVEncCommonLowLatency, v.low_latency ? 1u : 0u, "LowLatency", true},
        };
        for (const P& p : params) {
            if (!p.wanted) continue;
            if (api->IsSupported(p.id) != S_OK) {
                CT_LOG_WARN("Mp4Writer: encoder does not support %s", p.name);
                continue;
            }
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = p.value;
            const HRESULT hr = api->SetValue(p.id, &var);
            if (FAILED(hr)) CT_LOG_WARN("Mp4Writer: ICodecAPI %s = %u failed: %s", p.name, p.value, HrToString(hr).c_str());
        }
    }

    HRESULT AddVideoStream() {
        const VideoEncodeConfig& v = cfg.video;
        ComPtr<IMFMediaType> out;
        CT_RETURN_IF_FAILED(MFCreateMediaType(out.GetAddressOf()));
        CT_RETURN_IF_FAILED(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        CT_RETURN_IF_FAILED(out->SetGUID(MF_MT_SUBTYPE, v.codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AVG_BITRATE, EffectiveBitrate(v)));
        CT_RETURN_IF_FAILED(SetVideoFrameAttrs(out.Get(), v));
        if (v.codec == VideoCodec::H264) CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
        else CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH265VProfile_Main_420_8));
        CT_RETURN_IF_FAILED(writer->AddStream(out.Get(), &video_index));

        ComPtr<IMFMediaType> in;
        CT_RETURN_IF_FAILED(MFCreateMediaType(in.GetAddressOf()));
        CT_RETURN_IF_FAILED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        CT_RETURN_IF_FAILED(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        CT_RETURN_IF_FAILED(SetVideoFrameAttrs(in.Get(), v));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(v.size.w)));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        ComPtr<IMFAttributes> enc;
        CT_RETURN_IF_FAILED(EncoderParams(enc));
        HRESULT hr = writer->SetInputMediaType(video_index, in.Get(), enc.Get());
        if (FAILED(hr)) {
            CT_LOG_WARN("Mp4Writer: SetInputMediaType with encoder parameters failed (%s); retrying without",
                        HrToString(hr).c_str());
            hr = writer->SetInputMediaType(video_index, in.Get(), nullptr);
            if (SUCCEEDED(hr)) ApplyCodecApiOneByOne();
        }
        return hr;
    }

    HRESULT AddAudioStream(AudioStream& a) {
        const UINT32 ch = static_cast<UINT32>(a.cfg.channels);
        ComPtr<IMFMediaType> out;
        CT_RETURN_IF_FAILED(MFCreateMediaType(out.GetAddressOf()));
        CT_RETURN_IF_FAILED(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        CT_RETURN_IF_FAILED(out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AacBytesPerSecond(a.cfg.bitrate_bps)));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
        CT_RETURN_IF_FAILED(out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29));
        CT_RETURN_IF_FAILED(writer->AddStream(out.Get(), &a.index));

        ComPtr<IMFMediaType> in;
        CT_RETURN_IF_FAILED(MFCreateMediaType(in.GetAddressOf()));
        CT_RETURN_IF_FAILED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        CT_RETURN_IF_FAILED(in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * ch));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2 * ch * kAudioRate));
        CT_RETURN_IF_FAILED(in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        const HRESULT hr = writer->SetInputMediaType(a.index, in.Get(), nullptr);
        if (FAILED(hr)) CT_LOG_ERROR("Mp4Writer: AAC stream '%s': SetInputMediaType failed: %s", a.cfg.label, HrToString(hr).c_str());
        return hr;
    }

    // The values the video encoder actually runs with (what it accepted, or its defaults), for the log: rate control
    // and speed settings decide the video-engine load (P05 5.8).
    bool began = false;
    // IMFSinkWriter::BeginWriting, then the encoder name and its settings (they are only final once writing began).
    HRESULT BeginWriting() {
        const HRESULT hr = writer->BeginWriting();
        if (FAILED(hr)) {
            CT_LOG_ERROR("IMFSinkWriter::BeginWriting failed: %s", HrToString(hr).c_str());
            writer.Reset();
            return hr;
        }
        began = true;
        if (cfg.has_video && (encoder_name.empty() || encoder_name == "unknown encoder")) IdentifyEncoder(cfg.video.prefer_hardware);
        if (cfg.has_video) LogCodecApiValues();
        return S_OK;
    }

    void LogCodecApiValues() {
        ComPtr<ICodecAPI> api;
        if (FAILED(writer->GetServiceForStream(video_index, GUID_NULL, IID_PPV_ARGS(api.GetAddressOf()))) || !api) return;
        struct P { const GUID* id; const char* name; };
        const P params[] = {
            {&CODECAPI_AVEncCommonRateControlMode, "rc"}, {&CODECAPI_AVEncCommonQuality, "quality"},
            {&CODECAPI_AVEncCommonQualityVsSpeed, "qvs"}, {&CODECAPI_AVEncCommonLowLatency, "low_latency"},
            {&CODECAPI_AVEncMPVDefaultBPictureCount, "bframes"}, {&CODECAPI_AVEncMPVGOPSize, "gop"},
            {&CODECAPI_AVEncCommonMeanBitRate, "mean_bps"}, {&CODECAPI_AVEncCommonMaxBitRate, "max_bps"},
            {&CODECAPI_AVEncVideoMaxNumRefFrame, "refs"}, {&CODECAPI_AVEncVideoUsage, "usage"},
            {&CODECAPI_AVEncH264CABACEnable, "cabac"}, {&CODECAPI_AVEncVideoEncodeQP, "qp"},
        };
        char line[512];
        int at = std::snprintf(line, sizeof(line), "Mp4Writer: encoder settings:");
        for (const P& p : params) {
            VARIANT var;
            VariantInit(&var);
            const HRESULT hr = api->IsSupported(p.id) == S_OK ? api->GetValue(p.id, &var) : E_NOTIMPL;
            long long value = -1;
            if (SUCCEEDED(hr)) {
                if (var.vt == VT_UI4) value = var.ulVal;
                else if (var.vt == VT_BOOL) value = var.boolVal ? 1 : 0;
                else if (var.vt == VT_UI8) value = static_cast<long long>(var.ullVal);
                else if (var.vt == VT_I4) value = var.lVal;
            }
            VariantClear(&var);
            if (at > 0 && at < static_cast<int>(sizeof(line)))
                at += SUCCEEDED(hr) ? std::snprintf(line + at, sizeof(line) - at, " %s=%lld", p.name, value)
                                    : std::snprintf(line + at, sizeof(line) - at, " %s=n/a", p.name);
        }
        CT_LOG_INFO("%s", line);
    }

    void IdentifyEncoder(bool hardware_allowed) {
        encoder_name.clear();
        self->m_hardware = false;
        ComPtr<IMFTransform> mft;
        if (FAILED(writer->GetServiceForStream(video_index, GUID_NULL, IID_PPV_ARGS(mft.GetAddressOf()))) || !mft) {
            encoder_name = "unknown encoder";
            return;
        }
        ComPtr<IMFAttributes> a;
        if (FAILED(mft->GetAttributes(a.GetAddressOf())) || !a) {
            encoder_name = "unknown encoder";
            return;
        }
        wchar_t* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
            encoder_name = WideToUtf8(name);
            CoTaskMemFree(name);
        }
        GUID clsid{};
        if (encoder_name.empty() && SUCCEEDED(a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            wchar_t buf[64];
            if (StringFromGUID2(clsid, buf, 64) > 0) encoder_name = EncoderCaps::NameForClsid(WideToUtf8(buf));
        }
        // Hardware MFTs carry MFT_ENUM_HARDWARE_URL_Attribute ("AMDh264Encoder" on AMD), not the friendly name.
        if (encoder_name.empty() && SUCCEEDED(a->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &name, &len))) {
            encoder_name = WideToUtf8(name);
            CoTaskMemFree(name);
        }
        UINT32 url_len = 0, async = 0;
        const bool hw = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &url_len)) ||
                        (SUCCEEDED(a->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async);
        self->m_hardware = hw && hardware_allowed;
        // The Microsoft software encoder exposes neither name nor CLSID on its attribute store: name it after the
        // first enumerated encoder of the same kind.
        if (encoder_name.empty())
            for (const EncoderInfo& e : EncoderCaps::Enumerate(cfg.video.codec))
                if (e.hardware == self->m_hardware) {
                    encoder_name = e.name;
                    break;
                }
        if (encoder_name.empty()) encoder_name = "unknown encoder";
    }

    HRESULT ConfigureWriter(bool hardware) {
        CT_RETURN_IF_FAILED(CreateWriter(hardware));
        if (cfg.has_video) {
            const HRESULT hr = AddVideoStream();
            if (FAILED(hr)) return hr;
            IdentifyEncoder(hardware);
        }
        for (auto& a : audio) CT_RETURN_IF_FAILED(AddAudioStream(*a));
        return S_OK;
    }

    HRESULT SetupVideoBuffers(platform::TexturePool* ext) {
        const VideoEncodeConfig& v = cfg.video;
        if (ext) {
            pool = ext;
            external_pool = true;
        } else {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(v.size.w);
            d.Height = static_cast<UINT>(v.size.h);
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_NV12;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            CT_RETURN_IF_FAILED(internal_pool.Init(*dev, d, std::clamp<uint32_t>(cfg.sync_pool_size, 2, 64), "media.sync_nv12"));
            pool = &internal_pool;
        }
        const D3D11_TEXTURE2D_DESC& pd = pool->Desc();
        if (pd.Format != DXGI_FORMAT_NV12 || pd.Width != static_cast<UINT>(v.size.w) || pd.Height != static_cast<UINT>(v.size.h)) {
            CT_LOG_ERROR("Mp4Writer: NV12 pool is %ux%u format %d, the stream is %dx%d NV12", pd.Width, pd.Height,
                         static_cast<int>(pd.Format), v.size.w, v.size.h);
            return E_INVALIDARG;
        }
        vbufs.resize(pool->Count());
        for (uint32_t i = 0; i < pool->Count(); ++i) {
            CT_RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pool->At(i), 0, FALSE, vbufs[i].GetAddressOf()));
            ComPtr<IMF2DBuffer> b2d;
            CT_RETURN_IF_FAILED(vbufs[i].As(&b2d));
            DWORD len = 0;
            CT_RETURN_IF_FAILED(b2d->GetContiguousLength(&len));
            // Set once: the length never changes, and the buffer may be in flight inside MF later.
            CT_RETURN_IF_FAILED(vbufs[i]->SetCurrentLength(len));
            pool->SetUserData(i, vbufs[i].Get());
        }
        pool->SetReusePredicate(&CanReuse, this);
        return S_OK;
    }

    // ---- producer side ----------------------------------------------------------------------------------------

    bool IsActive(int64_t last_push_qpc, int64_t now_qpc) const {
        const int64_t second = Clock::QpcFrequency();
        if (last_push_qpc == 0) return now_qpc - start_qpc < second;
        return now_qpc - last_push_qpc < second;
    }

    bool WaitProgress(const Stopwatch& sw) {
        if (failed.load(std::memory_order_acquire) || !thread.IsRunning()) return false;
        if (sw.ElapsedMs() > kSyncWaitMs) return false;
        progress.Wait(5);
        return true;
    }

    bool PushVideo(const VideoItem& item, bool block) {
        Stopwatch sw;
        sw.Start();
        for (;;) {
            if (vqueue.Size() < vlimit && vqueue.Push(item)) break;
            if (!block || !WaitProgress(sw)) return false;
        }
        video_frontier_us.store(item.t_us + frame_dur_100ns / 10, std::memory_order_release);
        video_last_push_qpc.store(item.qpc, std::memory_order_release);
        stats.queue_depth.store(vqueue.Size(), std::memory_order_relaxed);
        wake.Set();
        return true;
    }

    // Queues the accumulated buffer. On failure (non-blocking, queue full) the buffer's content is discarded and the
    // buffer stays the accumulator, so only this thread ever acquires buffers (ObjectPool's SPSC contract).
    bool PushAccumulated(AudioStream& a, uint32_t stream, bool block) {
        const AudioItem item{static_cast<uint32_t>(a.acc_buf), a.acc_frames, a.acc_t};
        Stopwatch sw;
        sw.Start();
        for (;;) {
            if (a.queue.Size() < a.limit && a.queue.Push(item)) break;
            if (!block || !WaitProgress(sw)) {
                stats.audio_rejected_frames[stream].fetch_add(a.acc_frames, std::memory_order_relaxed);
                a.acc_frames = 0;
                return false;
            }
        }
        a.frontier_us.store(item.t_us + FramesToUs(item.frames), std::memory_order_release);
        a.last_push_qpc.store(Clock::QpcNow(), std::memory_order_release);
        a.acc_buf = -1;
        a.acc_frames = 0;
        wake.Set();
        return true;
    }

    bool PushAudio(uint32_t stream, const float* data, uint32_t frames, TimeUs t_us, bool block) {
        AudioStream& a = *audio[stream];
        const uint32_t ch = static_cast<uint32_t>(a.cfg.channels);
        if (a.acc_buf >= 0 && a.acc_frames > 0) {
            const TimeUs expected = a.acc_t + FramesToUs(a.acc_frames);
            const TimeUs d = t_us - expected;
            if (d > 25 || d < -25) PushAccumulated(a, stream, block);   // not contiguous: close the batch
        }
        uint32_t done = 0;
        while (done < frames) {
            if (a.acc_buf < 0) {
                Stopwatch sw;
                sw.Start();
                uint32_t idx = 0;
                while (!a.pool.Acquire(&idx)) {
                    if (!block || !WaitProgress(sw)) {
                        stats.audio_rejected_frames[stream].fetch_add(frames - done, std::memory_order_relaxed);
                        return false;
                    }
                }
                a.acc_buf = idx;
                a.acc_frames = 0;
            }
            if (a.acc_frames == 0) a.acc_t = t_us + FramesToUs(done);
            const uint32_t n = std::min(frames - done, kAudioItemFrames - a.acc_frames);
            int16_t* dst = a.pool.At(static_cast<uint32_t>(a.acc_buf)) + static_cast<size_t>(a.acc_frames) * ch;
            if (data) audio::dsp::FloatToPcm16(data + static_cast<size_t>(done) * ch, dst, static_cast<size_t>(n) * ch);
            else std::memset(dst, 0, static_cast<size_t>(n) * ch * sizeof(int16_t));
            a.acc_frames += n;
            done += n;
            if (a.acc_frames == kAudioItemFrames && !PushAccumulated(a, stream, block)) {
                if (done < frames) stats.audio_rejected_frames[stream].fetch_add(frames - done, std::memory_order_relaxed);
                return false;
            }
        }
        return true;
    }

    // Stop accepting and wait until no producer call is inside the writer.
    void StopAccepting() {
        accepting.store(false, std::memory_order_seq_cst);
        for (int spins = 0;; ++spins) {
            bool busy = video_in_call.load(std::memory_order_seq_cst) != 0;
            for (auto& a : audio) busy = busy || a->in_call.load(std::memory_order_seq_cst) != 0;
            if (!busy || spins > 20000) break;
            if (spins < 100) YieldProcessor();
            else Sleep(0);
        }
    }

    // ---- encode thread ----------------------------------------------------------------------------------------

    HRESULT WriteSample(DWORD index, IMFSample* s) {
        if (samples_written == inject_after) {
            ++samples_written;
            return inject_hr;
        }
        ++samples_written;
        return writer->WriteSample(index, s);
    }

    HRESULT WriteVideo(const VideoItem& it) {
        ComPtr<IMFSample> s;
        HRESULT hr = MFCreateSample(s.GetAddressOf());
        if (SUCCEEDED(hr)) hr = s->AddBuffer(vbufs[it.index].Get());
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(it.t_us * 10);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(frame_dur_100ns);
        if (SUCCEEDED(hr)) {
            const int64_t t0 = Clock::QpcNow();
            stats.video_latency.AddUs(Clock::QpcToUs(t0, it.qpc));
            hr = WriteSample(video_index, s.Get());
            stats.video_write.AddUs(Clock::QpcToUs(Clock::QpcNow(), t0));
        }
        s.Reset();                 // drop our reference before the slot can be reused
        pool->Release(it.index);   // the reuse predicate still blocks the texture until MF released it
        uint32_t held = 0;
        for (const auto& b : vbufs) {
            b->AddRef();
            held += b->Release() > 1 ? 1u : 0u;
        }
        if (held > stats.max_textures_in_mf.load(std::memory_order_relaxed))
            stats.max_textures_in_mf.store(held, std::memory_order_relaxed);
        if (SUCCEEDED(hr)) {
            stats.video_written.fetch_add(1, std::memory_order_relaxed);
            stats.last_video_us.store(it.t_us, std::memory_order_relaxed);
        }
        return hr;
    }

    HRESULT WritePcm(AudioStream& a, const int16_t* pcm, uint32_t frames, TimeUs t_us) {
        const DWORD bytes = frames * static_cast<DWORD>(a.cfg.channels) * 2;
        ComPtr<IMFMediaBuffer> mb;
        CT_RETURN_IF_FAILED(MFCreateMemoryBuffer(bytes, mb.GetAddressOf()));
        BYTE* p = nullptr;
        CT_RETURN_IF_FAILED(mb->Lock(&p, nullptr, nullptr));
        std::memcpy(p, pcm, bytes);
        mb->Unlock();
        CT_RETURN_IF_FAILED(mb->SetCurrentLength(bytes));
        ComPtr<IMFSample> s;
        CT_RETURN_IF_FAILED(MFCreateSample(s.GetAddressOf()));
        CT_RETURN_IF_FAILED(s->AddBuffer(mb.Get()));
        CT_RETURN_IF_FAILED(s->SetSampleTime(t_us * 10));
        CT_RETURN_IF_FAILED(s->SetSampleDuration(FramesTo100ns(frames)));
        return WriteSample(a.index, s.Get());
    }

    HRESULT WriteSilence(AudioStream& a, uint32_t stream, TimeUs from_us, TimeUs to_us) {
        int64_t frames = UsToFrames(to_us - from_us);
        int64_t written = 0;
        while (written < frames) {
            const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(frames - written, kAudioItemFrames));
            CT_RETURN_IF_FAILED(WritePcm(a, silence.data(), n, from_us + FramesToUs(written)));
            written += n;
        }
        if (!a.started) stats.audio_lead_pad_frames[stream].fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
        a.next_us = from_us + FramesToUs(frames);
        return S_OK;
    }

    HRESULT WriteAudio(uint32_t stream, const AudioItem& it) {
        AudioStream& a = *audio[stream];
        HRESULT hr = S_OK;
        // Timestamps never go backwards: the part of an item that starts before the data written so far (an overlapping
        // or late producer, or a negative start) is trimmed; an item entirely before it is dropped.
        const int16_t* pcm = a.pool.At(it.buf);
        uint32_t frames = it.frames;
        TimeUs t_us = it.t_us;
        if (t_us < a.next_us) {
            const int64_t drop = std::min<int64_t>(UsToFrames(a.next_us - t_us), frames);
            if (drop > 0) {
                stats.audio_overlap_frames[stream].fetch_add(static_cast<uint64_t>(drop), std::memory_order_relaxed);
                pcm += static_cast<size_t>(drop) * static_cast<size_t>(a.cfg.channels);
                frames -= static_cast<uint32_t>(drop);
            }
            t_us = a.next_us;   // sub-frame overlaps (rounding) just snap to the end of the written data
        }
        if (frames == 0) {
            a.pool.Release(it.buf);
            return S_OK;
        }
        // Streams begin at t = 0; gaps (frames the producer could not queue) become silence so nothing shifts.
        if (!a.started && t_us > 0) hr = WriteSilence(a, stream, 0, t_us);
        else if (a.started && t_us - a.next_us > kGapFillUs) hr = WriteSilence(a, stream, a.next_us, t_us);
        a.started = true;
        if (SUCCEEDED(hr)) hr = WritePcm(a, pcm, frames, t_us);
        a.pool.Release(it.buf);
        if (SUCCEEDED(hr)) {
            a.next_us = t_us + FramesToUs(frames);
            stats.audio_written[stream].fetch_add(frames, std::memory_order_relaxed);
            stats.last_audio_us[stream].store(t_us, std::memory_order_relaxed);
        }
        return hr;
    }

    void Stage() {
        if (cfg.has_video && !vstaged && vqueue.Pop(vhead)) vstaged = true;
        for (auto& a : audio)
            if (!a->staged && a->queue.Pop(a->head)) a->staged = true;
    }

    void DropQueued() {
        if (cfg.has_video) {
            if (vstaged) pool->Release(vhead.index);
            vstaged = false;
            VideoItem v;
            while (vqueue.Pop(v)) pool->Release(v.index);
        }
        for (auto& a : audio) {
            if (a->staged) a->pool.Release(a->head.buf);
            a->staged = false;
            AudioItem it;
            while (a->queue.Pop(it)) a->pool.Release(it.buf);
        }
    }

    void FinalizeSink() {
        if (finalized || !writer) return;
        finalized = true;
        // A stream that never got a sample makes IMFSinkWriter::Finalize fail: give silent audio streams silence up
        // to the end of the video.
        if (!failed.load(std::memory_order_acquire)) {
            TimeUs end = 100'000;
            if (cfg.has_video && stats.video_written.load() > 0)
                end = std::max(end, stats.last_video_us.load() + frame_dur_100ns / 10);
            for (size_t i = 0; i < audio.size(); ++i)
                if (!audio[i]->started) {
                    CT_LOG_WARN("Mp4Writer: audio stream '%s' received no data; writing silence", audio[i]->cfg.label);
                    CT_LOG_IF_FAILED(WriteSilence(*audio[i], static_cast<uint32_t>(i), 0, end));
                    audio[i]->started = true;
                }
        }
        Stopwatch sw;
        sw.Start();
        if (finalize_delay_ms) Sleep(finalize_delay_ms);
        finalize_hr = writer->Finalize();
        stats.finalize_ms.store(static_cast<int64_t>(sw.ElapsedMs()), std::memory_order_relaxed);
        if (FAILED(finalize_hr)) CT_LOG_ERROR("IMFSinkWriter::Finalize(%s) failed: %s", cfg.path.c_str(), HrToString(finalize_hr).c_str());
        else CT_LOG_INFO("Mp4Writer: finalized %s in %.0f ms (%llu video frames)", cfg.path.c_str(), sw.ElapsedMs(),
                         static_cast<unsigned long long>(stats.video_written.load()));
    }

    void Fail(HRESULT hr) {
        HRESULT expected = S_OK;
        last_error.compare_exchange_strong(expected, hr, std::memory_order_acq_rel);
        failed.store(true, std::memory_order_release);
        CT_LOG_ERROR("Mp4Writer: writing %s failed: %s", cfg.path.c_str(), HrToString(hr).c_str());
        StopAccepting();
        DropQueued();
        FinalizeSink();   // a file with the data so far is usually playable
        progress.Set();
        if (cfg.listener && !abort.load(std::memory_order_acquire)) cfg.listener->OnWriterError(hr);   // owner may be gone after Abort
    }

    void Run() {
        std::vector<MuxStreamState> st(1 + audio.size());
        const int base = cfg.has_video ? 0 : 1;   // st[0] is the video stream (unused without video)
        const int n = static_cast<int>(st.size()) - base;
        for (;;) {
            if (abort.load(std::memory_order_acquire)) break;
            Stage();
            const bool stop = stopping.load(std::memory_order_acquire);
            const int64_t now = Clock::QpcNow();
            if (cfg.has_video) {
                MuxStreamState& v = st[0];
                v.has_head = vstaged;
                v.head_us = vhead.t_us;
                v.frontier_us = video_frontier_us.load(std::memory_order_acquire);
                v.active = !stop && IsActive(video_last_push_qpc.load(std::memory_order_acquire), now);
                v.pressure = (vqueue.Size() + 1) * 4 >= vlimit * 3;
            }
            for (size_t i = 0; i < audio.size(); ++i) {
                AudioStream& a = *audio[i];
                MuxStreamState& s = st[1 + i];
                s.has_head = a.staged;
                s.head_us = a.head.t_us;
                s.frontier_us = a.frontier_us.load(std::memory_order_acquire);
                s.active = !stop && IsActive(a.last_push_qpc.load(std::memory_order_acquire), now);
                s.pressure = (a.queue.Size() + 1) * 4 >= a.limit * 3;
            }
            const int pick = ChooseNextStream(st.data() + base, n);
            if (pick >= 0) {
                const int k = pick + base;
                HRESULT hr;
                if (k == 0) {
                    vstaged = false;
                    hr = WriteVideo(vhead);
                } else {
                    audio[static_cast<size_t>(k - 1)]->staged = false;
                    hr = WriteAudio(static_cast<uint32_t>(k - 1), audio[static_cast<size_t>(k - 1)]->head);
                }
                progress.Set();
                if (FAILED(hr)) {
                    Fail(hr);
                    return;
                }
                continue;
            }
            bool any = vstaged || !vqueue.Empty();
            for (auto& a : audio) any = any || a->staged || !a->queue.Empty();
            if (stop && !any) break;
            wake.Wait(any ? 5 : 100);
        }
        if (abort.load(std::memory_order_acquire)) DropQueued();
        FinalizeSink();
        if (FAILED(finalize_hr)) {
            HRESULT expected = S_OK;
            last_error.compare_exchange_strong(expected, finalize_hr, std::memory_order_acq_rel);
        }
        progress.Set();
    }
};

// ---- Mp4Writer -----------------------------------------------------------------------------------------------------

Mp4Writer::Mp4Writer() : m_impl(std::make_unique<Impl>()) { m_impl->self = this; }

Mp4Writer::~Mp4Writer() {
    Impl& m = *m_impl;
    if (m.started && m.thread.Handle()) {
        if (!m.abort.load()) Finalize();
        if (m.thread.IsRunning()) {
            if (m.abort.load()) {
                // Crash path: Abort returned while ct-encode is still inside IMFSinkWriter::Finalize (or about to call
                // it). Freeing the sink writer, buffers or events under it would crash; joining could hang the dying
                // process. Leak the Impl on purpose: the thread only touches Impl state (stats, events, writer) and
                // the NV12 pool, which must outlive the writer anyway.
                CT_LOG_WARN("Mp4Writer: ct-encode still finalizing %s after Abort; leaking the writer state", m.cfg.path.c_str());
                (void)m_impl.release();
                return;
            }
            // Finalize's 60 s join timed out: wait for the thread before releasing anything it uses.
            CT_LOG_WARN("Mp4Writer: waiting for ct-encode to finish %s", m.cfg.path.c_str());
            m.thread.Join();
        }
    }
    if (m.pool && m.external_pool) {
        m.pool->SetReusePredicate(nullptr, nullptr);
        for (uint32_t i = 0; i < m.pool->Count(); ++i) m.pool->SetUserData(i, nullptr);
    }
    m.vbufs.clear();
    m.writer.Reset();
    m.internal_pool.Shutdown();
    if (m.mf_ok) MfRuntime::Shutdown();
}

HRESULT Mp4Writer::Init(platform::D3DDevice& dev, const Config& cfg, platform::TexturePool* nv12_pool) {
    Impl& m = *m_impl;
    auto fail = [&](HRESULT hr) {
        m_impl->last_error.store(hr, std::memory_order_release);
        return hr;
    };
    if (m.init_ok || m.started) return fail(E_UNEXPECTED);
    m.dev = &dev;
    m.cfg = cfg;
    const VideoEncodeConfig& v = cfg.video;
    if (cfg.path.empty() || (!cfg.has_video && cfg.audio.empty()) || cfg.audio.size() > kMaxAudioStreams) return fail(E_INVALIDARG);
    if (cfg.has_video && (v.size.IsEmpty() || (v.size.w & 1) || (v.size.h & 1) || v.fps_num <= 0 || v.fps_den <= 0)) {
        CT_LOG_ERROR("Mp4Writer: invalid video %dx%d @ %d/%d (sizes must be even)", v.size.w, v.size.h, v.fps_num, v.fps_den);
        return fail(E_INVALIDARG);
    }
    for (const AudioEncodeConfig& a : cfg.audio)
        if (a.sample_rate != kAudioRate || a.channels < 1 || a.channels > 2) return fail(E_INVALIDARG);
    if (!dev.Device()) return fail(E_INVALIDARG);

    HRESULT hr = MfRuntime::Ensure();
    if (FAILED(hr)) return fail(hr);
    m.mf_ok = true;
    audio::dsp::Init();
    if (cfg.has_video) {
        hr = dev.InitMfManager();
        if (FAILED(hr)) return fail(hr);
        m.vlimit = std::clamp<uint32_t>(cfg.video_queue, 2, kQueueCapacity);
        m.frame_dur_100ns = 10'000'000ll * v.fps_den / v.fps_num;
    }
    for (const AudioEncodeConfig& ac : cfg.audio) {
        auto a = std::make_unique<Impl::AudioStream>();
        a->cfg = ac;
        if (!a->cfg.label) a->cfg.label = "audio";
        a->limit = std::clamp<uint32_t>(cfg.audio_queue, 2, kQueueCapacity);
        const uint32_t bufs = a->limit + 3;   // queue + the staged head + the accumulator + slack
        const size_t per = static_cast<size_t>(kAudioItemFrames) * static_cast<size_t>(ac.channels);
        a->storage = std::make_unique<int16_t[]>(per * bufs);
        int16_t* base = a->storage.get();
        a->pool.Init(bufs, [&](size_t i) { return base + i * per; });
        m.audio.push_back(std::move(a));
    }
    m.silence.assign(static_cast<size_t>(kAudioItemFrames) * 2, 0);

    hr = m.ConfigureWriter(v.prefer_hardware);
    if (FAILED(hr) && cfg.has_video && v.prefer_hardware) {
        const HRESULT hw_hr = hr;
        hr = m.ConfigureWriter(false);
        if (SUCCEEDED(hr)) CT_LOG_WARN("Mp4Writer: hardware encoder setup failed (%s); using the software encoder", HrToString(hw_hr).c_str());
    }
    if (FAILED(hr)) {
        m.writer.Reset();
        return fail(hr);
    }
    if (cfg.has_video) {
        hr = m.SetupVideoBuffers(nv12_pool);
        if (FAILED(hr)) {
            m.writer.Reset();
            return fail(hr);
        }
    }
    if (!cfg.defer_begin_writing) {
        hr = m.BeginWriting();
        if (FAILED(hr)) return fail(hr);
    }
    m.init_ok = true;
    if (cfg.has_video)
        CT_LOG_INFO("Mp4Writer: %s %dx%d @ %d/%d %s, %s %u kbps (max %u), encoder %s (%s), %zu audio stream(s)", cfg.path.c_str(),
                    v.size.w, v.size.h, v.fps_num, v.fps_den, CodecName(v.codec),
                    v.rc == RateControl::Quality ? "quality" : v.rc == RateControl::Vbr ? "vbr" : "cbr", EffectiveBitrate(v) / 1000,
                    EffectiveMaxBitrate(v) / 1000, m.encoder_name.empty() ? "?" : m.encoder_name.c_str(),
                    m_hardware ? "hardware" : "software", cfg.audio.size());
    else
        CT_LOG_INFO("Mp4Writer: %s audio only, %zu stream(s)", cfg.path.c_str(), cfg.audio.size());
    return S_OK;
}

bool Mp4Writer::Start() {
    Impl& m = *m_impl;
    if (!m.init_ok || m.started) return false;
    if (m.cfg.defer_begin_writing && !m.began) {
        const int64_t t0 = Clock::QpcNow();
        const HRESULT hr = m.BeginWriting();
        if (FAILED(hr)) {
            m_impl->last_error.store(hr, std::memory_order_release);
            return false;
        }
        CT_LOG_INFO("Mp4Writer: deferred BeginWriting took %.1f ms", static_cast<double>(Clock::QpcNow() - t0) * 1000.0 / static_cast<double>(Clock::QpcFrequency()));
    }
    m.start_qpc = Clock::QpcNow();
    m_impl->accepting.store(true, std::memory_order_seq_cst);
    Thread::Options opt;
    opt.name = "ct-encode";
    opt.priority = THREAD_PRIORITY_ABOVE_NORMAL;
    opt.mmcss_task = L"Playback";
    if (!m.thread.Start(opt, [&m] { m.Run(); })) {
        m_impl->accepting.store(false);
        return false;
    }
    m.started = true;
    return true;
}

bool Mp4Writer::OnFrame(ID3D11Texture2D* nv12, uint32_t pool_index, TimeUs t_us) {
    (void)nv12;
    Impl& m = *m_impl;
    if (!m.cfg.has_video || !m.external_pool || pool_index >= m.vbufs.size()) {
        m_impl->stats.video_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m.video_in_call.fetch_add(1, std::memory_order_seq_cst);
    bool ok = m_impl->accepting.load(std::memory_order_seq_cst);
    if (ok) ok = m.PushVideo(VideoItem{pool_index, t_us, Clock::QpcNow()}, false);
    m.video_in_call.fetch_sub(1, std::memory_order_seq_cst);
    if (!ok) m_impl->stats.video_rejected.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

HRESULT Mp4Writer::WriteVideoSync(ID3D11Texture2D* nv12, TimeUs t_us) {
    Impl& m = *m_impl;
    if (!m.cfg.has_video || m.external_pool || !m.pool || !nv12) return E_INVALIDARG;
    if (!m_impl->accepting.load(std::memory_order_acquire)) return FAILED(LastError()) ? LastError() : MF_E_SHUTDOWN;
    D3D11_TEXTURE2D_DESC d{};
    nv12->GetDesc(&d);
    const D3D11_TEXTURE2D_DESC& pd = m.pool->Desc();
    if (d.Format != DXGI_FORMAT_NV12 || d.Width != pd.Width || d.Height != pd.Height) return E_INVALIDARG;
    Stopwatch sw;
    sw.Start();
    uint32_t idx = 0;
    ID3D11Texture2D* dst = nullptr;
    while (!(dst = m.pool->Acquire(idx))) {
        if (!m.WaitProgress(sw)) return FAILED(LastError()) ? LastError() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    {
        platform::D3DDevice::Lock lock(*m.dev);
        m.dev->Context()->CopyResource(dst, nv12);
    }
    if (!m.PushVideo(VideoItem{idx, t_us, Clock::QpcNow()}, true)) {
        m.pool->Release(idx);
        m_impl->stats.video_rejected.fetch_add(1, std::memory_order_relaxed);
        return FAILED(LastError()) ? LastError() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return S_OK;
}

bool Mp4Writer::OnAudio(uint32_t stream, const audio::AudioPacket& p) {
    Impl& m = *m_impl;
    if (stream >= m.audio.size()) return false;
    Impl::AudioStream& a = *m.audio[stream];
    if (p.channels != a.cfg.channels) {
        m_impl->stats.audio_rejected_frames[stream].fetch_add(p.frames, std::memory_order_relaxed);
        return false;
    }
    a.in_call.fetch_add(1, std::memory_order_seq_cst);
    bool ok = m_impl->accepting.load(std::memory_order_seq_cst);
    if (ok) ok = m.PushAudio(stream, p.silence_fill ? nullptr : p.data, p.frames, p.t_us, false);
    else m_impl->stats.audio_rejected_frames[stream].fetch_add(p.frames, std::memory_order_relaxed);
    a.in_call.fetch_sub(1, std::memory_order_seq_cst);
    return ok;
}

HRESULT Mp4Writer::WriteAudioSync(uint32_t stream, const float* interleaved, uint32_t frames, TimeUs t_us) {
    Impl& m = *m_impl;
    if (stream >= m.audio.size() || (!interleaved && frames)) return E_INVALIDARG;
    if (!m_impl->accepting.load(std::memory_order_acquire)) return FAILED(LastError()) ? LastError() : MF_E_SHUTDOWN;
    if (!m.PushAudio(stream, interleaved, frames, t_us, true)) return FAILED(LastError()) ? LastError() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return S_OK;
}

HRESULT Mp4Writer::Finalize() {
    Impl& m = *m_impl;
    if (!m.started) {
        if (m.init_ok && m.writer && !m.finalized) {
            // Never started: nothing was written; release the file.
            m.writer.Reset();
            m.finalized = true;
        }
        return FAILED(LastError()) ? LastError() : (m.init_ok ? MF_E_NOT_INITIALIZED : E_UNEXPECTED);
    }
    if (m.thread.Handle()) {
        m.StopAccepting();
        // The producers are out: flush their partial batches (this thread acts as the producer now).
        if (!m.failed.load(std::memory_order_acquire))
            for (size_t i = 0; i < m.audio.size(); ++i)
                if (m.audio[i]->acc_buf >= 0 && m.audio[i]->acc_frames > 0) m.PushAccumulated(*m.audio[i], static_cast<uint32_t>(i), true);
        m.stopping.store(true, std::memory_order_release);
        m.wake.Set();
        if (!m.thread.Join(60'000)) {
            CT_LOG_ERROR("Mp4Writer: ct-encode did not finish within 60 s (finalizing %s)", m.cfg.path.c_str());
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        m.writer.Reset();
    }
    return LastError();
}

void Mp4Writer::Abort() {
    Impl& m = *m_impl;
    m_impl->accepting.store(false, std::memory_order_seq_cst);
    if (!m.started || !m.thread.Handle()) return;
    m.abort.store(true, std::memory_order_release);
    m.wake.Set();
    WaitForSingleObject(m.thread.Handle(), 3000);
}

std::string Mp4Writer::ErrorMessage() const {
    const HRESULT hr = LastError();
    const std::string& path = m_impl->cfg.path;
    if (SUCCEEDED(hr)) return std::string();
    if (IsDiskFull(hr)) return Format("The disk is full. The recording was saved up to this point: %s", path.c_str());
    if (hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return Format("The folder does not exist: %s", std::string(PathParent(path)).c_str());
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return Format("Access denied writing %s", path.c_str());
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return Format("The graphics device was reset. The recording was saved up to this point: %s", path.c_str());
    return Format("Writing %s failed: %s", path.c_str(), HrToString(hr).c_str());
}

std::string Mp4Writer::EncoderName() const { return m_impl->encoder_name; }

uint32_t Mp4Writer::VideoStreamIndex() const { return m_impl->cfg.has_video ? m_impl->video_index : UINT32_MAX; }

uint32_t Mp4Writer::AudioStreamIndex(uint32_t stream) const {
    return stream < m_impl->audio.size() ? m_impl->audio[stream]->index : UINT32_MAX;
}

HRESULT Mp4Writer::LastError() const { return m_impl->last_error.load(std::memory_order_acquire); }

const Mp4Writer::Stats& Mp4Writer::GetStats() const { return m_impl->stats; }

bool Mp4Writer::Accepting() const { return m_impl->accepting.load(std::memory_order_acquire); }

void Mp4Writer::SetFinalizeDelayForTest(uint32_t ms) { m_impl->finalize_delay_ms = ms; }

void Mp4Writer::InjectWriteErrorForTest(uint64_t after_samples, HRESULT hr) {
    m_impl->inject_after = after_samples;
    m_impl->inject_hr = hr;
}

} // namespace ct::media
