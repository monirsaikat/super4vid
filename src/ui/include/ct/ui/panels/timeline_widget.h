#pragma once
// ct/ui/panels/timeline_widget.h
// The custom-drawn timeline (docs/05 section 4, docs/phases/P09 9.4): header column, ruler with markers and loop
// range, camera-keyframe lane, track lanes with clips (filmstrip/waveform through TimelineMediaProvider), playhead,
// scrollbars; selection, rubber band, move/trim/slip/duplicate drags with snapping, scrubbing, marker and camera
// key drags, context menus, drop targets for "CT_MEDIA" (payload: NUL-terminated media id) and "CT_ANNOTATION"
// (payload: int Annotation::Type). Every mutation goes through EditorState::Edit; a drag is one undo entry.
// Culling: per track the clips are sorted by start (cached per EditorState::revision) and binary searched.
// Thread model: UI thread, inside an ImGui window.

#include <ct/project/project.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/timeline_math.h>
#include <ct/ui/timeline_media.h>

#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct::ui {

class TimelineWidget {
public:
    enum class Mode {
        Idle, Pressed, Move, TrimIn, TrimOut, Slip, Duplicate, RubberBand, Scrub, MarkerDrag, CameraKeyDrag,
        TrackReorder, HScroll, VScroll,
    };
    enum class Zone { Body, TrimIn, TrimOut };

    struct Lane {
        enum class Kind { CameraKeys, Track } kind = Kind::Track;
        int track = -1;          // index into project.tracks
        float y = 0, h = 0;      // screen y (after scroll) and height
        bool pinned = false;     // camera lane: not scrolled
    };

    TimelineWidget();
    void SetMediaProvider(TimelineMediaProvider* p) { m_media = p ? p : &NullMediaProvider::Instance(); }

    // Toolbar row ([+Track] [Snap] [Ripple] | Split Delete Marker ... zoom slider [fit]).
    void DrawToolbar(EditorState& state);
    // Draws the widget into a child region of `size` (0 = remaining content region) and handles its input.
    void Draw(EditorState& state, ImVec2 size = ImVec2(0, 0));

    // Commands (also used by shortcuts and menus).
    void ZoomBy(EditorState& state, double factor, TimeUs anchor);   // anchor -1 = playhead
    void Fit(EditorState& state);
    void ScrollToPlayhead(EditorState& state);   // page by 80 % when the playhead left the visible range

    // Last frame's geometry (tests, preview linkage).
    const TimelineView& View() const { return m_view; }
    const std::vector<Lane>& Lanes() const { return m_lanes; }
    bool ClipRect(const EditorState& state, const project::Id& clip, ImVec2* min, ImVec2* max) const;
    float LanesTop() const { return m_lanes_top; }
    float RulerY() const { return m_ruler_y; }
    Mode CurrentMode() const { return m_mode; }
    bool WantsRedraw() const { return m_wants_redraw; }   // shimmer / drag autoscroll / playing
    int ClipsDrawn() const { return m_clips_drawn; }

    // Default lane height for a track (DIP, before track_scale).
    static float TrackHeight(const project::Project& p, const project::Track& t);

private:
    struct DragState {
        Zone zone = Zone::Body;
        project::Id clip;
        project::Id dup;                 // Ctrl+drag copy
        ImVec2 press_pos{};
        TimeUs press_t = 0;
        TimeUs orig_start = 0, orig_end = 0;
        project::Id orig_track;
        TimeUs slip_applied = 0;
        std::vector<std::pair<project::Id, TimeUs>> group;   // multi-selection move: id, original start
        TimeUs marker_t = 0;             // marker / camera key being dragged (current time)
        bool ctrl = false, alt = false, shift = false;
        bool was_selected = false;
        int track_index = -1;            // track reorder
        float scroll_grab = 0;           // scrollbar drag offset
    };

    void RebuildCache(const EditorState& state);
    void Layout(const EditorState& state, ImVec2 origin, ImVec2 size);
    void HandleInput(EditorState& state, bool hovered, bool active);
    void HandleDrag(EditorState& state);
    void HandleDrop(EditorState& state);
    void HandleWheel(EditorState& state);
    void DrawRuler(EditorState& state, ImDrawList* dl);
    void DrawLanes(EditorState& state, ImDrawList* dl);
    void DrawClip(EditorState& state, ImDrawList* dl, const project::Clip& c, const project::Track& t, const Lane& lane);
    void DrawCameraLane(EditorState& state, ImDrawList* dl, const Lane& lane);
    void DrawHeaders(EditorState& state, ImDrawList* dl);
    void DrawScrollbars(EditorState& state, ImDrawList* dl);
    void DrawContextMenus(EditorState& state);

    const Lane* LaneAt(float y) const;
    const project::Clip* ClipAt(const EditorState& state, const Lane& lane, TimeUs t) const;
    Zone ZoneAt(const project::Clip& c, float x) const;
    TimeUs SnapTime(const EditorState& state, TimeUs t, const std::vector<project::Id>& ignore) const;
    TimeUs SnapMove(const EditorState& state, TimeUs start, TimeUs dur, const std::vector<project::Id>& ignore) const;
    float ContentHeight() const;
    TimeUs ContentEnd(const EditorState& state) const;
    void SelectInRect(EditorState& state, ImVec2 a, ImVec2 b, bool add);
    bool IsSelected(const project::Id& id) const;

    TimelineMediaProvider* m_media;
    TimelineView m_view;
    std::vector<Lane> m_lanes;
    std::vector<std::string_view> m_sel_sorted;   // this frame's selected clip ids, sorted
    // Per-track clip indices sorted by start_us; rebuilt when the revision or clip vector changes.
    std::vector<std::vector<int>> m_sorted;
    uint64_t m_cache_revision = ~0ull;
    const void* m_cache_clips = nullptr;
    size_t m_cache_count = 0;

    ImVec2 m_origin{}, m_size{};
    float m_header_w = 0, m_ruler_h = 0, m_ruler_y = 0, m_lanes_top = 0, m_lanes_bottom = 0, m_hbar_h = 0;
    float m_content_h = 0;
    Mode m_mode = Mode::Idle;
    DragState m_drag;
    bool m_wants_redraw = false;
    int m_clips_drawn = 0;
    ImVec2 m_mouse{};

    // context menu targets
    project::Id m_ctx_clip;
    TimeUs m_ctx_t = 0;
    int m_ctx_track = -1;
    bool m_open_clip_menu = false, m_open_lane_menu = false, m_open_marker_menu = false, m_open_key_menu = false;
    bool m_open_rename = false;
    char m_rename_buf[128] = {};

    // drop preview
    bool m_drop_active = false;
    ImVec2 m_drop_min{}, m_drop_max{};
    bool m_drop_ok = false;
};

// Builds the clip a drop of `media` onto `track` creates at `start` (false when the media cannot go there).
bool MakeClipForTrack(const project::Project& p, const project::Media& m, const project::Track& t, TimeUs start,
                      project::Clip* out);
// Default annotation clip of `type` at `start` (4 s).
project::Clip MakeAnnotationClip(int type, const project::Id& track, TimeUs start);

} // namespace ct::ui
