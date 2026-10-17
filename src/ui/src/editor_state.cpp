// EditorState: the Edit() wrapper over project::ops + UndoStack coalescing, selection, simple playback.
#include <ct/ui/editor_state.h>

#include <ct/core/time.h>

#include <algorithm>

namespace ct::ui {

void Selection::Clear() {
    clips.clear();
    camera_key.reset();
    marker.reset();
    keyframe.reset();
    keyframe_t = 0;
}

bool Selection::Empty() const { return clips.empty() && !camera_key && !marker && !keyframe; }

bool Selection::Has(const project::Id& clip) const {
    return std::find(clips.begin(), clips.end(), clip) != clips.end();
}

void Selection::Set(const project::Id& clip) {
    Clear();
    clips.push_back(clip);
}

void Selection::Toggle(const project::Id& clip) {
    auto it = std::find(clips.begin(), clips.end(), clip);
    if (it != clips.end()) clips.erase(it);
    else clips.push_back(clip);
    camera_key.reset();
    marker.reset();
}

void Selection::Add(const project::Id& clip) {
    if (!Has(clip)) clips.push_back(clip);
}

// --- SimplePlayback ---------------------------------------------------------------------------------------------

void SimplePlayback::Play(double rate) {
    m_rate = rate;
    m_last_s = -1;
    if (rate > 0 && m_state.Playhead() >= m_state.project.Duration()) m_state.SetPlayhead(0);
}

void SimplePlayback::Seek(TimeUs t) { m_state.SetPlayhead(t); }

bool SimplePlayback::Tick(double now_s) {
    if (m_rate == 0) {
        m_last_s = -1;
        return false;
    }
    if (m_last_s < 0) m_last_s = now_s;
    const double dt = now_s - m_last_s;
    m_last_s = now_s;
    const TimeUs end = m_state.project.Duration();
    TimeUs t = m_state.Playhead() + static_cast<TimeUs>(dt * m_rate * 1e6);
    const auto& loop = m_state.project.ui.loop;
    if (loop && loop->second > loop->first && m_rate > 0 && t >= loop->second) t = loop->first + (t - loop->second);
    if (t <= 0 && m_rate < 0) {
        t = 0;
        m_rate = 0;
    }
    if (t >= end && m_rate > 0) {
        t = end;
        m_rate = 0;
    }
    m_state.SetPlayhead(t);
    return m_rate != 0;
}

// --- EditorState ------------------------------------------------------------------------------------------------

EditorState::EditorState() : simple_playback(*this) { playback = &simple_playback; }

void EditorState::SetPlayhead(TimeUs t) {
    if (t < 0) t = 0;
    if (project.ui.playhead_us != t) {
        project.ui.playhead_us = t;
        needs_render = true;
    }
}

TimeUs EditorState::FrameUs() const {
    const auto& c = project.canvas;
    if (c.fps_num <= 0 || c.fps_den <= 0) return 16'667;
    const TimeUs f = FrameTimeUs(1, c.fps_num, c.fps_den);
    return f > 0 ? f : 1;
}

void EditorState::Commit(bool snapshot, std::string before, const char* label) {
    if (snapshot) {
        undo.PushJson(std::move(before), m_drag_active ? m_drag_label.c_str() : label);
        if (m_drag_active) m_drag_pushed = true;
        autosave.NotifyChanged();
    }
    ++revision;
    needs_render = true;
    last_error.clear();
}

void EditorState::BeginDrag(const char* label) {
    if (m_drag_active) EndDrag();
    m_drag_active = true;
    m_drag_pushed = false;
    m_drag_label = label ? label : "";
    undo.BeginCoalesce(m_drag_label.c_str());
}

void EditorState::EndDrag() {
    if (!m_drag_active) return;
    undo.EndCoalesce();
    m_drag_active = false;
    m_drag_pushed = false;
}

bool EditorState::Undo() {
    EndDrag();
    if (!undo.Undo(project)) return false;
    ++revision;
    needs_render = true;
    autosave.NotifyChanged();
    PruneSelection();
    return true;
}

bool EditorState::Redo() {
    EndDrag();
    if (!undo.Redo(project)) return false;
    ++revision;
    needs_render = true;
    autosave.NotifyChanged();
    PruneSelection();
    return true;
}

void EditorState::SetProject(project::Project p) {
    EndDrag();
    project = std::move(p);
    undo.Clear();
    undo.MarkSaved();
    selection.Clear();
    scroll_us = project.ui.scroll_us;
    scroll_y = 0;
    ++revision;
    needs_render = true;
}

void EditorState::PruneSelection() {
    auto& c = selection.clips;
    c.erase(std::remove_if(c.begin(), c.end(), [&](const project::Id& id) { return project.FindClip(id) == nullptr; }), c.end());
    if (selection.camera_key) {
        bool found = false;
        for (const auto& k : project.camera.keys) found |= k.t_us == *selection.camera_key;
        if (!found) selection.camera_key.reset();
    }
    if (selection.marker) {
        bool found = false;
        for (const auto& m : project.markers) found |= m.t_us == *selection.marker;
        if (!found) selection.marker.reset();
    }
    if (selection.keyframe && !project.FindClip(selection.keyframe->first)) selection.keyframe.reset();
}

void EditorState::Touch() {
    ++revision;
    needs_render = true;
}

} // namespace ct::ui
