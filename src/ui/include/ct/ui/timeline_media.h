#pragma once
// ct/ui/timeline_media.h
// What the timeline needs from media caches (P06 playback::MediaCache / thumbnails / peaks), behind an interface so
// the widget builds and is testable before P06/P09-part-2 exist. Implementations must never block: return
// Loading and request a redraw (App::RequestRedraw) when the data lands.
// Thread model: called on the UI thread inside the frame. Texture ids must stay valid until the frame is rendered
// (pin entries used this frame; see P09 pitfalls).

#include <ct/core/types.h>
#include <ct/project/types.h>

#include <imgui.h>

#include <cstddef>

namespace ct::ui {

enum class MediaStatus { Ready, Loading, Unavailable };

class TimelineMediaProvider {
public:
    virtual ~TimelineMediaProvider() = default;
    // Thumbnail of `media` near media time `t` (160x90 nominal). Ready fills tex (+ uv rect for atlases).
    virtual MediaStatus Thumbnail(const project::Id& media, TimeUs t, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) = 0;
    // Filmstrip thumbnail of a timeline clip at media time `t`. Default: Thumbnail(clip.media, ...); a provider with
    // camera files overrides it so Source::Camera clips of a recording show the webcam, not the screen.
    virtual MediaStatus ClipThumbnail(const project::Clip& clip, TimeUs t, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) {
        return Thumbnail(clip.media, t, tex, uv0, uv1);
    }
    // Mono min/max peak pairs for audio stream `stream` (index into Media::audio_streams) of `media`:
    // out[2*i] = min, out[2*i+1] = max in [-1, 1], at `*per_second` pairs per second of media time (100 in P06).
    // Returns nullptr while loading/unavailable (`*status` says which).
    virtual const float* PeaksMinMax(const project::Id& media, int stream, size_t* pair_count, int* per_second,
                                     MediaStatus* status) = 0;
};

// Before P06: no thumbnails (static frame placeholders), no peaks (flat line).
class NullMediaProvider final : public TimelineMediaProvider {
public:
    MediaStatus Thumbnail(const project::Id&, TimeUs, ImTextureID&, ImVec2&, ImVec2&) override { return MediaStatus::Unavailable; }
    const float* PeaksMinMax(const project::Id&, int, size_t* n, int* per_second, MediaStatus* status) override {
        if (n) *n = 0;
        if (per_second) *per_second = 0;
        if (status) *status = MediaStatus::Unavailable;
        return nullptr;
    }
    static NullMediaProvider& Instance();
};

} // namespace ct::ui
