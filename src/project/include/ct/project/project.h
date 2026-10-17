#pragma once
// ct/project/project.h
// The Project aggregate: canvas, media, tracks, clips, transitions, camera, markers, UI state, plus lookups and the
// timeline invariants of docs/04 section 3 (Validate). Edits go through ct/project/edit_ops.h.
// Thread model: one owner (the UI thread). Never keep pointers into the vectors across edits; use ids.

#include <ct/core/json.h>
#include <ct/core/types.h>
#include <ct/project/camera.h>
#include <ct/project/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ct::project {

constexpr int kProjectVersion = 1;
constexpr int kMinCanvas = 16, kMaxCanvas = 8192;
constexpr double kMinSpeed = 0.1, kMaxSpeed = 16.0;

struct Project {
    int version = kProjectVersion;
    Id id;                 // UUID; also names the autosave file
    std::string name;
    std::string path;      // .ctproj location; not serialized
    uint64_t next_id = 1;  // not serialized: Load/FromJson set it past the largest id counter in the file
    Canvas canvas;
    std::vector<Media> media;
    std::vector<Track> tracks;        // index 0 = bottom layer (drawn first)
    std::vector<Clip> clips;
    std::vector<Transition> transitions;
    Camera camera;
    std::vector<Marker> markers;      // sorted by t_us, unique times
    UiState ui;
    Json extra;                       // unknown top-level keys preserved

    // docs/04 section 3 invariants: clips on one track never overlap; 0 <= in_us < out_us <= media duration (images
    // and annotation clips unbounded); speed in [kMinSpeed, kMaxSpeed]; ids unique; referenced ids exist and match the
    // track kind; keyframe arrays sorted; transitions between adjacent clips; canvas even and in [16, 8192].
    bool Validate(std::string* why) const;

    TimeUs Duration() const;   // End() of the last clip, 0 when empty
    Id NewId(const char* prefix);   // prefix + base-36 counter, skipping ids that already exist

    Media* FindMedia(const Id& id);
    Track* FindTrack(const Id& id);
    Clip* FindClip(const Id& id);
    const Media* FindMedia(const Id& id) const;
    const Track* FindTrack(const Id& id) const;
    const Clip* FindClip(const Id& id) const;
    int TrackIndex(const Id& id) const;   // -1 when missing

    std::vector<Clip*> ClipsOnTrack(const Id& track);              // sorted by start_us
    std::vector<const Clip*> ClipsOnTrack(const Id& track) const;  // sorted by start_us
    // Clips with start_us <= t < End() on tracks that are not hidden, bottom track first.
    std::vector<const Clip*> ClipsAt(TimeUs t) const;
    // First Source::Screen clip (lowest track, then earliest start): drives ScreenFit in the compositor.
    const Clip* PrimaryScreenClip() const;

    // Whether `clip` may live on `track`: video tracks take recording (screen/camera), video and image media; audio
    // tracks take recording (mic/system), video and audio media; annotation tracks take annotation clips only.
    bool ClipFits(const Clip& clip, const Track& track, std::string* why) const;
    // Largest allowed out_us for the clip's media (INT64_MAX for images, annotations and unknown durations).
    TimeUs MaxOutUs(const Clip& clip) const;

    // Recomputes next_id from the ids present (used after loading).
    void RecomputeNextId();
};

} // namespace ct::project
