#pragma once
// ct/media/probe.h
// Minimal source-reader probe of a media file (duration, stream counts, video size/fps/codec) used by the P04 tests
// and tools; P06 replaces the internals with MediaInfo. ScanStream reads every compressed sample of one stream
// (no decoding) for frame counts and timestamps.
// Stream numbers here are the file's track order (the order Mp4Writer created the streams: video 0, then audio in
// Config order). Note: the MF source reader's own stream indices are the reverse of the track order.
// Thread model: free functions, any thread with COM initialized (MTA). They block on file IO: never on the UI thread.

#include <ct/core/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::media {

struct ProbeResult {
    TimeUs duration_us = 0;
    int video_width = 0, video_height = 0;
    int fps_num = 0, fps_den = 0;
    int video_streams = 0, audio_streams = 0;
    std::string video_codec;   // "h264", "hevc", or the subtype GUID
    // Additions: per-stream subtype names in file order ("h264", "aac", ...) and audio format of the first audio stream.
    std::vector<std::string> stream_codecs;
    int audio_sample_rate = 0, audio_channels = 0;
};
HRESULT Probe(std::string_view path, ProbeResult&);

struct StreamScan {
    uint64_t samples = 0;
    uint64_t bytes = 0;
    uint64_t keyframes = 0;              // MFSampleExtension_CleanPoint
    TimeUs first_us = -1, last_us = -1;  // sample times as the source reader reports them (presentation order)
    TimeUs end_us = 0;                   // max(sample time + duration)
    TimeUs max_gap_us = 0;               // largest step between consecutive sample times
    std::vector<TimeUs> times;           // every sample time, when requested
};
// stream_index: the file's track order (video = 0, mic = 1, system = 2 for Camtesia recordings).
HRESULT ScanStream(std::string_view path, uint32_t stream_index, StreamScan&, bool keep_times = false);

} // namespace ct::media
