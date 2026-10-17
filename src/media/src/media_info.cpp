#define CT_LOG_CATEGORY "media"
#include <ct/media/media_info.h>

#include "mf_util_internal.h"

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {

HRESULT LoadImageInfo(std::string_view path, MediaInfo& out) {
    ComPtr<IWICImagingFactory> factory;
    CT_RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring wpath = Utf8ToWide(path);
    HRESULT hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    decoder.GetAddressOf());
    if (FAILED(hr)) return hr;   // missing file / unknown format: the caller decides how loud to be
    ComPtr<IWICBitmapFrameDecode> frame;
    CT_RETURN_IF_FAILED(decoder->GetFrame(0, frame.GetAddressOf()));
    UINT w = 0, h = 0;
    CT_RETURN_IF_FAILED(frame->GetSize(&w, &h));
    GUID container{};
    decoder->GetContainerFormat(&container);
    StreamInfo s;
    s.kind = StreamInfo::Kind::Video;
    s.index = 0;
    s.codec = container == GUID_ContainerFormatPng    ? "PNG"
              : container == GUID_ContainerFormatJpeg ? "JPEG"
              : container == GUID_ContainerFormatBmp  ? "BMP"
              : container == GUID_ContainerFormatWebp ? "WEBP"
                                                      : "IMAGE";
    s.width = static_cast<int>(w);
    s.height = static_cast<int>(h);
    out.streams.push_back(s);
    out.is_image = true;
    out.duration_us = 0;
    out.seekable = false;
    return S_OK;
}

} // namespace

bool MediaInfo::IsImagePath(std::string_view path) {
    const std::string ext = ToLowerAscii(PathExt(path));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp";
}

HRESULT MediaInfo::Load(std::string_view path, MediaInfo& out) {
    out = MediaInfo{};
    if (IsImagePath(path)) return LoadImageInfo(path, out);

    CT_RETURN_IF_FAILED(platform::MfRuntime::EnsureForProcess());
    ComPtr<IMFAttributes> attrs;
    CT_RETURN_IF_FAILED(MFCreateAttributes(attrs.GetAddressOf(), 2));
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, FALSE);
    ComPtr<IMFSourceReader> reader;
    const std::wstring wpath = Utf8ToWide(path);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), reader.GetAddressOf());
    if (FAILED(hr)) {
        CT_LOG_WARN("MediaInfo: cannot open %.*s: %s", static_cast<int>(path.size()), path.data(), HrToString(hr).c_str());
        return hr;
    }

    out.duration_us = detail::ReaderDurationUs(reader.Get());
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(detail::kMediaSource, MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &var))) {
        ULONG chars = 0;
        if (SUCCEEDED(PropVariantToUInt32(var, &chars))) out.seekable = (chars & MFMEDIASOURCE_CAN_SEEK) != 0;
    }
    PropVariantClear(&var);

    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        hr = reader->GetNativeMediaType(i, 0, type.GetAddressOf());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) {
            CT_LOG_WARN("GetNativeMediaType(%lu) failed: %s", i, HrToString(hr).c_str());
            break;
        }
        GUID major{}, subtype{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        StreamInfo s;
        s.index = static_cast<int>(i);
        s.codec = detail::SubtypeName(subtype);
        s.duration_us = out.duration_us;
        if (major == MFMediaType_Video) {
            s.kind = StreamInfo::Kind::Video;
            const detail::VideoTypeInfo vt = detail::ReadVideoType(type.Get());
            s.width = vt.aperture.w;
            s.height = vt.aperture.h;
            s.fps_num = vt.fps_num;
            s.fps_den = vt.fps_den;
            s.rotation_deg = vt.rotation_deg;
        } else if (major == MFMediaType_Audio) {
            s.kind = StreamInfo::Kind::Audio;
            s.sample_rate = static_cast<int>(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
            s.channels = static_cast<int>(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        } else {
            continue;   // subtitles, data streams
        }
        out.streams.push_back(s);
    }
    return S_OK;
}

int MediaInfo::FirstVideo() const {
    for (const StreamInfo& s : streams)
        if (s.kind == StreamInfo::Kind::Video) return s.index;
    return -1;
}

std::vector<int> MediaInfo::AudioStreams() const {
    std::vector<int> out;
    for (const StreamInfo& s : streams)
        if (s.kind == StreamInfo::Kind::Audio) out.push_back(s.index);
    return out;
}

const StreamInfo* MediaInfo::Find(int stream_index) const {
    for (const StreamInfo& s : streams)
        if (s.index == stream_index) return &s;
    return nullptr;
}

} // namespace ct::media
