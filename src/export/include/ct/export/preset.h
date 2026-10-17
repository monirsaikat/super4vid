#pragma once
// ct/export/preset.h
// Export presets (docs/04 section 5): the built-in list, JSON (de)serialization, user presets under
// %APPDATA%\Camtesia\presets\<id>.json and a file size estimate for the export dialog.
// Thread model: Preset is a plain value. Builtin/ToJson/FromJson/EstimateBytes are pure and thread-safe; LoadUser and
// SaveUser do disk IO (call them from ct::Async, not the UI thread) and need platform::Paths::Init() first.

#include <ct/core/error.h>
#include <ct/core/json.h>
#include <ct/core/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct::exportng {

// Same enumerators, in the same order, as ct::media::VideoCodec / ct::media::RateControl (P04). ct_export does not
// link ct_media for presets; ExportJob converts with static_cast (see DECISIONS.md, P10 task 10.1).
enum class VideoCodec { H264, Hevc };
enum class RateControl { Quality, Vbr, Cbr };

struct Preset {
    enum class Container { Mp4, Gif, Png, M4a };

    std::string id, name;   // id: [a-z0-9_-]{1,64}, also the user preset file name
    Container container = Container::Mp4;
    struct Video {
        VideoCodec codec = VideoCodec::H264;
        int width = 0, height = 0;     // 0 = canvas size; otherwise even, 16..8192
        int fps_num = 0, fps_den = 1;  // 0 = canvas fps ("source" in JSON)
        RateControl rc = RateControl::Vbr;
        uint32_t bitrate_kbps = 10000, max_bitrate_kbps = 16000;
        int quality = 75;              // 0..100, used by RateControl::Quality
        int gop_s = 2;
        bool bframes = true;
        std::string profile = "high";  // "baseline" | "main" | "high"
        bool operator==(const Video&) const = default;
    } video;
    struct Audio {
        uint32_t bitrate_kbps = 192;   // AAC: 64..320
        int sample_rate = 48000;       // 44100 | 48000
        int channels = 2;              // 1 | 2
        bool operator==(const Audio&) const = default;
    } audio;
    struct Gif {
        int max_colors = 256;          // 2..256
        bool dither = true;
        bool per_frame_palette = false;
        int fps = 20;                  // 1..50
        bool operator==(const Gif&) const = default;
    } gif;
    bool fast_start = true;

    bool operator==(const Preset&) const = default;

    static std::vector<Preset> Builtin();   // ids listed in docs/04 section 5, in dialog order
    static const Preset* FindBuiltin(std::string_view id);

    // %APPDATA%\Camtesia\presets (platform::Paths::SettingsDir() + "\\presets"); empty before Paths::Init().
    static std::string UserDir();
    // Every valid <dir>\*.json, sorted by name then id; invalid files are logged and skipped.
    static std::vector<Preset> LoadUser();
    static std::vector<Preset> LoadUserFrom(std::string_view dir);
    bool SaveUser(Err* err) const;                           // UserDir()\<id>.json, atomic write
    bool SaveUserTo(std::string_view dir, Err* err) const;   // creates dir

    Json ToJson() const;   // always writes every section (container-independent), see docs/04 section 5
    // Missing keys keep their defaults; wrong types, unknown enum names and out-of-range values fail with a message
    // naming the key. `out` is only written on success.
    static bool FromJson(const Json& j, Preset& out, Err* err);
    static bool IsValidId(std::string_view id);

    // Estimated output size in bytes. Zero width/height/fps (= canvas) use `canvas` / `canvas_fps`.
    // MP4: (video + audio bitrate) * duration + 1 % container overhead; Quality mode derives a bitrate from the
    // pixel rate and the quality. GIF: screen-content heuristic (bytes per pixel per frame after frame diff).
    // PNG: one frame. M4A: audio bitrate.
    uint64_t EstimateBytes(TimeUs duration, SizeI canvas = {1920, 1080}, int canvas_fps = 60) const;
};

const char* ContainerName(Preset::Container c);       // "mp4" | "gif" | "png" | "m4a"
const char* ContainerExtension(Preset::Container c);  // ".mp4" | ".gif" | ".png" | ".m4a"

} // namespace ct::exportng
