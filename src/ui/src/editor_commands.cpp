// Editor commands over EditorState::Edit (one undo entry each).
#include <ct/ui/editor_commands.h>

#include <ct/project/edit_ops.h>

#include <algorithm>
#include <cstring>

namespace ct::ui::cmd {

using namespace ct::project;

namespace {
bool TrackLocked(const Project& p, const Id& track) {
    const Track* t = p.FindTrack(track);
    return !t || t->locked;
}
} // namespace

bool SplitAtPlayhead(EditorState& s) {
    const TimeUs t = s.Playhead();
    std::vector<Id> targets;
    if (!s.selection.clips.empty()) {
        for (const Id& id : s.selection.clips) {
            const Clip* c = s.project.FindClip(id);
            if (c && c->start_us < t && t < c->End() && !TrackLocked(s.project, c->track)) targets.push_back(id);
        }
    } else {
        for (const Clip& c : s.project.clips)
            if (c.start_us < t && t < c.End() && !TrackLocked(s.project, c.track)) targets.push_back(c.id);
    }
    if (targets.empty()) return false;
    std::vector<Id> rights;
    const bool ok = s.EditCopy("Split", [&](Project& p) {
        Err err;
        for (const Id& id : targets) {
            Id l, r;
            if (!ops::Split(p, id, t, &l, &r, &err)) {
                s.last_error = err.message;
                return false;
            }
            rights.push_back(r);
        }
        return true;
    });
    return ok;
}

bool DeleteSelection(EditorState& s, bool ripple) {
    if (!s.selection.clips.empty()) {
        std::vector<Id> ids;
        for (const Id& id : s.selection.clips)
            if (const Clip* c = s.project.FindClip(id); c && !TrackLocked(s.project, c->track)) ids.push_back(id);
        if (ids.empty()) return false;
        Err err;
        const bool ok = s.Edit(ripple ? "Ripple delete" : "Delete", [&](Project& p) { return ops::Delete(p, ids, ripple, &err); });
        if (!ok) s.last_error = err.message;
        else s.PruneSelection();
        return ok;
    }
    if (s.selection.marker) return RemoveMarker(s, *s.selection.marker);
    if (s.selection.camera_key) {
        const TimeUs t = *s.selection.camera_key;
        const bool ok = s.Edit("Delete camera key", [&](Project& p) { return ops::RemoveCameraKey(p, t); });
        if (ok) s.selection.camera_key.reset();
        return ok;
    }
    return false;
}

bool DuplicateSelection(EditorState& s) {
    if (s.selection.clips.empty()) return false;
    std::vector<Id> copies;
    Err err;
    const std::vector<Id> ids = s.selection.clips;
    const bool ok = s.EditCopy("Duplicate", [&](Project& p) {
        for (const Id& id : ids) {
            Id out;
            if (!ops::Duplicate(p, id, &out, &err)) return false;
            copies.push_back(out);
        }
        return true;
    });
    if (!ok) {
        s.last_error = err.message;
        return false;
    }
    s.selection.Clear();
    s.selection.clips = copies;
    return true;
}

void SelectAll(EditorState& s) {
    s.selection.Clear();
    for (const Clip& c : s.project.clips) s.selection.clips.push_back(c.id);
}

void CopySelection(EditorState& s) {
    s.clipboard.clear();
    for (const Id& id : s.selection.clips)
        if (const Clip* c = s.project.FindClip(id)) s.clipboard.push_back(*c);
    std::sort(s.clipboard.begin(), s.clipboard.end(), [](const Clip& a, const Clip& b) { return a.start_us < b.start_us; });
}

bool CutSelection(EditorState& s) {
    CopySelection(s);
    return DeleteSelection(s, s.ripple);
}

bool PasteAtPlayhead(EditorState& s) {
    if (s.clipboard.empty()) return false;
    const TimeUs base = s.clipboard.front().start_us;
    const TimeUs at = s.Playhead();
    std::vector<Id> added;
    Err err;
    const bool ok = s.EditCopy("Paste", [&](Project& p) {
        for (Clip c : s.clipboard) {
            if (!p.FindTrack(c.track)) return false;
            c.id.clear();
            c.start_us = at + (c.start_us - base);
            Id out;
            if (!ops::AddClip(p, std::move(c), &out, &err)) return false;
            added.push_back(out);
        }
        return true;
    });
    if (!ok) {
        s.last_error = err.message.empty() ? "Not enough room at the playhead" : err.message;
        return false;
    }
    s.selection.Clear();
    s.selection.clips = added;
    return true;
}

bool NudgeSelection(EditorState& s, int frames) {
    if (s.selection.clips.empty() || frames == 0) return false;
    const TimeUs d = s.FrameUs() * frames;
    std::vector<Id> ids = s.selection.clips;
    // Move in the direction of travel first so neighbours in the selection never collide.
    std::sort(ids.begin(), ids.end(), [&](const Id& a, const Id& b) {
        const Clip* ca = s.project.FindClip(a);
        const Clip* cb = s.project.FindClip(b);
        if (!ca || !cb) return false;
        return d > 0 ? ca->start_us > cb->start_us : ca->start_us < cb->start_us;
    });
    Err err;
    return s.EditCopy("Nudge", [&](Project& p) {
        for (const Id& id : ids) {
            const Clip* c = p.FindClip(id);
            if (!c) return false;
            const TimeUs ns = c->start_us + d;
            if (ns < 0 || !ops::MoveClip(p, id, ns, "", &err)) return false;
        }
        return true;
    });
}

bool AddMarkerAtPlayhead(EditorState& s) {
    Marker m;
    m.t_us = s.Playhead();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Marker %zu", s.project.markers.size() + 1);
    m.label = buf;
    const bool ok = s.Edit("Add marker", [&](Project& p) { return ops::AddMarker(p, m); });
    if (ok) {
        s.selection.Clear();
        s.selection.marker = m.t_us;
    }
    return ok;
}

bool RemoveMarker(EditorState& s, TimeUs t) {
    const bool ok = s.Edit("Delete marker", [&](Project& p) { return ops::RemoveMarker(p, t); });
    if (ok) s.selection.marker.reset();
    return ok;
}

bool RenameMarker(EditorState& s, TimeUs t, const char* label) {
    return s.Edit("Rename marker", [&](Project& p) {
        for (Marker& m : p.markers) {
            if (m.t_us != t) continue;
            if (m.label == label) return false;
            m.label = label;
            return true;
        }
        return false;
    });
}

bool AddCameraKeyAtPlayhead(EditorState& s) {
    const TimeUs t = s.Playhead();
    const Camera::View v = s.project.camera.At(t);
    CameraKey k;
    k.t_us = t;
    k.zoom = v.zoom > 1.01f ? v.zoom : s.project.camera.auto_params.zoom;
    k.cx = ClampCentre(v.cx, k.zoom);
    k.cy = ClampCentre(v.cy, k.zoom);
    k.ease = Ease::InOut;
    k.generated = false;
    const bool ok = s.Edit("Add camera key", [&](Project& p) { return ops::SetCameraKey(p, k); });
    if (ok) {
        s.selection.Clear();
        s.selection.camera_key = t;
    }
    return ok;
}

bool RemoveNearestCameraKey(EditorState& s) {
    const TimeUs t = s.Playhead();
    const CameraKey* best = nullptr;
    TimeUs best_d = kUsPerSecond;
    for (const CameraKey& k : s.project.camera.keys) {
        const TimeUs d = k.t_us > t ? k.t_us - t : t - k.t_us;
        if (d <= best_d) {
            best = &k;
            best_d = d;
        }
    }
    if (!best) return false;
    const TimeUs kt = best->t_us;
    return s.Edit("Remove camera key", [&](Project& p) { return ops::RemoveCameraKey(p, kt); });
}

bool SetCameraMode(EditorState& s, Camera::Mode mode) {
    if (s.project.camera.mode == mode) return false;
    return s.Edit("Camera mode", [&](Project& p) { return ops::SetCameraMode(p, mode); });
}

bool AddTrack(EditorState& s, Track::Kind kind) {
    const char* base = kind == Track::Kind::Audio ? "Audio" : kind == Track::Kind::Annotations ? "Callouts" : "Video";
    int n = 1;
    for (const Track& t : s.project.tracks) n += t.kind == kind ? 1 : 0;
    char name[32];
    std::snprintf(name, sizeof(name), "%s %d", base, n);
    return s.Edit("Add track", [&](Project& p) { return ops::AddTrack(p, kind, name, nullptr, -1); });
}

bool ToggleTrackFlag(EditorState& s, const Id& track, TrackFlag f) {
    const char* label = f == TrackFlag::Mute ? "Mute track" : f == TrackFlag::Lock ? "Lock track" : "Hide track";
    return s.Edit(label, [&](Project& p) {
        Track* t = p.FindTrack(track);
        if (!t) return false;
        bool& b = f == TrackFlag::Mute ? t->muted : f == TrackFlag::Lock ? t->locked : t->hidden;
        b = !b;
        return true;
    });
}

bool SetClipSpeed(EditorState& s, const Id& clip, double speed) {
    Err err;
    const bool ok = s.Edit("Change speed", [&](Project& p) { return ops::SetSpeed(p, clip, speed, &err); });
    if (!ok) s.last_error = err.message;
    return ok;
}

bool ToggleClipMute(EditorState& s, const Id& clip) {
    const Clip* c = s.project.FindClip(clip);
    if (!c) return false;
    AudioProps a = c->audio;
    a.muted = !a.muted;
    return s.Edit(a.muted ? "Mute clip" : "Unmute clip", [&](Project& p) { return ops::SetAudio(p, clip, a); });
}

bool ResetTransform(EditorState& s, const Id& clip) {
    const Clip* c = s.project.FindClip(clip);
    if (!c) return false;
    Transform t;
    t.extra = c->transform.extra;
    return s.EditCopy("Reset transform", [&](Project& p) {
        if (!ops::SetTransform(p, clip, t)) return false;
        Clip* pc = p.FindClip(clip);
        for (const char* k : {"transform.x", "transform.y", "transform.scale", "transform.rotation_deg", "transform.opacity"})
            pc->keyframes.erase(k);
        return true;
    });
}

bool SetClipColor(EditorState& s, const Id& clip, ColorF c) {
    return s.Edit("Clip color", [&](Project& p) {
        Clip* pc = p.FindClip(clip);
        if (!pc) return false;
        pc->color = c;
        return true;
    });
}

const Clip* NextAdjacent(const EditorState& s, const Id& clip) {
    const Clip* a = s.project.FindClip(clip);
    if (!a) return nullptr;
    const TimeUs frame = s.FrameUs();
    const Clip* best = nullptr;
    for (const Clip& c : s.project.clips) {
        if (c.track != a->track || c.id == a->id) continue;
        const TimeUs gap = c.start_us - a->End();
        if (gap >= 0 && gap <= frame && (!best || c.start_us < best->start_us)) best = &c;
    }
    return best;
}

bool AddTransitionAfter(EditorState& s, const Id& clip) {
    const Clip* a = s.project.FindClip(clip);
    const Clip* b = NextAdjacent(s, clip);
    if (!a || !b) {
        s.last_error = "Transitions need two adjacent clips on the same track";
        return false;
    }
    Transition t;
    t.track = a->track;
    t.a = a->id;
    t.b = b->id;
    const TimeUs half = std::min(a->Duration(), b->Duration()) / 2;
    t.duration_us = std::min<TimeUs>(500'000, half);
    Err err;
    const bool ok = s.Edit("Add transition", [&](Project& p) { return ops::AddTransition(p, t, &err); });
    if (!ok) s.last_error = err.message;
    return ok;
}

void SetLoopIn(EditorState& s) {
    auto& loop = s.project.ui.loop;
    const TimeUs t = s.Playhead();
    const TimeUs out = loop ? loop->second : s.project.Duration();
    loop = std::make_pair(t, out > t ? out : t + s.FrameUs());
    s.Touch();
}

void SetLoopOut(EditorState& s) {
    auto& loop = s.project.ui.loop;
    const TimeUs t = s.Playhead();
    const TimeUs in = loop ? loop->first : 0;
    loop = std::make_pair(in < t ? in : 0, t > 0 ? t : s.FrameUs());
    s.Touch();
}

void ClearLoopIn(EditorState& s) {
    auto& loop = s.project.ui.loop;
    if (!loop) return;
    loop->first = 0;
    if (loop->second >= s.project.Duration()) loop.reset();
    s.Touch();
}

void ClearLoopOut(EditorState& s) {
    auto& loop = s.project.ui.loop;
    if (!loop) return;
    loop->second = s.project.Duration();
    if (loop->first <= 0) loop.reset();
    s.Touch();
}

const char* MediaDisplayName(const Media& m) {
    const char* p = m.path.c_str();
    const char* name = p;
    for (const char* c = p; *c; ++c)
        if ((*c == '/' || *c == '\\') && c[1]) name = c + 1;
    return name;
}

} // namespace ct::ui::cmd
