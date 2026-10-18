#pragma once
// ct/project/auto_zoom.h
// Auto-Zoom and Auto-Follow: deterministic camera keys generated from click / typing events of a recording.
// Pure functions of their inputs (same events + params -> same keys, bit for bit).
// Thread model: pure; safe on any thread (the editor may run it on the IO pool for long recordings).

#include <ct/core/types.h>
#include <ct/events/event_log.h>
#include <ct/project/camera.h>
#include <ct/project/project.h>

#include <functional>
#include <vector>

namespace ct::project {

struct InterestEvent {
    TimeUs t_us = 0;
    float x = 0, y = 0;   // normalized to the screen video frame
    enum class Kind { Click, Typing } kind = Kind::Click;
};

struct AutoZoom {
    static constexpr TimeUs kFollowSampleUs = 100'000;   // cursor sampling inside a zoomed segment
    static constexpr TimeUs kFollowPanUs = 300'000;      // duration of one follow pan (Ease::Out)
    static constexpr TimeUs kFollowMinGapUs = 400'000;   // at most one follow move per 400 ms
    static constexpr float kAbsorbRadius = 0.25f;        // events closer than this to a segment centre join it

    // Left/right mouse downs (on_click) and typing-burst starts at the cursor position (on_typing), sorted by time.
    static std::vector<InterestEvent> ExtractInterest(const events::EventLog& log, SizeI video_size, const AutoZoomParams& params);

    // Camera keys over [0, duration_us] (media time of the recording), all with generated = true:
    // 1. events -> segments: start at e (t_in = e.t - lead), absorb followers within 1.5 * min_hold and 0.25 of the
    //    running-mean centre; t_out = last.t + min_hold.
    // 2. clamp to [ease, duration - ease] (room for the zoom ramps), drop segments shorter than min_hold, then for
    //    neighbours closer than 2 * ease: merge them when their centres are within 0.25 (or a pan does not fit),
    //    else keep both and pan from c1 to c2 over ease_ms instead of zooming out in between.
    // 3. keys per segment: (t_in - ease, 1, .5/.5, InOut) (t_in, Z, c, Hold) ... (t_out, Z, c, InOut)
    //    (t_out + ease, 1, .5/.5, Hold); centres clamped to [1/(2Z), 1 - 1/(2Z)].
    // 4. follow (params.follow and a log with a region size): every 100 ms inside a segment, when the cursor is more
    //    than follow_deadzone / Z from the centre, pan the centre towards it by the overshoot (two keys: start with
    //    Ease::Out, end with Hold, 300 ms apart), at most once per 400 ms.
    // The result is continuous: Camera::At never jumps.
    static std::vector<CameraKey> Generate(const std::vector<InterestEvent>& events, TimeUs duration_us,
                                           const AutoZoomParams& params, const events::EventLog* for_follow);

    // Keys for one screen clip on the timeline: events inside [in_us, out_us) of the clip's media, mapped to timeline
    // time (start_us + (t - in_us) / speed).
    static std::vector<CameraKey> ForClip(const Clip& clip, const events::EventLog& log, SizeI video_size,
                                          const AutoZoomParams& params);

    // Inserts generated keys without touching existing keys at the same times (import appends per recording).
    static void AppendKeys(Camera& camera, const std::vector<CameraKey>& keys);

    // The loaded event log of a Recording media, or nullptr (no events, not loaded yet, unreadable).
    using LogLookup = std::function<const events::EventLog*(const Media&)>;
    // "Regenerate" (P09 9.5): ForClip over every Screen clip of a Recording media that has a log, in timeline order;
    // keys sorted with unique times (on equal times the earlier clip wins). Each clip's video size is the media's
    // (the canvas when unknown), as on import. *clips = number of clips that contributed.
    static std::vector<CameraKey> ForProject(const Project& p, const LogLookup& log_for, int* clips = nullptr);
    // Replaces the generated keys of p.camera with ForProject (manual keys stay; a manual key wins at equal times).
    // Returns false and leaves p untouched when no clip has a log. *changed: the key list differs from before.
    static bool Regenerate(Project& p, const LogLookup& log_for, bool* changed = nullptr, int* clips = nullptr);
};

} // namespace ct::project
