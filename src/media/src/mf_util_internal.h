#pragma once
// mf_util_internal.h (private to ct_media)
// Small Media Foundation helpers shared by media_info / video_decoder / audio_decoder.


#include <ct/core/types.h>
#include <ct/platform/mf_runtime.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <string>

namespace ct::media::detail {

// The reader's special stream indices are negative enum values; DWORD versions keep /W4 quiet.
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kFirstAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

inline std::string SubtypeName(const GUID& g) {
    struct Name { const GUID* guid; const char* name; };
    static const Name kNames[] = {
        {&MFVideoFormat_H264, "H264"},   {&MFVideoFormat_HEVC, "HEVC"},   {&MFVideoFormat_H265, "HEVC"},
        {&MFVideoFormat_MPEG2, "MPEG2"}, {&MFVideoFormat_VP90, "VP9"},    {&MFVideoFormat_AV1, "AV1"},
        {&MFVideoFormat_MJPG, "MJPG"},   {&MFVideoFormat_NV12, "NV12"},   {&MFVideoFormat_YUY2, "YUY2"},
        {&MFVideoFormat_RGB32, "RGB32"}, {&MFVideoFormat_ARGB32, "ARGB32"}, {&MFVideoFormat_MP4V, "MP4V"},
        {&MFVideoFormat_WMV3, "WMV3"},   {&MFVideoFormat_WVC1, "WVC1"},
        {&MFAudioFormat_AAC, "AAC"},     {&MFAudioFormat_MP3, "MP3"},     {&MFAudioFormat_PCM, "PCM"},
        {&MFAudioFormat_Float, "FLOAT"}, {&MFAudioFormat_FLAC, "FLAC"},   {&MFAudioFormat_ALAC, "ALAC"},
        {&MFAudioFormat_Opus, "OPUS"},   {&MFAudioFormat_Dolby_AC3, "AC3"}, {&MFAudioFormat_Dolby_DDPlus, "EAC3"},
        {&MFAudioFormat_WMAudioV8, "WMA"}, {&MFAudioFormat_WMAudioV9, "WMA"}, {&MFAudioFormat_MPEG, "MPEG"},
    };
    for (const Name& n : kNames)
        if (*n.guid == g) return n.name;
    // FOURCC-style subtypes keep the code in Data1.
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((g.Data1 >> (8 * i)) & 0xFF);
        if (c < 32 || c > 126) return "UNKNOWN";
        s.push_back(c);
    }
    return s;
}

struct VideoTypeInfo {
    SizeI frame{};          // MF_MT_FRAME_SIZE (may be padded, e.g. 1920x1088)
    RectI aperture{};       // MF_MT_MINIMUM_DISPLAY_APERTURE (else the full frame)
    int fps_num = 0, fps_den = 1;
    int rotation_deg = 0;
    LONG stride = 0;        // MF_MT_DEFAULT_STRIDE (0 if absent)
};

inline VideoTypeInfo ReadVideoType(IMFMediaType* type) {
    VideoTypeInfo v;
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))) v.frame = {static_cast<int32_t>(w), static_cast<int32_t>(h)};
    v.aperture = {0, 0, v.frame.w, v.frame.h};
    MFVideoArea area{};
    UINT32 got = 0;
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &got)) &&
        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        v.aperture = {area.OffsetX.value, area.OffsetY.value, area.Area.cx, area.Area.cy};
    }
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
        v.fps_num = static_cast<int>(num);
        v.fps_den = static_cast<int>(den);
    }
    v.rotation_deg = static_cast<int>(MFGetAttributeUINT32(type, MF_MT_VIDEO_ROTATION, 0)) % 360;
    v.stride = static_cast<LONG>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, 0));
    return v;
}

inline TimeUs ReaderDurationUs(IMFSourceReader* reader) {
    PROPVARIANT var;
    PropVariantInit(&var);
    TimeUs d = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &var))) {
        ULONGLONG hns = 0;
        if (SUCCEEDED(PropVariantToUInt64(var, &hns))) d = static_cast<TimeUs>(hns / 10);
    }
    PropVariantClear(&var);
    return d;
}

inline HRESULT SetReaderPosition(IMFSourceReader* reader, TimeUs t_us) {
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (t_us < 0 ? 0 : t_us) * 10;
    const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    return hr;
}

} // namespace ct::media::detail
