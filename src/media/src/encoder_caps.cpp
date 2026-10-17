#define CT_LOG_CATEGORY "media"
#include <ct/media/encoder_caps.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/media/mf_runtime.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <optional>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {

SRWLOCK g_cache_lock = SRWLOCK_INIT;
std::optional<std::vector<EncoderInfo>> g_cache[2];

std::string GuidString(const GUID& g) {
    wchar_t buf[64];
    if (StringFromGUID2(g, buf, 64) <= 0) return std::string();
    return WideToUtf8(buf);
}

void EnumerateInto(VideoCodec codec, UINT32 flags, bool hardware_pass, std::vector<EncoderInfo>& out) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outt{MFMediaType_Video, codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outt, &acts, &n);
    if (FAILED(hr)) {
        CT_LOG_WARN("MFTEnumEx(%s, flags 0x%x) failed: %s", CodecName(codec), flags, HrToString(hr).c_str());
        return;
    }
    for (UINT32 i = 0; i < n; ++i) {
        IMFActivate* a = acts[i];
        EncoderInfo e;
        e.codec = codec;
        wchar_t* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
            e.name = WideToUtf8(name);
            CoTaskMemFree(name);
        }
        UINT32 url_len = 0;
        e.hardware = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &url_len));
        GUID clsid{};
        if (SUCCEEDED(a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) e.clsid = GuidString(clsid);
        // The software pass also returns the hardware MFTs on some systems; keep each encoder once.
        bool dup = false;
        for (const EncoderInfo& o : out)
            if (o.name == e.name && o.clsid == e.clsid) dup = true;
        if (dup || (!hardware_pass && e.hardware)) {
            a->Release();
            continue;
        }
        ComPtr<IMFTransform> mft;
        if (SUCCEEDED(a->ActivateObject(IID_PPV_ARGS(mft.GetAddressOf())))) {
            ComPtr<IMFAttributes> attrs;
            UINT32 aware = 0;
            if (SUCCEEDED(mft->GetAttributes(attrs.GetAddressOf())) && attrs &&
                SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)))
                e.d3d_aware = aware != 0;
            mft.Reset();
            a->ShutdownObject();
        }
        a->Release();
        out.push_back(std::move(e));
    }
    CoTaskMemFree(acts);
}

const std::vector<EncoderInfo>& Cached(VideoCodec codec) {
    const int k = codec == VideoCodec::H264 ? 0 : 1;
    AcquireSRWLockExclusive(&g_cache_lock);
    if (!g_cache[k]) {
        std::vector<EncoderInfo> list;
        MfScope mf;
        if (SUCCEEDED(mf.Result())) {
            EnumerateInto(codec, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true, list);
            EnumerateInto(codec, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false, list);
        }
        g_cache[k] = std::move(list);
    }
    const std::vector<EncoderInfo>& r = *g_cache[k];
    ReleaseSRWLockExclusive(&g_cache_lock);
    return r;   // never modified after creation
}

} // namespace

const char* CodecName(VideoCodec c) { return c == VideoCodec::H264 ? "h264" : "hevc"; }

std::vector<EncoderInfo> EncoderCaps::Enumerate(VideoCodec codec) { return Cached(codec); }

bool EncoderCaps::HasHardware(VideoCodec codec) {
    for (const EncoderInfo& e : Cached(codec))
        if (e.hardware) return true;
    return false;
}

std::string EncoderCaps::Report() {
    std::string r;
    for (VideoCodec c : {VideoCodec::H264, VideoCodec::Hevc}) {
        const std::vector<EncoderInfo>& list = Cached(c);
        if (list.empty()) AppendFormat(r, "%s: no encoder\n", CodecName(c));
        for (const EncoderInfo& e : list)
            AppendFormat(r, "%s: %s (%s%s) %s\n", CodecName(c), e.name.c_str(), e.hardware ? "hardware" : "software",
                         e.d3d_aware ? ", D3D11-aware" : "", e.clsid.c_str());
    }
    return r;
}

std::string EncoderCaps::NameForClsid(const std::string& clsid) {
    for (VideoCodec c : {VideoCodec::H264, VideoCodec::Hevc})
        for (const EncoderInfo& e : Cached(c))
            if (IEquals(e.clsid, clsid)) return e.name;
    return std::string();
}

} // namespace ct::media
