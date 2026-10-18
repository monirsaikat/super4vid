#pragma once
// ct/ui/editor_state.h
// The editor document: project + undo + autosave + selection + timeline view, and the single mutation entry point
// Edit(label, fn). Widgets never call project::ops directly; they call state.Edit so undo, dirty, the preview
// re-render flag and autosave stay consistent (P09 pitfalls).
//
// This is the first half of P09's EditorState (docs/phases/P09 "Core structures"). The preview/compositor/player
// members land with the second half; until then playback goes through the PlaybackControl interface below
// (render::TimelinePlayer implements it) and the preview reads `needs_render`.
// Thread model: UI thread only; workers post results.

#include <ct/core/types.h>
#include <ct/project/autosave.h>
#include <ct/project/project.h>
#include <ct/project/serialize.h>
#include <ct/project/undo.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ct::ui {

enum class Tool { Select, Razor, Hand };

struct Selection {
    std::vector<project::Id> clips;
    std::optional<TimeUs> camera_key;
    std::optional<TimeUs> marker;
    std::optional<std::pair<project::Id, std::string>> keyframe;   // clip, prop
    TimeUs keyframe_t = 0;

    void Clear();
    bool Empty() const;
    bool Has(const project::Id& clip) const;
    void Set(const project::Id& clip);      // only this clip (clears markers/keys too)
    void Toggle(const project::Id& clip);   // Ctrl+click
    void Add(const project::Id& clip);
};

// Transport the editor drives. render::TimelinePlayer (P09 second half) implements it; the default implementation
// only moves the playhead (no media), which is what the timeline/tests/demo need.
class PlaybackControl {
public:
    virtual ~PlaybackControl() = default;
    virtual bool Playing() const = 0;
    virtual double Rate() const = 0;           // signed; 0 when paused
    virtual void Play(double rate) = 0;        // 1 = normal, negative = reverse
    virtual void Pause() = 0;
    virtual void Seek(TimeUs t) = 0;           // sets the playhead (project.ui.playhead_us) and the clock
    virtual void HintScrub(TimeUs t) = 0;      // paused prefetch around t
    // Called once per frame by the editor; advances the playhead while playing. Returns true while playing.
    virtual bool Tick(double now_s) = 0;
    // Output volume (linear 0..2) and mute (P09 second half; players without audio keep the defaults).
    virtual float Volume() const { return 1.0f; }
    virtual void SetVolume(float) {}
    virtual bool Muted() const { return false; }
    virtual void SetMuted(bool) {}
};

struct EditorState;

// Moves project.ui.playhead_us in real time at Rate(); clamps to [0, Duration]; honours the loop range.
class SimplePlayback final : public PlaybackControl {
public:
    explicit SimplePlayback(EditorState& s) : m_state(s) {}
    bool Playing() const override { return m_rate != 0; }
    double Rate() const override { return m_rate; }
    void Play(double rate) override;
    void Pause() override { m_rate = 0; }
    void Seek(TimeUs t) override;
    void HintScrub(TimeUs) override {}
    bool Tick(double now_s) override;

private:
    EditorState& m_state;
    double m_rate = 0;
    double m_last_s = -1;
};

struct EditorState {
    EditorState();
    EditorState(const EditorState&) = delete;
    EditorState& operator=(const EditorState&) = delete;

    project::Project project;
    project::UndoStack undo;
    project::Autosave autosave;   // Init() by the owner when the document has an id (not in tests/demo)

    Selection selection;
    Tool tool = Tool::Select;
    bool snap = true;
    bool ripple = false;

    // Timeline view (P09 9.4)
    double pps = 100;          // pixels per second (DIP), [2, 2000]
    TimeUs scroll_us = 0;      // time at the left edge of the lanes
    float scroll_y = 0;        // vertical lane scroll (DIP)
    float track_scale = 1;     // Alt+Wheel lane height factor [0.5, 3]

    bool needs_render = true;  // preview re-render (any model or playhead change)
    uint64_t revision = 0;     // bumped by every model change (caches key on it)
    std::string last_error;    // last failed edit's reason (status bar / banner)

    PlaybackControl* playback = nullptr;   // never null after construction (defaults to `simple_playback`)
    SimplePlayback simple_playback;
    std::vector<project::Clip> clipboard;

    TimeUs Playhead() const { return project.ui.playhead_us; }
    void SetPlayhead(TimeUs t);            // clamps >= 0, flags the preview
    TimeUs FrameUs() const;                // one canvas frame (>= 1)
    bool IsDirty() const { return undo.IsDirty(); }

    // Every mutation goes through Edit: snapshot for undo (one per drag while BeginDrag is active), apply `fn`
    // (bool(project::Project&)), and on success bump revision, flag the preview and notify autosave.
    // `fn` must leave the project untouched when it returns false - true for any single project::ops call. Use
    // EditCopy for multi-op edits: it runs `fn` on a copy and swaps it in only on success.
    template <class Fn>
    bool Edit(const char* label, Fn&& fn) {
        const bool snapshot = !(m_drag_active && m_drag_pushed);
        std::string before;
        if (snapshot) before = project::ToJson(project);
        if (!fn(project)) return false;
        Commit(snapshot, std::move(before), label);
        return true;
    }
    template <class Fn>
    bool EditCopy(const char* label, Fn&& fn) {
        project::Project work = project;
        if (!fn(work)) return false;
        const bool snapshot = !(m_drag_active && m_drag_pushed);
        std::string before;
        if (snapshot) before = project::ToJson(project);
        project = std::move(work);
        Commit(snapshot, std::move(before), label);
        return true;
    }

    // Like Edit, but when nothing changed the document since `amend_revision` (the `revision` right after an earlier
    // edit) and no drag is open, the change joins that edit's undo entry instead of pushing its own (live Auto-Zoom
    // regeneration after a parameter change: one undo reverts parameters and keys). Otherwise a normal Edit(label).
    template <class Fn>
    bool EditAmend(const char* label, uint64_t amend_revision, Fn&& fn) {
        if (revision != amend_revision || m_drag_active || !undo.CanUndo()) return Edit(label, fn);
        if (!fn(project)) return false;
        undo.Amend();
        Commit(false, std::string(), label);
        return true;
    }

    // Undo coalescing for drags: all Edits between Begin and End form one undo entry labelled `label`.
    void BeginDrag(const char* label);
    void EndDrag();
    bool DragActive() const { return m_drag_active; }

    bool Undo();
    bool Redo();

    // Replaces the document (after Load / New): clears undo, selection, marks saved.
    void SetProject(project::Project p);
    void PruneSelection();   // drops ids that no longer exist (after ripple/remove/undo)
    void Touch();            // non-undoable change that still invalidates caches (e.g. track mute from the player)

private:
    void Commit(bool snapshot, std::string before, const char* label);
    bool m_drag_active = false;
    bool m_drag_pushed = false;
    std::string m_drag_label;
};

} // namespace ct::ui
