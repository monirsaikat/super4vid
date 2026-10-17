#pragma once
// ct/project/edit_ops.h
// Every edit of a Project goes through these operations. Each one validates its arguments against the timeline
// invariants (Project::Validate), and either mutates the project and returns true, or leaves it untouched and
// returns false (with a user-presentable Err where the signature has one). Clip operations refuse locked tracks.
// Transitions that stop being valid after an edit (clips no longer adjacent, or too short) are removed.
// Thread model: UI thread (the project owner). Callers push an undo snapshot before calling a mutating op.

#include <ct/core/error.h>
#include <ct/project/project.h>

#include <optional>
#include <string>
#include <vector>

namespace ct::project::ops {

// Media / tracks. AddMedia assigns a fresh id when m.id is empty or taken.
bool AddMedia(Project& p, Media m, Id* out);
bool RemoveMedia(Project& p, const Id& media, bool remove_clips, Err* err);   // fails while clips use it, unless remove_clips
bool AddTrack(Project& p, Track::Kind kind, std::string name, Id* out, int at_index = -1);   // -1 = on top
bool RemoveTrack(Project& p, const Id& track, Err* err);   // removes its clips and transitions
bool MoveTrack(Project& p, const Id& track, int new_index);

// Clips. Times are timeline microseconds unless named in/out (media time).
bool AddClip(Project& p, Clip c, Id* out, Err* err);   // rejects overlap on the track; fresh id when c.id is empty or taken
// new_track empty = keep the track. Rejects overlap and incompatible track kinds.
bool MoveClip(Project& p, const Id& clip, TimeUs new_start, const Id& new_track, Err* err);
bool TrimIn(Project& p, const Id& clip, TimeUs new_start_us, Err* err);   // keeps End() fixed; in_us >= 0
bool TrimOut(Project& p, const Id& clip, TimeUs new_end_us, Err* err);    // out_us <= media duration (images: unbounded)
bool Slip(Project& p, const Id& clip, TimeUs delta_us, Err* err);         // shifts in/out together
// The left part keeps the id. Keyframes are split and re-based (values at the split are kept on both sides);
// transitions into the clip stay on the left part, transitions out of it move to the right part.
bool Split(Project& p, const Id& clip, TimeUs at_us, Id* left, Id* right, Err* err);
// ripple: later clips on the same tracks shift left by the durations of the removed clips before them.
bool Delete(Project& p, const std::vector<Id>& clips, bool ripple, Err* err);
bool Duplicate(Project& p, const Id& clip, Id* out, Err* err);   // placed right after the source if free, else fails
bool SetSpeed(Project& p, const Id& clip, double speed, Err* err);   // keeps start_us; Duration = (out-in)/speed

// Properties: false when the clip is missing or a value is out of range (non-finite, negative sizes, ...).
bool SetTransform(Project& p, const Id& clip, const Transform& t);
bool SetCrop(Project& p, const Id& clip, const Crop& c);
bool SetAudio(Project& p, const Id& clip, const AudioProps& a);
bool SetCursorStyle(Project& p, const Id& clip, const CursorStyle& s);
bool SetKeystrokeStyle(Project& p, const Id& clip, const KeystrokeStyle& s);
bool SetPip(Project& p, const Id& clip, const std::optional<Pip>& pip);
bool SetAnnotation(Project& p, const Id& clip, const Annotation& a);   // annotation clips only
// t is relative to the clip start; prop must be one of kKeyframeProps.
bool SetKeyframe(Project& p, const Id& clip, const std::string& prop, TimeUs t, float v, Ease ease);
bool RemoveKeyframe(Project& p, const Id& clip, const std::string& prop, TimeUs t);

// Transitions: a and b adjacent on the same track (0 <= b.start - a.End() <= 1 frame), duration <= min(a, b) / 2.
bool AddTransition(Project& p, Transition t, Err* err);
bool RemoveTransition(Project& p, const Id& a, const Id& b);

bool SetCameraKey(Project& p, CameraKey key);   // inserts or replaces at key.t_us
bool RemoveCameraKey(Project& p, TimeUs t);
bool SetCameraMode(Project& p, Camera::Mode mode);

bool AddMarker(Project& p, Marker m);   // false when a marker already exists at m.t_us
bool RemoveMarker(Project& p, TimeUs t);
bool MoveMarker(Project& p, TimeUs from, TimeUs to);

bool SetCanvas(Project& p, const Canvas& c, Err* err);

// Nearest snap candidate within tolerance, else t. Candidates: 0, start/end of every clip not listed in `ignore`,
// markers, and playhead positions passed as "playhead:<t_us>" entries in `ignore`.
TimeUs Snap(const Project& p, TimeUs t, TimeUs tolerance_us, const std::vector<Id>& ignore);

} // namespace ct::project::ops
