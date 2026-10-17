#pragma once
// ct/media/media_info.h
// Probes a media file: streams (video/audio), codec, size, frame rate, rotation, audio format, duration, seekability.
// Video/audio files go through a Source Reader without D3D (no decoder is created, so it is fast); images
// (.png .jpg .jpeg .bmp .webp, by extension) through WIC.
// Thread model: any thread with COM initialized (MTA preferred). Plain value type.

#include <ct/core/types.h>

#include <string>
#include <string_view>
#include <vector>

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::media {

struct StreamInfo {
    enum class Kind { Video, Audio } kind = Kind::Video;
    int index = 0;                 // source reader stream index
    std::string codec;             // "H264", "HEVC", "AAC", "MP3", "PCM", "FLAC", ... or the subtype GUID's FOURCC
    int width = 0, height = 0;     // video: coded picture size as reported by the container (display aperture if present)
    int fps_num = 0, fps_den = 1;  // video
    int rotation_deg = 0;          // video: 0/90/180/270 (MF_MT_VIDEO_ROTATION)
    int sample_rate = 0, channels = 0;   // audio
    TimeUs duration_us = 0;        // presentation duration (the same for every stream: MF reports it per source)
};

struct MediaInfo {
    TimeUs duration_us = 0;
    bool seekable = true;
    std::vector<StreamInfo> streams;
    bool is_image = false;

    static HRESULT Load(std::string_view path, MediaInfo& out);   // source reader without D3D (fast); images via WIC
    static bool IsImagePath(std::string_view path);              // by extension

    int FirstVideo() const;                // stream index (StreamInfo::index), -1 if none
    std::vector<int> AudioStreams() const; // stream indices, in file order
    const StreamInfo* Find(int stream_index) const;
};

} // namespace ct::media
