#pragma once
// ct/media/encoder_caps.h
// Which H.264/HEVC encoders Media Foundation offers on this machine (hardware first), for logs, the About dialog
// and to gate the HEVC option in the UI.
// Thread model: thread-safe; the first call per codec enumerates (MFTEnumEx + activating each MFT once to read
// MF_SA_D3D11_AWARE, tens of ms) and the result is cached for the process. Needs COM on the calling thread.
// Never call it on the UI thread (use ct::Async).

#include <ct/media/encode_config.h>

#include <string>
#include <vector>

namespace ct::media {

struct EncoderInfo {
    std::string name;          // MFT_FRIENDLY_NAME_Attribute, e.g. "AMDh264Encoder", "H264 Encoder MFT"
    bool hardware = false;     // MFT_ENUM_HARDWARE_URL_Attribute present
    VideoCodec codec = VideoCodec::H264;
    bool d3d_aware = false;    // MF_SA_D3D11_AWARE on the activated transform
    std::string clsid;         // "{...}" (MFT_TRANSFORM_CLSID_Attribute); names the encoder a sink writer picked
};

struct EncoderCaps {
    // MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, HARDWARE | SORTANDFILTER), then SYNCMFT | ASYNCMFT | SORTANDFILTER for
    // software; input NV12, output the codec. Hardware entries first.
    static std::vector<EncoderInfo> Enumerate(VideoCodec);
    static bool HasHardware(VideoCodec);
    static std::string Report();                                 // one line per encoder, both codecs
    static std::string NameForClsid(const std::string& clsid);   // empty if unknown
};

const char* CodecName(VideoCodec);   // "h264" / "hevc"

} // namespace ct::media
