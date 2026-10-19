#define CT_LOG_CATEGORY "media"
#include <ct/media/probe.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/media/mf_runtime.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {

std::string SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_H264) return "h264";
    if (g == MFVideoFormat_HEVC) return "hevc";
    if (g == MFAudioFormat_AAC) return "aac";
    if (g == MFAudioFormat_PCM) return "pcm";
    if (g == MFAudioFormat_Float) return "float";
    if (g == MFAudioFormat_MP3) return "mp3";
    if (g == MFVideoFormat_NV12) return "nv12";
    wchar_t buf[64];
    return StringFromGUID2(g, buf, 64) > 0 ? WideToUtf8(buf) : std::string("?");
}

HRESULT OpenReader(std::string_view path, ComPtr<IMFSourceReader>& reader) {
    ComPtr<IMFAttributes> attr;
    CT_RETURN_IF_FAILED(MFCreateAttributes(attr.GetAddressOf(), 1));
    const std::wstring w = Utf8ToWide(path);
    return MFCreateSourceReaderFromURL(w.c_str(), attr.Get(), reader.GetAddressOf());
}

// Source-reader stream indices in file (track) order. The MF MPEG-4 source presents the tracks of a file in reverse
// order: for super4vid's moov (track 1 video, 2 mic, 3 system) the reader's streams are 0 = system, 1 = mic, 2 = video
// (observed on 10.0.26100; verified by the mp4_writer tests with tracks of different bitrates), and its stream
// identifiers are its own 1..n, not the track_IDs. P06's MediaInfo replaces this.
std::vector<DWORD> FileOrder(IMFSourceReader* r) {
    std::vector<DWORD> order;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(r->GetNativeMediaType(i, 0, t.GetAddressOf()))) break;
        order.insert(order.begin(), i);
    }
    return order;
}

} // namespace

HRESULT Probe(std::string_view path, ProbeResult& r) {
    r = ProbeResult{};
    MfScope mf;
    CT_RETURN_IF_FAILED(mf.Result());
    ComPtr<IMFSourceReader> reader;
    const HRESULT hr_open = OpenReader(path, reader);
    if (FAILED(hr_open)) return hr_open;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8)
        r.duration_us = static_cast<TimeUs>(var.uhVal.QuadPart / 10);
    PropVariantClear(&var);
    for (DWORD i : FileOrder(reader.Get())) {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(i, 0, t.GetAddressOf());
        if (FAILED(hr)) return hr;
        GUID major{}, sub{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        r.stream_codecs.push_back(SubtypeName(sub));
        if (major == MFMediaType_Video) {
            if (r.video_streams++ == 0) {
                UINT32 w = 0, h = 0, num = 0, den = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
                r.video_width = static_cast<int>(w);
                r.video_height = static_cast<int>(h);
                r.fps_num = static_cast<int>(num);
                r.fps_den = static_cast<int>(den);
                r.video_codec = SubtypeName(sub);
            }
        } else if (major == MFMediaType_Audio) {
            if (r.audio_streams++ == 0) {
                r.audio_sample_rate = static_cast<int>(MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
                r.audio_channels = static_cast<int>(MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
            }
        }
    }
    return S_OK;
}

HRESULT ScanStream(std::string_view path, uint32_t stream_index, StreamScan& s, bool keep_times) {
    s = StreamScan{};
    MfScope mf;
    CT_RETURN_IF_FAILED(mf.Result());
    ComPtr<IMFSourceReader> reader;
    const HRESULT hr_open = OpenReader(path, reader);
    if (FAILED(hr_open)) return hr_open;
    CT_RETURN_IF_FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
    const std::vector<DWORD> order = FileOrder(reader.Get());
    if (stream_index >= order.size()) return MF_E_INVALIDSTREAMNUMBER;
    const DWORD reader_index = order[stream_index];
    CT_RETURN_IF_FAILED(reader->SetStreamSelection(reader_index, TRUE));
    TimeUs prev = -1;
    for (;;) {
        DWORD actual = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        CT_RETURN_IF_FAILED(reader->ReadSample(reader_index, 0, &actual, &flags, &ts, sample.GetAddressOf()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        const TimeUs t = ts / 10;
        LONGLONG dur = 0;
        sample->GetSampleDuration(&dur);
        DWORD len = 0;
        sample->GetTotalLength(&len);
        if (MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0)) ++s.keyframes;
        ++s.samples;
        s.bytes += len;
        if (s.first_us < 0) s.first_us = t;
        s.last_us = t;
        s.end_us = std::max<TimeUs>(s.end_us, (ts + dur) / 10);
        if (prev >= 0 && t - prev > s.max_gap_us) s.max_gap_us = t - prev;
        prev = t;
        if (keep_times) s.times.push_back(t);
    }
    return S_OK;
}

} // namespace ct::media
