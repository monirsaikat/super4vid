#pragma once
// ct/media/peaks.h
// Waveform overview of one audio stream: min/max of the mono mix ((L + R) / 2 at 48 kHz) per 1/per_second window
// (10 ms). Built by a full AudioDecoder pass (slow: run it on the IO pool) and cached on disk as
// %LOCALAPPDATA%\Camtesia\cache\<fnv64(path|size|mtime|stream)>.peaks (portable mode: <exe>\data\cache).
// Thread model: plain value type; Build/LoadCache/SaveCache are blocking and thread-safe (no shared state).

#include <ct/core/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::media {

struct Peaks {
    int per_second = 100;
    std::vector<float> min, max;   // one value per window, in [-1, 1]
    TimeUs duration_us = 0;        // decoded length

    static HRESULT Build(std::string_view path, int stream, Peaks&);                 // stream -1 = first audio stream
    static bool LoadCache(std::string_view media_path, int stream, Peaks&);          // false when absent/stale/corrupt
    static void SaveCache(std::string_view media_path, int stream, const Peaks&);    // best effort (logged)
    static std::string CachePath(std::string_view media_path, int stream);           // empty when the file is missing
};

} // namespace ct::media
