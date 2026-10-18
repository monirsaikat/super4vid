// The timeline widget: layout, drawing (one ImDrawList, culled), interaction state machine, drop targets.
#include <ct/ui/panels/timeline_widget.h>

#include <ct/project/edit_ops.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ct::ui {

using namespace ct::project;
using namespace metrics;

NullMediaProvider& NullMediaProvider::Instance() {
    static NullMediaProvider p;
    return p;
}

namespace {

// End of the closest clip before `c` and start of the closest clip after it on c's track (no allocation).
void TrackNeighbours(const Project& p, const Clip& c, TimeUs* prev_end, TimeUs* next_start) {
    for (const Clip& o : p.clips) {
        if (o.track != c.track || o.id == c.id) continue;
        if (o.start_us < c.start_us) *prev_end = std::max(*prev_end, o.End());
        else *next_start = std::min(*next_start, o.start_us);
    }
}

ImU32 ToU32(const ColorF& c, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * alpha));
}

enum class ClipKind { Video, Camera, Audio, Annotation };

ClipKind KindOf(const Clip& c, const Track& t) {
    if (t.kind == Track::Kind::Annotations || c.IsAnnotation()) return ClipKind::Annotation;
    if (t.kind == Track::Kind::Audio) return ClipKind::Audio;
    if (c.source == Clip::Source::Camera) return ClipKind::Camera;
    return ClipKind::Video;
}

void KindColors(ClipKind k, ImU32* fill, ImU32* text) {
    switch (k) {
    case ClipKind::Camera: *fill = col::clip_camera; *text = col::clip_camera_text; break;
    case ClipKind::Audio: *fill = col::clip_audio; *text = col::clip_audio_text; break;
    case ClipKind::Annotation: *fill = col::clip_annotation; *text = col::clip_annotation_text; break;
    default: *fill = col::clip_video; *text = col::clip_video_text; break;
    }
}

const char* TrackIcon(const Project& p, const Track& t) {
    if (t.kind == Track::Kind::Annotations) return ICON_FONT;
    if (t.kind == Track::Kind::Audio) {
        for (const Clip& c : p.clips)
            if (c.track == t.id) return c.source == Clip::Source::Mic ? ICON_MICROPHONE : ICON_VOLUME;
        return ICON_VOLUME;
    }
    for (const Clip& c : p.clips)
        if (c.track == t.id) return c.source == Clip::Source::Camera ? ICON_CAMERA : ICON_MONITOR;
    return ICON_VIDEO;
}

const char* AnnotationTypeName(Annotation::Type t) {
    static const char* const kNames[] = {"Text", "Callout", "Arrow", "Rectangle", "Ellipse", "Highlight", "Blur", "Keystrokes"};
    const int i = static_cast<int>(t);
    return i >= 0 && i < 8 ? kNames[i] : "Annotation";
}

const char* ClipLabel(const Project& p, const Clip& c) {
    if (!c.name.empty()) return c.name.c_str();
    if (c.annotation) return c.annotation->text.empty() ? AnnotationTypeName(c.annotation->type) : c.annotation->text.c_str();
    if (const Media* m = p.FindMedia(c.media)) return cmd::MediaDisplayName(*m);
    return "Clip";
}

int AudioStreamIndex(const Media& m, Clip::Source s) {
    const char* want = s == Clip::Source::Mic ? "mic" : s == Clip::Source::System ? "system" : nullptr;
    if (!want) return 0;
    for (size_t i = 0; i < m.audio_streams.size(); ++i)
        if (m.audio_streams[i] == want) return static_cast<int>(i);
    return 0;
}

bool InRect(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y; }

// Header toggle buttons, right to left: [lock] [mute|hide].
struct HeaderButtons {
    ImVec2 lock_min, lock_max, vis_min, vis_max;
};
HeaderButtons HeaderButtonRects(float header_right, float lane_y, float lane_h) {
    const float s = Dp(22);
    const float y = lane_y + (lane_h - s) * 0.5f;
    HeaderButtons b;
    b.lock_min = ImVec2(header_right - Dp(8) - s, y);
    b.lock_max = b.lock_min + ImVec2(s, s);
    b.vis_min = ImVec2(b.lock_min.x - Dp(2) - s, y);
    b.vis_max = b.vis_min + ImVec2(s, s);
    return b;
}

} // namespace

TimelineWidget::TimelineWidget() : m_media(&NullMediaProvider::Instance()) {}

float TimelineWidget::TrackHeight(const Project& p, const Track& t) {
    if (t.height_px > 0) return static_cast<float>(t.height_px);
    switch (t.kind) {
    case Track::Kind::Audio: return kTlAudio;
    case Track::Kind::Annotations: return kTlAnnotation;
    default: break;
    }
    for (const Clip& c : p.clips)
        if (c.track == t.id) return c.source == Clip::Source::Camera ? kTlCamera : kTlVideo;
    return kTlVideo;
}

// --- cache / layout ---------------------------------------------------------------------------------------------

void TimelineWidget::RebuildCache(const EditorState& state) {
    const Project& p = state.project;
    if (m_cache_revision == state.revision && m_cache_clips == p.clips.data() && m_cache_count == p.clips.size() &&
        m_sorted.size() == p.tracks.size())
        return;
    m_cache_revision = state.revision;
    m_cache_clips = p.clips.data();
    m_cache_count = p.clips.size();
    m_sorted.resize(p.tracks.size());
    for (auto& v : m_sorted) v.clear();
    for (size_t i = 0; i < p.clips.size(); ++i) {
        const int ti = p.TrackIndex(p.clips[i].track);
        if (ti >= 0) m_sorted[static_cast<size_t>(ti)].push_back(static_cast<int>(i));
    }
    for (auto& v : m_sorted)
        std::sort(v.begin(), v.end(), [&](int a, int b) { return p.clips[static_cast<size_t>(a)].start_us < p.clips[static_cast<size_t>(b)].start_us; });
}

void TimelineWidget::Layout(const EditorState& state, ImVec2 origin, ImVec2 size) {
    const Project& p = state.project;
    m_origin = origin;
    m_size = size;
    m_header_w = Dp(kTlHeader);
    m_ruler_h = Dp(kTlRuler);
    m_hbar_h = Dp(12);
    m_ruler_y = origin.y;
    const float vbar_w = Dp(12);
    m_view.origin_x = origin.x + m_header_w;
    m_view.width = std::max(1.0f, size.x - m_header_w - vbar_w);
    m_view.pps = state.pps * Dp(1.0f);
    m_view.scroll_us = state.scroll_us;

    m_lanes.clear();
    float y = origin.y + m_ruler_h;
    Lane cam;
    cam.kind = Lane::Kind::CameraKeys;
    cam.y = y;
    cam.h = Dp(kTlCameraKf);
    cam.pinned = true;
    m_lanes.push_back(cam);
    y += cam.h + Dp(kTlGap);
    m_lanes_top = y;
    m_lanes_bottom = origin.y + size.y - m_hbar_h;

    const float gap = Dp(kTlGap);
    float ty = m_lanes_top - state.scroll_y;
    float content = 0;
    auto add = [&](int i) {
        Lane l;
        l.kind = Lane::Kind::Track;
        l.track = i;
        l.h = std::round(Dp(TrackHeight(p, p.tracks[static_cast<size_t>(i)])) * state.track_scale);
        l.y = ty;
        ty += l.h + gap;
        content += l.h + gap;
        m_lanes.push_back(l);
    };
    // Visual order (docs/05): overlays on top - annotation and video tracks top layer first, then audio.
    for (int i = static_cast<int>(p.tracks.size()) - 1; i >= 0; --i)
        if (p.tracks[static_cast<size_t>(i)].kind != Track::Kind::Audio) add(i);
    for (int i = 0; i < static_cast<int>(p.tracks.size()); ++i)
        if (p.tracks[static_cast<size_t>(i)].kind == Track::Kind::Audio) add(i);
    m_content_h = content;
}

float TimelineWidget::ContentHeight() const { return m_content_h; }

TimeUs TimelineWidget::ContentEnd(const EditorState& state) const {
    const TimeUs dur = state.project.Duration();
    const TimeUs vis = m_view.PxToUs(m_view.width);
    return std::max(dur + vis / 2, m_view.VisibleEnd());
}

const TimelineWidget::Lane* TimelineWidget::LaneAt(float y) const {
    for (const Lane& l : m_lanes) {
        if (!l.pinned && (y < m_lanes_top || y >= m_lanes_bottom)) continue;
        if (y >= l.y && y < l.y + l.h + Dp(kTlGap)) return &l;
    }
    return nullptr;
}

const Clip* TimelineWidget::ClipAt(const EditorState& state, const Lane& lane, TimeUs t) const {
    if (lane.kind != Lane::Kind::Track || lane.track < 0 || static_cast<size_t>(lane.track) >= m_sorted.size()) return nullptr;
    const auto& v = m_sorted[static_cast<size_t>(lane.track)];
    const auto& clips = state.project.clips;
    // Last clip with start <= t (with a small pixel tolerance so the trim handle just outside the end hits).
    const TimeUs tol = m_view.PxToUs(Dp(2));
    auto it = std::upper_bound(v.begin(), v.end(), t + tol, [&](TimeUs tt, int i) { return tt < clips[static_cast<size_t>(i)].start_us; });
    const Clip* best = nullptr;
    // Prefer the clip containing t; check the candidate and its predecessor (adjacent clips share an edge).
    for (int k = 0; k < 2 && it != v.begin(); ++k) {
        --it;
        const Clip& c = clips[static_cast<size_t>(*it)];
        if (t >= c.start_us - tol && t < c.End() + tol) {
            if (!best || (t >= c.start_us && t < c.End())) best = &c;
        }
    }
    return best;
}

TimelineWidget::Zone TimelineWidget::ZoneAt(const Clip& c, float x) const {
    const float x0 = m_view.X(c.start_us), x1 = m_view.X(c.End());
    const float handle = std::min(Dp(kTlTrimHandle), (x1 - x0) * 0.3f);
    if (x < x0 + handle) return Zone::TrimIn;
    if (x >= x1 - handle) return Zone::TrimOut;
    return Zone::Body;
}

bool TimelineWidget::ClipRect(const EditorState& state, const Id& id, ImVec2* mn, ImVec2* mx) const {
    const Clip* c = state.project.FindClip(id);
    if (!c) return false;
    const int ti = state.project.TrackIndex(c->track);
    for (const Lane& l : m_lanes) {
        if (l.kind != Lane::Kind::Track || l.track != ti) continue;
        *mn = ImVec2(m_view.X(c->start_us), l.y + Dp(kTlClipInset));
        *mx = ImVec2(m_view.X(c->End()), l.y + l.h - Dp(kTlClipInset));
        return true;
    }
    return false;
}

// Same candidates as ops::Snap (0, clip edges, markers, playhead) without building an ignore vector per call, and
// with the dragged marker excluded (it would otherwise snap to itself; review fix).
TimeUs TimelineWidget::SnapTime(const EditorState& state, TimeUs t, const SnapIgnore& ig) const {
    if (!state.snap || ImGui::GetIO().KeyAlt) return t;
    const TimeUs tol = m_view.PxToUs(Dp(kTlSnapPx));
    TimeUs best = t, best_d = tol + 1;
    auto consider = [&](TimeUs cand) {
        const TimeUs d = cand > t ? cand - t : t - cand;
        if (d <= tol && d < best_d) {
            best = cand;
            best_d = d;
        }
    };
    consider(0);
    consider(state.Playhead());
    for (const Clip& c : state.project.clips) {
        if (ig.clip && c.id == *ig.clip) continue;
        if (ig.selection && IsSelected(c.id)) continue;
        consider(c.start_us);
        consider(c.End());
    }
    for (const Marker& m : state.project.markers)
        if (m.t_us != ig.marker) consider(m.t_us);
    return best;
}

TimeUs TimelineWidget::SnapMove(const EditorState& state, TimeUs start, TimeUs dur, const SnapIgnore& ignore) const {
    const TimeUs s1 = SnapTime(state, start, ignore);
    const TimeUs e1 = SnapTime(state, start + dur, ignore);
    const TimeUs ds = s1 > start ? s1 - start : start - s1;
    const TimeUs de = e1 > start + dur ? e1 - (start + dur) : (start + dur) - e1;
    if (s1 != start && (e1 == start + dur || ds <= de)) return s1;
    if (e1 != start + dur) return e1 - dur;
    return start;
}

// --- commands ---------------------------------------------------------------------------------------------------

void TimelineWidget::ZoomBy(EditorState& state, double factor, TimeUs anchor) {
    if (anchor < 0) anchor = state.Playhead();
    const double old_pps = state.pps;
    const double new_pps = ClampPps(old_pps * factor);
    state.scroll_us = std::max<TimeUs>(0, ZoomScroll(state.scroll_us, anchor, old_pps, new_pps));
    state.pps = new_pps;
}

void TimelineWidget::Fit(EditorState& state) {
    const float w = m_view.width > 1 ? m_view.width / Dp(1.0f) : 1200.0f;
    state.pps = FitPps(state.project.Duration(), w);
    state.scroll_us = 0;
}

void TimelineWidget::ScrollToPlayhead(EditorState& state) {
    const TimeUs ph = state.Playhead();
    const TimeUs vis = m_view.PxToUs(m_view.width);
    const bool reverse = state.playback && state.playback->Playing() && state.playback->Rate() < 0;
    if (ph < state.scroll_us || ph > state.scroll_us + vis) {
        // Far away (seek, loop wrap): the playhead lands 10 % in from the edge it moves away from.
        state.scroll_us = std::max<TimeUs>(0, reverse ? ph - vis * 90 / 100 : ph - vis / 10);
    } else if (!reverse && ph > state.scroll_us + vis * 95 / 100) {
        state.scroll_us += vis * 80 / 100;
    } else if (reverse && state.scroll_us > 0 && ph < state.scroll_us + vis * 5 / 100) {
        state.scroll_us = std::max<TimeUs>(0, state.scroll_us - vis * 80 / 100);   // J: page left
    }
}

// --- toolbar ----------------------------------------------------------------------------------------------------

void TimelineWidget::DrawToolbar(EditorState& state) {
    ImGui::PushID("tl_toolbar");
    const float h = Dp(kControlCompact);
    if (IconButton("addtrack", ICON_ADD, str::kTipAddTrack, false, str::kTrack)) ImGui::OpenPopup("##addtrack");
    if (BeginPopover("##addtrack")) {
        if (ImGui::Selectable(str::kVideoTrack)) cmd::AddTrack(state, Track::Kind::Video);
        if (ImGui::Selectable(str::kAudioTrack)) cmd::AddTrack(state, Track::Kind::Audio);
        if (ImGui::Selectable(str::kCalloutTrack)) cmd::AddTrack(state, Track::Kind::Annotations);
        EndPopover();
    }
    ImGui::SameLine(0, Dp(4));
    if (IconButton("snap", ICON_CHECK, str::kTipSnap, state.snap, str::kSnap)) state.snap = !state.snap;
    ImGui::SameLine(0, Dp(4));
    if (IconButton("ripple", ICON_NEXT, str::kTipRipple, state.ripple, str::kRipple)) state.ripple = !state.ripple;
    ImGui::SameLine(0, Dp(10));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + Dp(6)), ImVec2(p.x, p.y + h - Dp(6)), col::border);
        ImGui::Dummy(ImVec2(Dp(1), h));
    }
    ImGui::SameLine(0, Dp(10));
    if (IconButton("split", ICON_CUT, str::kTipSplit, false, str::kSplit)) cmd::SplitAtPlayhead(state);
    ImGui::SameLine(0, Dp(4));
    ImGui::BeginDisabled(state.selection.Empty());
    if (IconButton("delete", ICON_DELETE, str::kTipDelete, false, str::kDelete)) cmd::DeleteSelection(state, state.ripple);
    ImGui::EndDisabled();
    ImGui::SameLine(0, Dp(4));
    if (IconButton("marker", ICON_FLAG, str::kTipMarker, false, str::kMarker)) cmd::AddMarkerAtPlayhead(state);

    // Right side: zoom out, slider, zoom in, fit.
    const float slider_w = Dp(140);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float total = h + Dp(4) + slider_w + Dp(4) + h + Dp(8) + Dp(52);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Dp(16), right - total));
    if (IconButton("zoomout", ICON_ZOOM_OUT, str::kTipZoomOut)) ZoomBy(state, 1 / 1.25, -1);
    ImGui::SameLine(0, Dp(4));
    // Logarithmic zoom slider.
    float z = static_cast<float>(std::log(state.pps / kMinPps) / std::log(kMaxPps / kMinPps));
    ImGui::SetNextItemWidth(slider_w);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Dp(12));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, Dp(3)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToVec4(col::bg2));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, Dp(999));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(999));
    if (ImGui::SliderFloat("##zoom", &z, 0, 1, "")) {
        const double np = kMinPps * std::pow(kMaxPps / kMinPps, static_cast<double>(z));
        ZoomBy(state, np / state.pps, -1);
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
    ItemTooltip(str::kTipZoom, "Ctrl+Wheel");
    ImGui::SameLine(0, Dp(4));
    if (IconButton("zoomin", ICON_ZOOM_IN, str::kTipZoomIn)) ZoomBy(state, 1.25, -1);
    ImGui::SameLine(0, Dp(8));
    if (Button(str::kFit, ButtonKind::Normal, ImVec2(Dp(52), h))) Fit(state);
    ItemTooltip(str::kTipFit, "F");
    ImGui::PopID();
}

// --- input ------------------------------------------------------------------------------------------------------

void TimelineWidget::SelectInRect(EditorState& state, ImVec2 a, ImVec2 b, bool add) {
    const ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y)), mx(std::max(a.x, b.x), std::max(a.y, b.y));
    if (!add) state.selection.Clear();
    const TimeUs t0 = m_view.T(mn.x), t1 = m_view.T(mx.x);
    const auto& clips = state.project.clips;
    for (const Lane& l : m_lanes) {
        if (l.kind != Lane::Kind::Track) {
            if (l.y < mx.y && l.y + l.h > mn.y) {
                for (const CameraKey& k : state.project.camera.keys)
                    if (k.t_us >= t0 && k.t_us <= t1) state.selection.camera_key = k.t_us;
            }
            continue;
        }
        if (l.y + l.h < mn.y || l.y > mx.y) continue;
        const auto& v = m_sorted[static_cast<size_t>(l.track)];
        auto it = std::lower_bound(v.begin(), v.end(), t0, [&](int i, TimeUs t) { return clips[static_cast<size_t>(i)].End() < t; });
        for (; it != v.end() && clips[static_cast<size_t>(*it)].start_us <= t1; ++it) state.selection.Add(clips[static_cast<size_t>(*it)].id);
    }
}

void TimelineWidget::HandleWheel(EditorState& state) {
    const ImGuiIO& io = ImGui::GetIO();
    const float wy = io.MouseWheel, wx = io.MouseWheelH;
    if (wy == 0 && wx == 0) return;
    if (io.KeyCtrl) {
        const TimeUs anchor = m_view.T(std::max(m_mouse.x, m_view.origin_x));
        ZoomBy(state, std::pow(1.2, static_cast<double>(wy)), anchor);
    } else if (io.KeyShift) {
        state.scroll_y -= wy * Dp(48);
    } else if (io.KeyAlt) {
        state.track_scale = std::clamp(state.track_scale * std::pow(1.1f, wy), 0.5f, 3.0f);
    } else {
        state.scroll_us -= m_view.PxToUs((wy - wx) * Dp(80));
    }
    state.scroll_us = std::max<TimeUs>(0, state.scroll_us);
}

void TimelineWidget::HandleInput(EditorState& state, bool hovered, bool active) {
    (void)active;
    const ImGuiIO& io = ImGui::GetIO();
    Project& p = state.project;
    const float lanes_right = m_view.origin_x + m_view.width;
    const bool in_ruler = m_mouse.y >= m_ruler_y && m_mouse.y < m_ruler_y + m_ruler_h;
    const bool in_header = m_mouse.x < m_view.origin_x;
    const bool in_hbar = m_mouse.y >= m_lanes_bottom;
    const bool in_vbar = m_mouse.x >= lanes_right && !in_ruler;

    if (m_mode == Mode::Idle && hovered) {
        const Lane* lane = LaneAt(m_mouse.y);
        if (lane && !in_header && lane->kind == Lane::Kind::Track) {
            if (const Clip* c = ClipAt(state, *lane, m_view.T(m_mouse.x)))
                if (ZoneAt(*c, m_mouse.x) != Zone::Body) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        HandleWheel(state);
    }
    if (m_mode == Mode::TrimIn || m_mode == Mode::TrimOut || m_mode == Mode::Slip) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // Right click: context menus.
    if (hovered && m_mode == Mode::Idle && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const TimeUs t = std::max<TimeUs>(0, m_view.T(m_mouse.x));
        m_ctx_t = t;
        if (in_ruler) {
            for (const Marker& mk : p.markers)
                if (std::fabs(m_view.X(mk.t_us) - m_mouse.x) <= Dp(6)) {
                    state.selection.Clear();
                    state.selection.marker = mk.t_us;
                    m_ctx_t = mk.t_us;
                    m_open_marker_menu = true;
                }
        } else if (const Lane* lane = LaneAt(m_mouse.y); lane && !in_header) {
            if (lane->kind == Lane::Kind::CameraKeys) {
                for (const CameraKey& k : p.camera.keys)
                    if (std::fabs(m_view.X(k.t_us) - m_mouse.x) <= Dp(6)) {
                        state.selection.Clear();
                        state.selection.camera_key = k.t_us;
                        m_ctx_t = k.t_us;
                        m_open_key_menu = true;
                    }
            } else if (const Clip* c = ClipAt(state, *lane, t)) {
                if (!state.selection.Has(c->id)) state.selection.Set(c->id);
                m_ctx_clip = c->id;
                m_open_clip_menu = true;
            } else {
                m_ctx_track = lane->track;
                m_open_lane_menu = true;
            }
        }
    }

    if (hovered && m_mode == Mode::Idle && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_drag = DragState();
        m_drag.press_pos = m_mouse;
        m_drag.press_t = m_view.T(m_mouse.x);
        m_drag.ctrl = io.KeyCtrl;
        m_drag.alt = io.KeyAlt;
        m_drag.shift = io.KeyShift;
        const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const TimeUs t = std::max<TimeUs>(0, m_drag.press_t);
        if (in_hbar && !in_header) {
            m_mode = Mode::HScroll;
            const double total = static_cast<double>(ContentEnd(state));
            const float thumb_x = m_view.origin_x + static_cast<float>(static_cast<double>(state.scroll_us) / total) * m_view.width;
            const float thumb_w = std::max(Dp(24), static_cast<float>(static_cast<double>(m_view.PxToUs(m_view.width)) / total) * m_view.width);
            m_drag.scroll_grab = (m_mouse.x >= thumb_x && m_mouse.x < thumb_x + thumb_w) ? m_mouse.x - thumb_x : thumb_w * 0.5f;
        } else if (in_vbar) {
            m_mode = Mode::VScroll;
            m_drag.scroll_grab = m_mouse.y;
            m_drag.marker_t = static_cast<TimeUs>(state.scroll_y);
        } else if (in_ruler && !in_header) {
            const Marker* hit = nullptr;
            for (const Marker& mk : p.markers)
                if (std::fabs(m_view.X(mk.t_us) - m_mouse.x) <= Dp(6)) hit = &mk;
            if (hit) {
                state.selection.Clear();
                state.selection.marker = hit->t_us;
                m_drag.marker_t = hit->t_us;
                if (dbl) {
                    std::snprintf(m_rename_buf, sizeof(m_rename_buf), "%s", hit->label.c_str());
                    m_ctx_t = hit->t_us;
                    m_open_rename = true;
                } else {
                    m_mode = Mode::MarkerDrag;
                }
            } else {
                m_mode = Mode::Scrub;
                state.playback->Pause();
                state.playback->Seek(t);
                state.playback->HintScrub(t);
            }
        } else if (in_header) {
            const Lane* lane = LaneAt(m_mouse.y);
            if (lane && lane->kind == Lane::Kind::Track) {
                const Track& tr = p.tracks[static_cast<size_t>(lane->track)];
                const HeaderButtons hb = HeaderButtonRects(m_view.origin_x, lane->y, lane->h);
                if (InRect(m_mouse, hb.lock_min, hb.lock_max)) {
                    cmd::ToggleTrackFlag(state, tr.id, cmd::TrackFlag::Lock);
                } else if (InRect(m_mouse, hb.vis_min, hb.vis_max)) {
                    cmd::ToggleTrackFlag(state, tr.id, tr.kind == Track::Kind::Audio ? cmd::TrackFlag::Mute : cmd::TrackFlag::Hide);
                } else {
                    m_mode = Mode::TrackReorder;
                    m_drag.track_index = lane->track;
                    m_drag.orig_track = tr.id;
                }
            }
        } else if (const Lane* lane = LaneAt(m_mouse.y)) {
            if (lane->kind == Lane::Kind::CameraKeys) {
                const CameraKey* hit = nullptr;
                for (const CameraKey& k : p.camera.keys)
                    if (std::fabs(m_view.X(k.t_us) - m_mouse.x) <= Dp(6)) hit = &k;
                if (hit) {
                    state.selection.Clear();
                    state.selection.camera_key = hit->t_us;
                    m_drag.marker_t = hit->t_us;
                    m_mode = Mode::CameraKeyDrag;
                } else if (dbl) {
                    const Camera::View v = p.camera.At(t);
                    CameraKey k;
                    k.t_us = t;
                    k.zoom = v.zoom > 1.01f ? v.zoom : p.camera.auto_params.zoom;
                    k.cx = ClampCentre(v.cx, k.zoom);
                    k.cy = ClampCentre(v.cy, k.zoom);
                    if (state.Edit(str::kUndoAddCameraKey, [&](Project& pp) { return ops::SetCameraKey(pp, k); })) {
                        state.selection.Clear();
                        state.selection.camera_key = t;
                    }
                } else {
                    state.selection.Clear();
                }
            } else if (const Clip* c = ClipAt(state, *lane, m_drag.press_t)) {
                m_drag.clip = c->id;
                m_drag.zone = ZoneAt(*c, m_mouse.x);
                m_drag.orig_start = c->start_us;
                m_drag.orig_end = c->End();
                m_drag.orig_track = c->track;
                m_drag.was_selected = state.selection.Has(c->id);
                if (m_drag.ctrl) {
                    state.selection.Toggle(c->id);
                } else if (m_drag.shift && !state.selection.clips.empty()) {
                    // Range on the same track: from the last selected clip on this track to this one.
                    TimeUs lo = c->start_us, hi = c->start_us;
                    for (const Id& id : state.selection.clips)
                        if (const Clip* s = p.FindClip(id); s && s->track == c->track) {
                            lo = std::min(lo, s->start_us);
                            hi = std::max(hi, s->start_us);
                        }
                    for (const Clip& o : p.clips)
                        if (o.track == c->track && o.start_us >= lo && o.start_us <= hi) state.selection.Add(o.id);
                } else if (!m_drag.was_selected) {
                    state.selection.Set(c->id);
                }
                m_mode = Mode::Pressed;
            } else {
                if (!m_drag.ctrl) state.selection.Clear();
                m_mode = Mode::RubberBand;
            }
        } else if (m_mouse.y >= m_lanes_top && m_mouse.y < m_lanes_bottom) {
            // Empty space below the last lane.
            if (!m_drag.ctrl) state.selection.Clear();
            m_mode = Mode::RubberBand;
        }
    }

    if (m_mode != Mode::Idle && ImGui::IsMouseDown(ImGuiMouseButton_Left)) HandleDrag(state);

    if (m_mode != Mode::Idle && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        switch (m_mode) {
        case Mode::Pressed:
            if (m_drag.was_selected && !m_drag.ctrl && !m_drag.shift) state.selection.Set(m_drag.clip);
            break;
        case Mode::RubberBand:
            if (std::fabs(m_mouse.x - m_drag.press_pos.x) > Dp(kTlDragThreshold) || std::fabs(m_mouse.y - m_drag.press_pos.y) > Dp(kTlDragThreshold))
                SelectInRect(state, m_drag.press_pos, m_mouse, m_drag.ctrl);
            break;
        default: break;
        }
        state.EndDrag();
        m_mode = Mode::Idle;
    }
}

void TimelineWidget::HandleDrag(EditorState& state) {
    Project& p = state.project;
    const float dx = m_mouse.x - m_drag.press_pos.x;
    const float dy = m_mouse.y - m_drag.press_pos.y;
    const bool moved = std::fabs(dx) > Dp(kTlDragThreshold) || std::fabs(dy) > Dp(kTlDragThreshold);
    const TimeUs dt = m_view.PxToUs(dx);
    const TimeUs frame = state.FrameUs();
    Err err;

    // Auto-scroll while dragging past the lanes' left/right edge.
    if (m_mode != Mode::HScroll && m_mode != Mode::VScroll && m_mode != Mode::TrackReorder && m_mode != Mode::Pressed) {
        const float right = m_view.origin_x + m_view.width;
        float over = 0;
        if (m_mouse.x > right - Dp(16)) over = m_mouse.x - (right - Dp(16));
        else if (m_mouse.x < m_view.origin_x + Dp(16) && m_mode != Mode::Scrub) over = m_mouse.x - (m_view.origin_x + Dp(16));
        if (over != 0) {
            const TimeUs step = m_view.PxToUs(std::clamp(over, -Dp(40), Dp(40)) * 0.5f);
            state.scroll_us = std::max<TimeUs>(0, state.scroll_us + step);
            m_drag.press_pos.x -= static_cast<float>(static_cast<double>(step) * m_view.pps / 1e6);
            m_view.scroll_us = state.scroll_us;
            m_wants_redraw = true;
        }
    }

    switch (m_mode) {
    case Mode::Pressed: {
        if (!moved) break;
        const Clip* c = p.FindClip(m_drag.clip);
        const Track* tr = c ? p.FindTrack(c->track) : nullptr;
        if (!c || !tr || tr->locked) break;
        if (m_drag.zone == Zone::TrimIn) {
            m_mode = Mode::TrimIn;
            state.BeginDrag(str::kUndoTrim);
        } else if (m_drag.zone == Zone::TrimOut) {
            m_mode = Mode::TrimOut;
            state.BeginDrag(str::kUndoTrim);
        } else if (m_drag.alt) {
            m_mode = Mode::Slip;
            state.BeginDrag(str::kUndoSlip);
        } else if (m_drag.ctrl) {
            m_mode = Mode::Duplicate;
            state.BeginDrag(str::kUndoDuplicate);
        } else {
            m_mode = Mode::Move;
            state.BeginDrag(str::kUndoMove);
            if (state.selection.clips.size() > 1 && state.selection.Has(m_drag.clip)) {
                for (const Id& id : state.selection.clips)
                    if (const Clip* s = p.FindClip(id)) m_drag.group.emplace_back(id, s->start_us);
            }
        }
        HandleDrag(state);
        break;
    }
    case Mode::Move: {
        const TimeUs dur = m_drag.orig_end - m_drag.orig_start;
        if (m_drag.group.size() > 1) {
            TimeUs min_start = INT64_MAX;
            for (const auto& g : m_drag.group) min_start = std::min(min_start, g.second);
            SnapIgnore sig;
            sig.selection = true;
            TimeUs cand = SnapMove(state, m_drag.orig_start + dt, dur, sig);
            TimeUs delta = cand - m_drag.orig_start;
            if (min_start + delta < 0) delta = -min_start;
            const Clip* c = p.FindClip(m_drag.clip);
            if (!c || c->start_us == m_drag.orig_start + delta) break;
            // Every clip moves by the same shift from where it is now: when shifting right the rightmost goes first
            // (and vice versa), so no clip of the group lands on another one's current position (review fix: the
            // order used to follow sign(delta), which failed when dragging back towards the origin).
            const TimeUs shift = delta - (c->start_us - m_drag.orig_start);
            auto group = m_drag.group;
            std::sort(group.begin(), group.end(), [&](const auto& a, const auto& b) { return shift > 0 ? a.second > b.second : a.second < b.second; });
            state.EditCopy(str::kUndoMove, [&](Project& pp) {
                for (const auto& g : group)
                    if (!ops::MoveClip(pp, g.first, g.second + delta, "", &err)) return false;
                return true;
            });
            break;
        }
        SnapIgnore sig;
        sig.clip = &m_drag.clip;
        TimeUs cand = SnapMove(state, std::max<TimeUs>(0, m_drag.orig_start + dt), dur, sig);
        cand = std::max<TimeUs>(0, cand);
        const Clip* c = p.FindClip(m_drag.clip);
        if (!c) break;
        Id track = c->track;
        if (const Lane* lane = LaneAt(m_mouse.y); lane && lane->kind == Lane::Kind::Track) {
            const Track& t = p.tracks[static_cast<size_t>(lane->track)];
            if (t.id != c->track && !t.locked && p.ClipFits(*c, t, nullptr)) track = t.id;
        }
        if (cand == c->start_us && track == c->track) break;
        const Id id = m_drag.clip;
        if (!state.Edit(str::kUndoMove, [&](Project& pp) { return ops::MoveClip(pp, id, cand, track, &err); }) && track != c->track) {
            // The other track has no room: keep the time move on the current track.
            const Id cur = p.FindClip(id)->track;
            state.Edit(str::kUndoMove, [&](Project& pp) { return ops::MoveClip(pp, id, cand, cur, &err); });
        }
        break;
    }
    case Mode::TrimIn: {
        SnapIgnore sig;
        sig.clip = &m_drag.clip;
        TimeUs ns = SnapTime(state, m_drag.orig_start + dt, sig);
        ns = std::min(ns, m_drag.orig_end - frame);
        const Clip* c = p.FindClip(m_drag.clip);
        if (!c) break;
        // Clamp analytically at the media start, the previous clip and 0 (review fix: was a bisection over up to 24
        // project copies per frame); the op's own rounding is absorbed by trying a few microseconds inwards.
        TimeUs prev_end = 0, next_start = INT64_MAX;
        TrackNeighbours(p, *c, &prev_end, &next_start);
        const TimeUs media_lo = c->start_us - static_cast<TimeUs>(std::floor(static_cast<double>(c->in_us) / c->speed));
        ns = std::max({ns, prev_end, media_lo, TimeUs(0)});
        if (ns == c->start_us) break;
        const Id id = m_drag.clip;
        for (int k = 0; k < 4 && ns < c->End(); ++k, ++ns)
            if (state.Edit(str::kUndoTrim, [&](Project& pp) { return ops::TrimIn(pp, id, ns, &err); })) break;
        break;
    }
    case Mode::TrimOut: {
        SnapIgnore sig;
        sig.clip = &m_drag.clip;
        TimeUs ne = SnapTime(state, m_drag.orig_end + dt, sig);
        ne = std::max(ne, m_drag.orig_start + frame);
        const Clip* c = p.FindClip(m_drag.clip);
        if (!c) break;
        TimeUs prev_end = 0, next_start = INT64_MAX;
        TrackNeighbours(p, *c, &prev_end, &next_start);
        const TimeUs max_out = p.MaxOutUs(*c);
        if (max_out < INT64_MAX / 4) {
            const TimeUs media_hi = c->start_us + static_cast<TimeUs>(std::floor(static_cast<double>(max_out - c->in_us) / c->speed));
            ne = std::min(ne, media_hi);
        }
        ne = std::min(ne, next_start);
        if (ne == c->End()) break;
        const Id id = m_drag.clip;
        for (int k = 0; k < 4 && ne > c->start_us; ++k, --ne)
            if (state.Edit(str::kUndoTrim, [&](Project& pp) { return ops::TrimOut(pp, id, ne, &err); })) break;
        break;
    }
    case Mode::Slip: {
        const Clip* c = p.FindClip(m_drag.clip);
        if (!c) break;
        const TimeUs target = -static_cast<TimeUs>(static_cast<double>(dt) * c->speed);
        const TimeUs delta = target - m_drag.slip_applied;
        if (delta == 0) break;
        const Id id = m_drag.clip;
        if (state.Edit(str::kUndoSlip, [&](Project& pp) { return ops::Slip(pp, id, delta, &err); })) m_drag.slip_applied = target;
        break;
    }
    case Mode::Duplicate: {
        const Clip* src = p.FindClip(m_drag.clip);
        if (!src) break;
        const TimeUs dur = m_drag.orig_end - m_drag.orig_start;
        Id ignore_dup = m_drag.dup.empty() ? m_drag.clip : m_drag.dup;
        SnapIgnore sig;
        sig.clip = &ignore_dup;
        TimeUs cand = std::max<TimeUs>(0, SnapMove(state, std::max<TimeUs>(0, m_drag.orig_start + dt), dur, sig));
        Id track = src->track;
        if (const Lane* lane = LaneAt(m_mouse.y); lane && lane->kind == Lane::Kind::Track) {
            const Track& t = p.tracks[static_cast<size_t>(lane->track)];
            if (!t.locked && p.ClipFits(*src, t, nullptr)) track = t.id;
        }
        if (m_drag.dup.empty()) {
            Clip copy = *src;
            copy.id.clear();
            copy.start_us = cand;
            copy.track = track;
            Id out;
            if (state.Edit(str::kUndoDuplicate, [&](Project& pp) { return ops::AddClip(pp, copy, &out, &err); })) {
                m_drag.dup = out;
                state.selection.Set(out);
            }
        } else if (const Clip* d = p.FindClip(m_drag.dup); d && (d->start_us != cand || d->track != track)) {
            const Id id = m_drag.dup;
            state.Edit(str::kUndoDuplicate, [&](Project& pp) { return ops::MoveClip(pp, id, cand, track, &err); });
        }
        break;
    }
    case Mode::Scrub: {
        const TimeUs t = std::max<TimeUs>(0, m_view.T(m_mouse.x));
        if (t != state.Playhead()) {
            state.playback->Seek(t);
            state.playback->HintScrub(t);
        }
        break;
    }
    case Mode::MarkerDrag: {
        if (!moved && !state.DragActive()) break;
        if (!state.DragActive()) state.BeginDrag(str::kUndoMoveMarker);
        SnapIgnore sig;
        sig.marker = m_drag.marker_t;   // never snap to itself (review fix)
        const TimeUs nt = std::max<TimeUs>(0, SnapTime(state, m_view.T(m_mouse.x), sig));
        if (nt == m_drag.marker_t) break;
        const TimeUs from = m_drag.marker_t;
        if (state.Edit(str::kUndoMoveMarker, [&](Project& pp) { return ops::MoveMarker(pp, from, nt); })) {
            m_drag.marker_t = nt;
            state.selection.marker = nt;
        }
        break;
    }
    case Mode::CameraKeyDrag: {
        if (!moved && !state.DragActive()) break;
        if (!state.DragActive()) state.BeginDrag(str::kUndoMoveCameraKey);
        const TimeUs nt = std::max<TimeUs>(0, SnapTime(state, m_view.T(m_mouse.x), SnapIgnore{}));
        if (nt == m_drag.marker_t) break;
        const TimeUs from = m_drag.marker_t;
        const bool ok = state.Edit(str::kUndoMoveCameraKey, [&](Project& pp) {
            CameraKey key{};
            bool found = false;
            for (const CameraKey& k : pp.camera.keys) {
                if (k.t_us == nt) return false;   // occupied: keep the last valid position
                if (k.t_us == from) {
                    key = k;
                    found = true;
                }
            }
            if (!found) return false;
            ops::RemoveCameraKey(pp, from);
            key.t_us = nt;
            key.generated = false;   // editing a generated key converts it to manual (docs/05)
            return ops::SetCameraKey(pp, key);
        });
        if (ok) {
            m_drag.marker_t = nt;
            state.selection.camera_key = nt;
        }
        break;
    }
    case Mode::TrackReorder: {
        if (!moved) break;
        const Lane* lane = LaneAt(m_mouse.y);
        if (!lane || lane->kind != Lane::Kind::Track) break;
        const int cur = p.TrackIndex(m_drag.orig_track);
        if (cur < 0 || lane->track == cur) break;
        if (p.tracks[static_cast<size_t>(lane->track)].kind == Track::Kind::Audio) {
            if (p.tracks[static_cast<size_t>(cur)].kind != Track::Kind::Audio) break;
        } else if (p.tracks[static_cast<size_t>(cur)].kind == Track::Kind::Audio) {
            break;
        }
        if (!state.DragActive()) state.BeginDrag(str::kUndoMoveTrack);
        const Id id = m_drag.orig_track;
        const int to = lane->track;
        state.Edit(str::kUndoMoveTrack, [&](Project& pp) { return ops::MoveTrack(pp, id, to); });
        break;
    }
    case Mode::HScroll: {
        const double total = static_cast<double>(ContentEnd(state));
        const double frac = static_cast<double>(m_mouse.x - m_drag.scroll_grab - m_view.origin_x) / m_view.width;
        state.scroll_us = std::max<TimeUs>(0, static_cast<TimeUs>(frac * total));
        break;
    }
    case Mode::VScroll: {
        const float visible = m_lanes_bottom - m_lanes_top;
        const float total = std::max(visible, m_content_h);
        state.scroll_y = static_cast<float>(m_drag.marker_t) + (m_mouse.y - m_drag.scroll_grab) * total / std::max(1.0f, visible);
        break;
    }
    default: break;
    }
}

void TimelineWidget::HandleDrop(EditorState& state) {
    m_drop_active = false;
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    const ImGuiPayload* media_pl = ImGui::AcceptDragDropPayload("CT_MEDIA", flags);
    const ImGuiPayload* ann_pl = media_pl ? nullptr : ImGui::AcceptDragDropPayload("CT_ANNOTATION", flags);
    const ImGuiPayload* pl = media_pl ? media_pl : ann_pl;
    if (pl) {
        Project& p = state.project;
        const Lane* lane = LaneAt(m_mouse.y);
        const TimeUs t0 = std::max<TimeUs>(0, m_view.T(m_mouse.x));
        Clip clip;
        bool ok = false;
        int track_idx = lane && lane->kind == Lane::Kind::Track ? lane->track : -1;
        TimeUs dur = 4 * kUsPerSecond;
        if (media_pl) {
            std::string id(static_cast<const char*>(pl->Data), strnlen(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize)));
            const Media* m = p.FindMedia(id);
            if (m) {
                // Lane under the mouse if compatible, else the first compatible track (a new one is created on drop).
                if (track_idx >= 0) ok = MakeClipForTrack(p, *m, p.tracks[static_cast<size_t>(track_idx)], t0, &clip);
                if (!ok)
                    for (size_t i = 0; i < p.tracks.size() && !ok; ++i)
                        if (MakeClipForTrack(p, *m, p.tracks[i], t0, &clip)) track_idx = static_cast<int>(i), ok = true;
                if (ok) dur = clip.Duration();
                else {
                    Track tmp;
                    tmp.kind = m->kind == Media::Kind::Audio ? Track::Kind::Audio : Track::Kind::Video;
                    ok = MakeClipForTrack(p, *m, tmp, t0, &clip);
                    track_idx = -1;
                    if (ok) dur = clip.Duration();
                }
            }
        } else {
            const int type = *static_cast<const int*>(pl->Data);
            if (track_idx < 0 || p.tracks[static_cast<size_t>(track_idx)].kind != Track::Kind::Annotations) {
                track_idx = -1;
                for (size_t i = 0; i < p.tracks.size(); ++i)
                    if (p.tracks[i].kind == Track::Kind::Annotations) {
                        track_idx = static_cast<int>(i);
                        break;
                    }
            }
            clip = MakeAnnotationClip(type, track_idx >= 0 ? p.tracks[static_cast<size_t>(track_idx)].id : Id(), t0);
            ok = true;
        }
        if (ok) {
            const TimeUs start = std::max<TimeUs>(0, SnapMove(state, t0, dur, {}));
            clip.start_us = start;
            m_drop_active = true;
            m_drop_ok = true;
            float ly = m_lanes_top, lh = Dp(kTlVideo);
            for (const Lane& l : m_lanes)
                if (l.kind == Lane::Kind::Track && l.track == track_idx) ly = l.y, lh = l.h;
            if (track_idx < 0) ly = m_lanes_top + m_content_h - state.scroll_y, lh = Dp(kTlVideo) * state.track_scale;
            m_drop_min = ImVec2(m_view.X(start), ly + Dp(1));
            m_drop_max = ImVec2(m_view.X(start + dur), ly + lh - Dp(1));
            if (track_idx >= 0) {
                // Free on the track? (no project copy per hover frame)
                clip.track = p.tracks[static_cast<size_t>(track_idx)].id;
                const TimeUs end = start + clip.Duration();
                m_drop_ok = !p.tracks[static_cast<size_t>(track_idx)].locked;
                for (const Clip& o : p.clips)
                    if (m_drop_ok && o.track == clip.track && o.start_us < end && start < o.End()) m_drop_ok = false;
            }
            if (pl->IsDelivery()) {
                Err err;
                const Track::Kind new_kind = clip.annotation ? Track::Kind::Annotations
                                           : (media_pl && p.FindMedia(clip.media) && p.FindMedia(clip.media)->kind == Media::Kind::Audio) ? Track::Kind::Audio
                                                                                                                                         : Track::Kind::Video;
                Id added;
                const bool done = state.EditCopy(str::kUndoAddClip, [&](Project& pp) {
                    Clip c = clip;
                    if (track_idx < 0) {
                        Id tid;
                        const char* name = new_kind == Track::Kind::Audio ? "Audio" : new_kind == Track::Kind::Annotations ? "Callouts" : "Video";
                        if (!ops::AddTrack(pp, new_kind, name, &tid, -1)) return false;
                        c.track = tid;
                    }
                    if (!ops::AddClip(pp, std::move(c), &added, &err)) return false;
                    // A video file's sound lives on an audio track (only audio-track clips play; DECISIONS P10).
                    if (const Clip* v = pp.FindClip(added)) cmd::AddLinkedAudioClip(pp, *v);
                    return true;
                });
                if (done) state.selection.Set(added);
                else state.last_error = err.message;
            }
        }
    }
    ImGui::EndDragDropTarget();
}

// --- drawing ----------------------------------------------------------------------------------------------------

void TimelineWidget::DrawRuler(EditorState& state, ImDrawList* dl) {
    const Project& p = state.project;
    const float x0 = m_view.origin_x, x1 = m_view.origin_x + m_view.width;
    const float y0 = m_ruler_y, y1 = m_ruler_y + m_ruler_h;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(m_origin.x + m_size.x, y1), col::bg1);
    dl->AddLine(ImVec2(x0, y1 - 0.5f), ImVec2(m_origin.x + m_size.x, y1 - 0.5f), col::border);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);

    if (p.ui.loop) {
        const float a = m_view.X(p.ui.loop->first), b = m_view.X(p.ui.loop->second);
        dl->AddRectFilled(ImVec2(a, y0 + Dp(2)), ImVec2(b, y1 - Dp(2)), col::accent_soft, Dp(3));
        dl->AddLine(ImVec2(a, y0 + Dp(2)), ImVec2(a, y1), col::accent, Dp(1.5f));
        dl->AddLine(ImVec2(b, y0 + Dp(2)), ImVec2(b, y1), col::accent, Dp(1.5f));
    }

    const TickUnit u = PickTickUnit(m_view.pps / Dp(1.0f), p.canvas.fps_num, p.canvas.fps_den, 80);
    const TimeUs vs = m_view.VisibleStart(), ve = m_view.VisibleEnd();
    const TimeUs minor = u.minor_div > 0 ? u.major_us / u.minor_div : 0;
    const bool draw_minor = minor > 0 && static_cast<double>(minor) * m_view.pps / 1e6 >= Dp(7);
    ImFont* font = Fonts::Get(FontRole::Label);
    const float fsize = Fonts::Size(FontRole::Label) * Dp(1.0f);
    if (draw_minor) {
        for (TimeUs t = (std::max<TimeUs>(0, vs) / minor) * minor; t <= ve; t += minor) {
            if (t % u.major_us == 0) continue;
            const float x = std::floor(m_view.X(t)) + 0.5f;
            dl->AddLine(ImVec2(x, y1 - Dp(4)), ImVec2(x, y1), col::text_faint);
        }
    }
    char buf[32];
    for (TimeUs t = (std::max<TimeUs>(0, vs) / u.major_us) * u.major_us; t <= ve + u.major_us; t += u.major_us) {
        const float x = std::floor(m_view.X(t)) + 0.5f;
        dl->AddLine(ImVec2(x, y1 - Dp(9)), ImVec2(x, y1), col::text_faint);
        FormatTick(buf, sizeof(buf), t, u, p.canvas.fps_num, p.canvas.fps_den);
        dl->AddText(font, fsize, ImVec2(x + Dp(4), y0 + Dp(3)), col::text_dim, buf);
    }

    // Markers: diamonds on the ruler.
    for (const Marker& mk : p.markers) {
        const float x = m_view.X(mk.t_us);
        if (x < x0 - Dp(10) || x > x1 + Dp(10)) continue;
        const float r = Dp(kTlDiamond) * 0.5f + Dp(1);
        const ImVec2 c(x, y1 - r - Dp(3));
        const ImVec2 pts[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
        dl->AddConvexPolyFilled(pts, 4, ToU32(mk.color));
        if (state.selection.marker && *state.selection.marker == mk.t_us) dl->AddPolyline(pts, 4, col::text, Dp(1.5f), ImDrawFlags_Closed);
        if (!mk.label.empty() && m_view.pps * 2 > Dp(40))
            dl->AddText(font, fsize, ImVec2(x + r + Dp(3), c.y - fsize * 0.5f), WithAlpha(ToU32(mk.color), 0.9f), mk.label.c_str());
    }
    dl->PopClipRect();

    // Corner: playhead timecode.
    dl->AddRectFilled(m_origin, ImVec2(x0, y1), col::bg1);
    dl->AddLine(ImVec2(m_origin.x, y1 - 0.5f), ImVec2(x0, y1 - 0.5f), col::border);
    FormatTimecode(buf, sizeof(buf), state.Playhead(), p.canvas.fps_num, p.canvas.fps_den);
    ImFont* mono = Fonts::Get(FontRole::Mono);
    const float msize = Fonts::Size(FontRole::Mono) * Dp(1.0f);
    dl->AddText(mono, msize, ImVec2(m_origin.x + Dp(12), y0 + (m_ruler_h - msize) * 0.5f), col::text, buf);
}

void TimelineWidget::DrawCameraLane(EditorState& state, ImDrawList* dl, const Lane& lane) {
    const Project& p = state.project;
    const float x0 = m_view.origin_x, x1 = m_view.origin_x + m_view.width;
    const float y0 = lane.y, y1 = lane.y + lane.h;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col::bg2_alt);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    const auto& keys = p.camera.keys;
    const TimeUs vs = m_view.VisibleStart() - m_view.PxToUs(Dp(10)), ve = m_view.VisibleEnd() + m_view.PxToUs(Dp(10));
    auto it = std::lower_bound(keys.begin(), keys.end(), vs, [](const CameraKey& k, TimeUs t) { return k.t_us < t; });
    if (it != keys.begin()) --it;
    const float cy = (y0 + y1) * 0.5f;
    // Zoom bands: where the camera is zoomed in between two keys.
    for (auto k = it; k != keys.end() && k->t_us <= ve; ++k) {
        auto n = k + 1;
        if (n == keys.end()) break;
        if (k->zoom > 1.01f && (n->zoom > 1.01f || k->ease == Ease::Hold)) {
            const float a = m_view.X(k->t_us), b = m_view.X(n->t_us);
            dl->AddRectFilled(ImVec2(a, cy - Dp(5)), ImVec2(b, cy + Dp(5)), WithAlpha(col::camera_kf, k->generated ? 0.12f : 0.22f), Dp(3));
        }
    }
    const float r = Dp(kTlDiamond) * 0.5f + Dp(1);
    for (auto k = it; k != keys.end() && k->t_us <= ve; ++k) {
        const float x = m_view.X(k->t_us);
        const ImVec2 pts[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
        const ImU32 c = k->generated ? WithAlpha(col::camera_kf, 0.55f) : col::camera_kf;
        dl->AddConvexPolyFilled(pts, 4, c);
        if (state.selection.camera_key && *state.selection.camera_key == k->t_us) dl->AddPolyline(pts, 4, col::text, Dp(1.5f), ImDrawFlags_Closed);
    }
    dl->PopClipRect();
}

void TimelineWidget::DrawClip(EditorState& state, ImDrawList* dl, const Clip& c, const Track& t, const Lane& lane) {
    const Project& p = state.project;
    const float lanes_l = m_view.origin_x, lanes_r = m_view.origin_x + m_view.width;
    const float inset = Dp(kTlClipInset);
    const float x0 = m_view.X(c.start_us), x1 = m_view.X(c.End());
    const float y0 = lane.y + inset, y1 = lane.y + lane.h - inset;
    // Draw extents clamped to the visible lanes (keeps vertex coordinates small at deep zoom).
    const float dx0 = std::max(x0, lanes_l - Dp(16)), dx1 = std::min(x1, lanes_r + Dp(16));
    if (dx1 <= dx0) return;
    ++m_clips_drawn;
    const ClipKind kind = KindOf(c, t);
    ImU32 fill, text;
    KindColors(kind, &fill, &text);
    if (c.color.a > 0) {
        fill = ToU32(ColorF{c.color.r, c.color.g, c.color.b, 1});
        text = Mix(fill, Rgb(0xFFFFFF), 0.75f);
    }
    const bool dim = t.hidden || (kind == ClipKind::Audio && (t.muted || c.audio.muted));
    const float alpha = dim ? 0.4f : 1.0f;
    const bool selected = IsSelected(c.id);
    const float w = x1 - x0;
    const float rounding = std::min(Dp(kRadiusClip), w * 0.5f);

    if (w < Dp(10)) {
        // Too narrow for content: one flat rect (keeps "fit" on 1,000 clips cheap), accent when selected.
        dl->AddRectFilled(ImVec2(dx0, y0), ImVec2(std::max(dx1 - (w >= Dp(3) ? 1.0f : 0.0f), dx0 + 1), y1),
                          selected ? col::accent_hover : WithAlpha(Mix(fill, col::bg1, 0.18f), alpha));
        return;
    }
    const ImVec2 a(dx0, y0), b(dx1, y1);
    const ImU32 body = WithAlpha(selected ? Mix(fill, Rgb(0xFFFFFF), 0.10f) : Mix(fill, col::bg1, 0.18f), alpha);
    dl->AddRectFilled(a, b, body, rounding);

    const float label_h = Dp(18);
    const bool has_label_row = (y1 - y0) >= Dp(30);
    const float content_y0 = has_label_row ? y0 + label_h : y0;
    dl->PushClipRect(ImVec2(std::max(dx0, lanes_l), y0), ImVec2(std::min(dx1, lanes_r), y1), true);

    // Content: filmstrip / waveform.
    if (kind == ClipKind::Video || kind == ClipKind::Camera) {
        const Media* m = p.FindMedia(c.media);
        if (m && (y1 - content_y0) > Dp(10)) {
            const float thumb_h = y1 - content_y0 - Dp(3);
            const float thumb_w = std::max(Dp(48), std::round(thumb_h * 16.0f / 9.0f));
            const float vis_l = std::max(x0, lanes_l), vis_r = std::min(x1, lanes_r);
            int first = static_cast<int>(std::floor((vis_l - x0) / thumb_w));
            const ImU32 ph = WithAlpha(Mix(fill, col::bg0, 0.45f), alpha);
            const ImU32 ph_edge = WithAlpha(Mix(fill, col::bg0, 0.25f), alpha);
            for (float sx = x0 + first * thumb_w; sx < vis_r; sx += thumb_w, ++first) {
                const ImVec2 ta(sx + Dp(1), content_y0), tb(std::min(sx + thumb_w - Dp(1), x1 - Dp(2)), content_y0 + thumb_h);
                if (tb.x <= ta.x) continue;
                ImTextureID tex = ImTextureID_Invalid;
                ImVec2 uv0(0, 0), uv1(1, 1);
                const MediaStatus st = m_media->ClipThumbnail(c, c.MediaTimeAt(m_view.T(sx)), tex, uv0, uv1);
                if (st == MediaStatus::Ready && tex != ImTextureID_Invalid) {
                    dl->AddImageRounded(ImTextureRef(tex), ta, tb, uv0, uv1, WithAlpha(Rgb(0xFFFFFF), alpha), Dp(3));
                } else if (st == MediaStatus::Loading) {
                    // Shimmer: a soft band sweeping across the placeholder.
                    dl->AddRectFilled(ta, tb, ph, Dp(3));
                    const float phase = static_cast<float>(std::fmod(ImGui::GetTime() * 1.2, 1.0));
                    const float bx = ta.x + (tb.x - ta.x + Dp(40)) * phase - Dp(40);
                    dl->AddRectFilledMultiColor(ImVec2(std::max(ta.x, bx), ta.y), ImVec2(std::min(tb.x, bx + Dp(40)), tb.y),
                                                WithAlpha(Rgb(0xFFFFFF), 0), WithAlpha(Rgb(0xFFFFFF), 0.08f * alpha),
                                                WithAlpha(Rgb(0xFFFFFF), 0.08f * alpha), WithAlpha(Rgb(0xFFFFFF), 0));
                    m_wants_redraw = true;
                } else {
                    dl->AddRectFilled(ta, tb, ph, Dp(3));
                    dl->AddRect(ta, tb, ph_edge, Dp(3));
                }
            }
        }
    } else if (kind == ClipKind::Audio) {
        const Media* m = p.FindMedia(c.media);
        const float wy0 = content_y0 + Dp(2), wy1 = y1 - Dp(3);
        const float mid = (wy0 + wy1) * 0.5f, amp = (wy1 - wy0) * 0.5f;
        const ImU32 wave = WithAlpha(col::waveform, 0.7f * alpha);
        size_t count = 0;
        int rate = 0;
        MediaStatus st = MediaStatus::Unavailable;
        const float* peaks = m ? m_media->PeaksMinMax(c.media, AudioStreamIndex(*m, c.source), &count, &rate, &st) : nullptr;
        const float vis_l = std::max(x0, lanes_l), vis_r = std::min(x1, lanes_r);
        if (peaks && count > 0 && rate > 0 && vis_r > vis_l) {
            const float step = 1.0f;
            const int cols = static_cast<int>((vis_r - vis_l) / step) + 1;
            const float base_gain = std::pow(10.0f, c.audio.gain_db / 20.0f);
            const bool gain_kf = c.keyframes.count("audio.gain_db") != 0;
            dl->PrimReserve(cols * 6, cols * 4);
            int used = 0;
            for (int i = 0; i < cols; ++i) {
                const float x = vis_l + i * step;
                const TimeUs tl0 = m_view.T(x), tl1 = m_view.T(x + step);
                const double m0 = static_cast<double>(c.MediaTimeAt(tl0)), m1 = static_cast<double>(c.MediaTimeAt(tl1));
                size_t i0 = static_cast<size_t>(std::max(0.0, m0 * rate / 1e6));
                size_t i1 = static_cast<size_t>(std::max(0.0, m1 * rate / 1e6));
                if (i1 <= i0) i1 = i0 + 1;
                if (i0 >= count) break;
                if (i1 > count) i1 = count;
                float lo = 0, hi = 0;
                for (size_t k = i0; k < i1; ++k) {
                    lo = std::min(lo, peaks[2 * k]);
                    hi = std::max(hi, peaks[2 * k + 1]);
                }
                float g = gain_kf ? std::pow(10.0f, c.Prop("audio.gain_db", tl0, c.audio.gain_db) / 20.0f) : base_gain;
                const TimeUs rel = tl0 - c.start_us;
                if (c.audio.fade_in_us > 0 && rel < c.audio.fade_in_us) g *= static_cast<float>(rel) / static_cast<float>(c.audio.fade_in_us);
                const TimeUs to_end = c.End() - tl0;
                if (c.audio.fade_out_us > 0 && to_end < c.audio.fade_out_us) g *= static_cast<float>(to_end) / static_cast<float>(c.audio.fade_out_us);
                g = std::clamp(g, 0.0f, 4.0f);
                const float top = mid - std::min(1.0f, hi * g) * amp;
                const float bot = mid - std::max(-1.0f, lo * g) * amp;
                dl->PrimRect(ImVec2(x, top), ImVec2(x + step, std::max(bot, top + Dp(1))), wave);
                ++used;
            }
            if (used < cols) {
                // Unused reserved primitives: degenerate (zero-area) quads.
                for (int i = used; i < cols; ++i) dl->PrimRect(ImVec2(vis_l, mid), ImVec2(vis_l, mid), 0);
            }
        } else {
            dl->AddLine(ImVec2(vis_l, mid), ImVec2(vis_r, mid), wave, Dp(1));
            if (st == MediaStatus::Loading) m_wants_redraw = true;
        }
    }

    // Fades: diagonal lines at the ends.
    if (c.audio.fade_in_us > 0) {
        const float fx = x0 + static_cast<float>(static_cast<double>(c.audio.fade_in_us) * m_view.pps / 1e6);
        dl->AddTriangleFilled(ImVec2(x0, y0), ImVec2(fx, y0), ImVec2(x0, y1), WithAlpha(col::bg0, 0.35f * alpha));
        dl->AddLine(ImVec2(x0, y1), ImVec2(fx, y0), WithAlpha(text, 0.9f * alpha), Dp(1.2f));
    }
    if (c.audio.fade_out_us > 0) {
        const float fx = x1 - static_cast<float>(static_cast<double>(c.audio.fade_out_us) * m_view.pps / 1e6);
        dl->AddTriangleFilled(ImVec2(fx, y0), ImVec2(x1, y0), ImVec2(x1, y1), WithAlpha(col::bg0, 0.35f * alpha));
        dl->AddLine(ImVec2(fx, y0), ImVec2(x1, y1), WithAlpha(text, 0.9f * alpha), Dp(1.2f));
    }

    // Label (sticky at the visible left edge), badges.
    if (w > Dp(18)) {
        const float lx = std::max(x0, lanes_l) + Dp(7);
        const float ly = has_label_row ? y0 + (label_h - ImGui::GetFontSize()) * 0.5f + Dp(1) : y0 + ((y1 - y0) - ImGui::GetFontSize()) * 0.5f;
        float label_right = std::min(x1, lanes_r) - Dp(6);
        char dur[24], spd[16];
        dur[0] = spd[0] = 0;
        ImFont* lf = Fonts::Get(FontRole::Label);
        const float ls = Fonts::Size(FontRole::Label) * Dp(1.0f);
        if (w > Dp(120)) {
            FormatDuration(dur, sizeof(dur), c.Duration());
            const float dw = lf ? lf->CalcTextSizeA(ls, FLT_MAX, 0, dur).x : 0;
            if (label_right - dw - Dp(8) > lx + Dp(40)) {
                dl->AddText(lf, ls, ImVec2(label_right - dw, ly + Dp(1)), WithAlpha(text, 0.7f * alpha), dur);
                label_right -= dw + Dp(8);
            }
        }
        if (c.speed != 1.0 && w > Dp(60)) {
            std::snprintf(spd, sizeof(spd), "%.4g\xC3\x97", c.speed);   // "1.5×"
            const float sw = lf ? lf->CalcTextSizeA(ls, FLT_MAX, 0, spd).x : 0;
            if (label_right - sw - Dp(12) > lx + Dp(30)) {
                const ImVec2 pa(label_right - sw - Dp(8), ly - Dp(1)), pb(label_right, ly + ls + Dp(2));
                dl->AddRectFilled(pa, pb, WithAlpha(col::bg0, 0.55f * alpha), Dp(999));
                dl->AddText(lf, ls, ImVec2(pa.x + Dp(4), ly + Dp(1)), WithAlpha(col::warn, alpha), spd);
                label_right = pa.x - Dp(6);
            }
        }
        if (label_right > lx + Dp(8)) {
            const char* label = ClipLabel(p, c);
            dl->PushClipRect(ImVec2(lx, y0), ImVec2(label_right, y1), true);
            const ImVec2 pos(lx, ly);
            ImGui::RenderTextEllipsis(dl, pos, ImVec2(label_right, ly + ImGui::GetFontSize()), label_right, label, nullptr, nullptr);
            dl->PopClipRect();
            (void)text;
        }
    }
    dl->PopClipRect();

    // Border: subtle by default, accent when selected.
    if (selected) {
        // Accent ring just outside the clip plus a light inner edge, so selection reads on every clip color.
        dl->AddRect(a - Dp(1, 1), b + Dp(1, 1), col::accent_hover, rounding + Dp(1), Dp(2));
        dl->AddRect(a + Dp(1, 1), b - Dp(1, 1), WithAlpha(Rgb(0xFFFFFF), 0.55f), std::max(0.0f, rounding - Dp(1)), Dp(1));
    } else {
        dl->AddRect(a, b, WithAlpha(Mix(fill, Rgb(0xFFFFFF), 0.25f), 0.55f * alpha), rounding, Dp(1));
    }
    // Trim handle hint on hover.
    if (m_mode == Mode::Idle && InRect(m_mouse, a, b)) {
        const Zone z = ZoneAt(c, m_mouse.x);
        if (z != Zone::Body) {
            const float hx = z == Zone::TrimIn ? x0 : x1;
            dl->AddRectFilled(ImVec2(hx - Dp(1.5f), y0 + Dp(4)), ImVec2(hx + Dp(1.5f), y1 - Dp(4)), col::text, Dp(2));
        }
    }
}

void TimelineWidget::DrawLanes(EditorState& state, ImDrawList* dl) {
    const Project& p = state.project;
    const float x0 = m_view.origin_x, x1 = m_view.origin_x + m_view.width;
    dl->PushClipRect(ImVec2(x0, m_lanes_top), ImVec2(x1, m_lanes_bottom), true);
    const auto& clips = p.clips;
    const TimeUs vs = m_view.VisibleStart() - m_view.PxToUs(Dp(16));
    const TimeUs ve = m_view.VisibleEnd() + m_view.PxToUs(Dp(16));
    int row = 0;
    for (const Lane& lane : m_lanes) {
        if (lane.kind != Lane::Kind::Track) continue;
        ++row;
        if (lane.y + lane.h < m_lanes_top || lane.y > m_lanes_bottom) continue;
        const Track& t = p.tracks[static_cast<size_t>(lane.track)];
        dl->AddRectFilled(ImVec2(x0, lane.y), ImVec2(x1, lane.y + lane.h), (row & 1) ? col::bg2 : col::bg2_alt);
        if (t.locked) {
            // Diagonal hatch for locked tracks.
            for (float hx = x0 - lane.h; hx < x1; hx += Dp(14))
                dl->AddLine(ImVec2(hx, lane.y + lane.h), ImVec2(hx + lane.h, lane.y), WithAlpha(col::bg0, 0.35f), Dp(1));
        }
        const auto& v = m_sorted[static_cast<size_t>(lane.track)];
        // Clips do not overlap on a track, so End() is sorted too: binary search the first one ending after vs.
        auto it = std::lower_bound(v.begin(), v.end(), vs, [&](int i, TimeUs tt) { return clips[static_cast<size_t>(i)].End() < tt; });
        for (; it != v.end(); ++it) {
            const Clip& c = clips[static_cast<size_t>(*it)];
            if (c.start_us > ve) break;
            DrawClip(state, dl, c, t, lane);
        }
    }
    // Transitions: hatched overlap region centred on the cut, with an icon.
    for (const Transition& tr : p.transitions) {
        const Clip* a = p.FindClip(tr.a);
        if (!a) continue;
        const int ti = p.TrackIndex(tr.track);
        for (const Lane& lane : m_lanes) {
            if (lane.kind != Lane::Kind::Track || lane.track != ti) continue;
            const float cx = m_view.X(a->End());
            const float half = static_cast<float>(static_cast<double>(tr.duration_us) * m_view.pps / 2e6);
            if (cx + half < x0 || cx - half > x1) break;
            const ImVec2 ra(cx - half, lane.y + Dp(3)), rb(cx + half, lane.y + lane.h - Dp(3));
            dl->AddRectFilled(ra, rb, WithAlpha(col::bg0, 0.55f), Dp(4));
            dl->PushClipRect(ra, rb, true);
            for (float hx = ra.x - lane.h; hx < rb.x; hx += Dp(6)) dl->AddLine(ImVec2(hx, rb.y), ImVec2(hx + lane.h, ra.y), WithAlpha(col::text, 0.18f), Dp(1));
            dl->PopClipRect();
            dl->AddRect(ra, rb, WithAlpha(col::text, 0.5f), Dp(4));
            break;
        }
    }
    // Marker lines across the lanes.
    for (const Marker& mk : p.markers) {
        const float x = m_view.X(mk.t_us);
        if (x < x0 || x > x1) continue;
        dl->AddLine(ImVec2(x, m_lanes_top), ImVec2(x, m_lanes_bottom), WithAlpha(ToU32(mk.color), 0.28f), Dp(1));
    }
    dl->PopClipRect();
}

void TimelineWidget::DrawHeaders(EditorState& state, ImDrawList* dl) {
    const Project& p = state.project;
    const float hx0 = m_origin.x, hx1 = m_view.origin_x;
    dl->AddRectFilled(ImVec2(hx0, m_ruler_y + m_ruler_h), ImVec2(hx1, m_origin.y + m_size.y), col::bg1);
    for (const Lane& lane : m_lanes) {
        const bool pinned = lane.pinned;
        if (!pinned && (lane.y + lane.h < m_lanes_top || lane.y > m_lanes_bottom)) continue;
        if (!pinned) dl->PushClipRect(ImVec2(hx0, m_lanes_top), ImVec2(hx1, m_lanes_bottom), true);
        const float y0 = lane.y, y1 = lane.y + lane.h;
        const float fs = ImGui::GetFontSize();
        if (lane.kind == Lane::Kind::CameraKeys) {
            dl->AddRectFilled(ImVec2(hx0, y0), ImVec2(hx1, y1), col::bg2_alt);
            dl->AddRectFilled(ImVec2(hx0, y0), ImVec2(hx0 + Dp(3), y1), col::camera_kf);
            ImFont* icon_font = Fonts::Get(FontRole::Icon);
            const float is = Fonts::Size(FontRole::Label) * Dp(1.0f);
            dl->AddText(icon_font, is, ImVec2(hx0 + Dp(14), y0 + (lane.h - is) * 0.5f - Dp(1)), col::text_dim, ICON_ZOOM_IN);
            ImFont* lf = Fonts::Get(FontRole::Label);
            dl->AddText(lf, is, ImVec2(hx0 + Dp(34), y0 + (lane.h - is) * 0.5f), col::text_dim, str::kCameraLane);
            const char* mode = p.camera.mode == Camera::Mode::Auto ? str::kAuto : str::kManual;
            const float mw = lf ? lf->CalcTextSizeA(is, FLT_MAX, 0, mode).x : 0;
            const ImVec2 pa(hx1 - Dp(10) - mw - Dp(12), y0 + Dp(6)), pb(hx1 - Dp(10), y1 - Dp(6));
            dl->AddRectFilled(pa, pb, p.camera.mode == Camera::Mode::Auto ? col::accent_soft : col::bg3, Dp(999));
            dl->AddText(lf, is, ImVec2(pa.x + Dp(6), y0 + (lane.h - is) * 0.5f), p.camera.mode == Camera::Mode::Auto ? col::accent_hover : col::text_dim, mode);
            continue;
        }
        const Track& t = p.tracks[static_cast<size_t>(lane.track)];
        dl->AddRectFilled(ImVec2(hx0, y0), ImVec2(hx1, y1), col::bg1);
        dl->AddLine(ImVec2(hx0, y1 + Dp(kTlGap) * 0.5f), ImVec2(hx1, y1 + Dp(kTlGap) * 0.5f), col::border);
        ImU32 kind_col = col::clip_video, kind_txt;
        const char* icon = TrackIcon(p, t);
        if (t.kind == Track::Kind::Audio) KindColors(ClipKind::Audio, &kind_col, &kind_txt);
        else if (t.kind == Track::Kind::Annotations) KindColors(ClipKind::Annotation, &kind_col, &kind_txt);
        else if (icon == ICON_CAMERA) KindColors(ClipKind::Camera, &kind_col, &kind_txt);
        dl->AddRectFilled(ImVec2(hx0, y0 + Dp(1)), ImVec2(hx0 + Dp(3), y1 - Dp(1)), kind_col);
        const bool reorder = m_mode == Mode::TrackReorder && m_drag.orig_track == t.id;
        if (reorder) dl->AddRectFilled(ImVec2(hx0, y0), ImVec2(hx1, y1), col::accent_soft);
        ImFont* icon_font = Fonts::Get(FontRole::Icon);
        const float is = Fonts::Size(FontRole::Icon) * Dp(1.0f);
        const float cy = y0 + lane.h * 0.5f;
        dl->AddText(icon_font, is, ImVec2(hx0 + Dp(12), cy - is * 0.5f), t.hidden || t.muted ? col::text_faint : col::text_dim, icon);
        const HeaderButtons hb = HeaderButtonRects(hx1, y0, lane.h);
        dl->PushClipRect(ImVec2(hx0, y0), ImVec2(hb.vis_min.x - Dp(4), y1), true);
        dl->AddText(ImVec2(hx0 + Dp(38), cy - fs * 0.5f), t.hidden || t.muted ? col::text_dim : col::text, t.name.c_str());
        dl->PopClipRect();
        auto toggle = [&](ImVec2 mn, ImVec2 mx, const char* glyph, bool on, ImU32 on_col) {
            const bool hov = m_mode == Mode::Idle && InRect(m_mouse, mn, mx);
            if (hov) dl->AddRectFilled(mn, mx, col::bg3, Dp(kRadiusControl));
            const float gs = Fonts::Size(FontRole::Label) * Dp(1.1f);
            dl->AddText(icon_font, gs, ImVec2(mn.x + (mx.x - mn.x - gs) * 0.5f, mn.y + (mx.y - mn.y - gs) * 0.5f - Dp(1)),
                        on ? on_col : hov ? col::text : col::text_faint, glyph);
        };
        if (t.kind == Track::Kind::Audio) toggle(hb.vis_min, hb.vis_max, t.muted ? ICON_MUTE : ICON_VOLUME, t.muted, col::warn);
        else toggle(hb.vis_min, hb.vis_max, t.hidden ? ICON_HIDE : ICON_VIEW, t.hidden, col::warn);
        toggle(hb.lock_min, hb.lock_max, t.locked ? ICON_LOCK : ICON_UNLOCK, t.locked, col::accent_hover);
        if (!pinned) dl->PopClipRect();
    }
    dl->AddLine(ImVec2(hx1 - 0.5f, m_ruler_y), ImVec2(hx1 - 0.5f, m_origin.y + m_size.y), col::border);
}

void TimelineWidget::DrawScrollbars(EditorState& state, ImDrawList* dl) {
    // Horizontal (time).
    const float x0 = m_view.origin_x, x1 = m_view.origin_x + m_view.width;
    const float y0 = m_lanes_bottom, y1 = m_origin.y + m_size.y;
    dl->AddRectFilled(ImVec2(m_origin.x, y0), ImVec2(m_origin.x + m_size.x, y1), col::bg1);
    const double total = static_cast<double>(ContentEnd(state));
    const float tw = std::max(Dp(24), static_cast<float>(static_cast<double>(m_view.PxToUs(m_view.width)) / total) * m_view.width);
    const float tx = x0 + static_cast<float>(static_cast<double>(state.scroll_us) / total) * m_view.width;
    const bool hh = m_mode == Mode::HScroll || (m_mode == Mode::Idle && m_mouse.y >= y0 && m_mouse.y < y1 && m_mouse.x >= x0 && m_mouse.x < x1);
    dl->AddRectFilled(ImVec2(std::max(x0, tx), y0 + Dp(3)), ImVec2(std::min(x1, tx + tw), y1 - Dp(3)), hh ? col::bg4 : col::bg3, Dp(999));
    // Vertical (lanes) when they do not fit.
    const float visible = m_lanes_bottom - m_lanes_top;
    if (m_content_h > visible + 1) {
        const float vx0 = x1, vx1 = m_origin.x + m_size.x;
        const float th = std::max(Dp(24), visible * visible / m_content_h);
        const float ty = m_lanes_top + (state.scroll_y / m_content_h) * visible;
        const bool vh = m_mode == Mode::VScroll || (m_mode == Mode::Idle && m_mouse.x >= vx0 && m_mouse.x < vx1 && m_mouse.y >= m_lanes_top);
        dl->AddRectFilled(ImVec2(vx0 + Dp(3), ty), ImVec2(vx1 - Dp(3), std::min(m_lanes_bottom, ty + th)), vh ? col::bg4 : col::bg3, Dp(999));
    }
}

void TimelineWidget::DrawContextMenus(EditorState& state) {
    if (m_open_clip_menu) ImGui::OpenPopup("##clipmenu");
    if (m_open_lane_menu) ImGui::OpenPopup("##lanemenu");
    if (m_open_marker_menu) ImGui::OpenPopup("##markermenu");
    if (m_open_key_menu) ImGui::OpenPopup("##keymenu");
    if (m_open_rename) ImGui::OpenPopup("##rename");
    m_open_clip_menu = m_open_lane_menu = m_open_marker_menu = m_open_key_menu = m_open_rename = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(8, 4));
    if (ImGui::BeginPopup("##clipmenu")) {
        const Clip* c = state.project.FindClip(m_ctx_clip);
        if (!c) {
            ImGui::CloseCurrentPopup();
        } else {
            const Id id = c->id;
            const bool inside = c->start_us < state.Playhead() && state.Playhead() < c->End();
            if (ImGui::MenuItem(str::kSplit, "S", false, inside)) cmd::SplitAtPlayhead(state);
            if (ImGui::MenuItem(str::kDelete, "Delete")) cmd::DeleteSelection(state, state.ripple);
            if (ImGui::MenuItem(str::kRippleDelete, "Shift+Delete")) cmd::DeleteSelection(state, true);
            if (ImGui::MenuItem(str::kDuplicate, "Ctrl+D")) cmd::DuplicateSelection(state);
            ImGui::Separator();
            if (ImGui::MenuItem(c->audio.muted ? str::kUnmute : str::kMute)) cmd::ToggleClipMute(state, id);
            if (ImGui::BeginMenu(str::kSpeed)) {
                static const double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0};
                for (double s : kSpeeds) {
                    char b[16];
                    std::snprintf(b, sizeof(b), "%.4g\xC3\x97", s);
                    if (ImGui::MenuItem(b, nullptr, std::fabs(c->speed - s) < 1e-9)) cmd::SetClipSpeed(state, id, s);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(str::kResetTransform)) cmd::ResetTransform(state, id);
            if (ImGui::MenuItem(str::kAddTransition, nullptr, false, cmd::NextAdjacent(state, id) != nullptr)) cmd::AddTransitionAfter(state, id);
            if (ImGui::BeginMenu(str::kColor)) {
                static const uint32_t kPalette[] = {0x2B6CB0, 0x6B46C1, 0x276749, 0x975A16, 0xB83280, 0xC53030, 0x2C7A7B, 0x4A5568};
                for (int i = 0; i < 8; ++i) {
                    ImGui::PushID(i);
                    const ImVec4 v = ToVec4(Rgb(kPalette[i]));
                    if (ImGui::ColorButton("##sw", v, ImGuiColorEditFlags_NoTooltip, Dp(20, 20))) {
                        cmd::SetClipColor(state, id, ColorF{v.x, v.y, v.z, 1});
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                    if (i % 4 != 3) ImGui::SameLine();
                }
                if (ImGui::MenuItem(str::kDefaultColor)) cmd::SetClipColor(state, id, ColorF{0, 0, 0, 0});
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##lanemenu")) {
        if (ImGui::MenuItem(str::kPasteHere, "Ctrl+V", false, !state.clipboard.empty())) {
            state.playback->Seek(m_ctx_t);
            cmd::PasteAtPlayhead(state);
        }
        if (ImGui::MenuItem(str::kAddMarkerHere)) {
            state.playback->Seek(m_ctx_t);
            cmd::AddMarkerAtPlayhead(state);
        }
        if (ImGui::BeginMenu(str::kAddTrackMenu)) {
            if (ImGui::MenuItem(str::kVideoTrack)) cmd::AddTrack(state, Track::Kind::Video);
            if (ImGui::MenuItem(str::kAudioTrack)) cmd::AddTrack(state, Track::Kind::Audio);
            if (ImGui::MenuItem(str::kCalloutTrack)) cmd::AddTrack(state, Track::Kind::Annotations);
            ImGui::EndMenu();
        }
        if (m_ctx_track >= 0 && static_cast<size_t>(m_ctx_track) < state.project.tracks.size()) {
            ImGui::Separator();
            if (ImGui::MenuItem(str::kRemoveTrack)) {
                const Id tid = state.project.tracks[static_cast<size_t>(m_ctx_track)].id;
                Err err;
                if (state.Edit(str::kRemoveTrack, [&](Project& pp) { return ops::RemoveTrack(pp, tid, &err); })) state.PruneSelection();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##markermenu")) {
        if (ImGui::MenuItem(str::kRename)) {
            for (const Marker& mk : state.project.markers)
                if (mk.t_us == m_ctx_t) std::snprintf(m_rename_buf, sizeof(m_rename_buf), "%s", mk.label.c_str());
            m_open_rename = true;
        }
        if (ImGui::MenuItem(str::kDelete)) cmd::RemoveMarker(state, m_ctx_t);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##keymenu")) {
        static const char* const kEases[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold"};
        const CameraKey* key = nullptr;
        for (const CameraKey& k : state.project.camera.keys)
            if (k.t_us == m_ctx_t) key = &k;
        if (key && ImGui::BeginMenu(str::kEasing)) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::MenuItem(kEases[i], nullptr, static_cast<int>(key->ease) == i)) {
                    CameraKey k = *key;
                    k.ease = static_cast<Ease>(i);
                    k.generated = false;
                    state.Edit(str::kUndoCameraKey, [&](Project& pp) { return ops::SetCameraKey(pp, k); });
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(str::kDelete)) {
            const TimeUs t = m_ctx_t;
            state.Edit(str::kUndoCameraKey, [&](Project& pp) { return ops::RemoveCameraKey(pp, t); });
            state.PruneSelection();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##rename")) {
        ImGui::TextUnformatted(str::kRenameMarker);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Dp(220));
        if (ImGui::InputText("##name", m_rename_buf, sizeof(m_rename_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            cmd::RenameMarker(state, m_ctx_t, m_rename_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

bool TimelineWidget::IsSelected(const Id& id) const {
    return std::binary_search(m_sel_sorted.begin(), m_sel_sorted.end(), std::string_view(id));
}

void TimelineWidget::Draw(EditorState& state, ImVec2 size) {
    m_wants_redraw = false;
    m_clips_drawn = 0;
    if (size.x <= 0 || size.y <= 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (size.x <= 0) size.x = avail.x;
        if (size.y <= 0) size.y = avail.y;
    }
    size.x = std::max(size.x, Dp(kTlHeader) + Dp(64));
    size.y = std::max(size.y, Dp(kTlRuler) + Dp(kTlCameraKf) + Dp(40));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ToVec4(col::bg1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##timeline", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    m_mouse = ImGui::GetIO().MousePos;

    RebuildCache(state);
    Layout(state, origin, size);
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool active = ImGui::IsItemActive();
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    }
    // Group-move snapping looks the selection up through m_sel_sorted: make it current before input.
    m_sel_sorted.clear();
    for (const Id& id : state.selection.clips) m_sel_sorted.emplace_back(id);
    std::sort(m_sel_sorted.begin(), m_sel_sorted.end());
    HandleDrop(state);
    const bool hover_input = hovered && !ImGui::GetDragDropPayload();
    HandleInput(state, hover_input, active);

    // Clamp the vertical scroll and re-layout with the post-input view.
    const float visible = (origin.y + size.y - m_hbar_h) - m_lanes_top;
    state.scroll_y = std::clamp(state.scroll_y, 0.0f, std::max(0.0f, m_content_h - visible));
    if (state.playback->Playing() && m_mode != Mode::Scrub) {
        m_view.scroll_us = state.scroll_us;
        ScrollToPlayhead(state);
    }
    Layout(state, origin, size);

    // Selection lookup for drawing: sorted views, O(log n) per clip (Select All on 1,000 clips stays cheap).
    m_sel_sorted.clear();
    for (const Id& id : state.selection.clips) m_sel_sorted.emplace_back(id);
    std::sort(m_sel_sorted.begin(), m_sel_sorted.end());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, origin + size, col::bg1);
    DrawLanes(state, dl);
    DrawCameraLane(state, dl, m_lanes.front());
    DrawRuler(state, dl);
    DrawHeaders(state, dl);
    DrawScrollbars(state, dl);

    // Playhead: 1 px `record` line across the lanes + 12 px handle on the ruler.
    {
        const float x = std::floor(m_view.X(state.Playhead())) + 0.5f;
        if (x >= m_view.origin_x - 1 && x <= m_view.origin_x + m_view.width + 1) {
            dl->PushClipRect(ImVec2(m_view.origin_x - Dp(7), m_ruler_y), ImVec2(m_view.origin_x + m_view.width + Dp(7), m_lanes_bottom), true);
            dl->AddLine(ImVec2(x, m_ruler_y + m_ruler_h - Dp(4)), ImVec2(x, m_lanes_bottom), col::record, Dp(1));
            const float hw = Dp(6), top = m_ruler_y + m_ruler_h - Dp(13);
            dl->AddRectFilled(ImVec2(x - hw, top - Dp(4)), ImVec2(x + hw, top + Dp(2)), col::accent, Dp(2), ImDrawFlags_RoundTop);
            dl->AddTriangleFilled(ImVec2(x - hw, top + Dp(2)), ImVec2(x + hw, top + Dp(2)), ImVec2(x, top + Dp(9)), col::accent);
            dl->PopClipRect();
        }
    }
    // Rubber band.
    if (m_mode == Mode::RubberBand) {
        const ImVec2 a(std::min(m_drag.press_pos.x, m_mouse.x), std::min(m_drag.press_pos.y, m_mouse.y));
        const ImVec2 b(std::max(m_drag.press_pos.x, m_mouse.x), std::max(m_drag.press_pos.y, m_mouse.y));
        dl->AddRectFilled(a, b, col::accent_soft, Dp(2));
        dl->AddRect(a, b, col::accent, Dp(2));
    }
    // Drop preview ghost.
    if (m_drop_active) {
        dl->PushClipRect(ImVec2(m_view.origin_x, m_lanes_top), ImVec2(m_view.origin_x + m_view.width, m_lanes_bottom), true);
        const ImU32 c = m_drop_ok ? col::accent : col::danger;
        dl->AddRectFilled(m_drop_min, m_drop_max, WithAlpha(c, 0.22f), Dp(kRadiusClip));
        dl->AddRect(m_drop_min, m_drop_max, c, Dp(kRadiusClip), Dp(1.5f));
        dl->PopClipRect();
    }
    if (m_mode != Mode::Idle) m_wants_redraw = true;

    DrawContextMenus(state);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// --- clip factories -----------------------------------------------------------------------------------------------

bool MakeClipForTrack(const Project& p, const Media& m, const Track& t, TimeUs start, Clip* out) {
    Clip c;
    c.media = m.id;
    c.track = t.id;
    c.start_us = start;
    c.in_us = 0;
    c.out_us = m.duration_us > 0 ? m.duration_us : 5 * kUsPerSecond;
    if (t.kind == Track::Kind::Annotations) return false;
    if (t.kind == Track::Kind::Audio) {
        if (m.kind == Media::Kind::Image) return false;
        if (m.kind == Media::Kind::Recording) {
            c.source = Clip::Source::System;
            for (const std::string& s : m.audio_streams)
                if (s == "mic") c.source = Clip::Source::Mic;
            if (m.audio_streams.empty()) return false;
        } else if (m.kind == Media::Kind::Video && m.audio_streams.empty()) {
            return false;
        }
    } else {
        if (m.kind == Media::Kind::Audio) return false;
        if (m.kind == Media::Kind::Recording) c.source = Clip::Source::Screen;
        if (m.kind == Media::Kind::Image) c.out_us = 5 * kUsPerSecond;
    }
    if (!t.id.empty() && !p.ClipFits(c, t, nullptr)) return false;
    *out = std::move(c);
    return true;
}

Clip MakeAnnotationClip(int type, const Id& track, TimeUs start) {
    Clip c;
    c.track = track;
    c.start_us = start;
    c.in_us = 0;
    c.out_us = 4 * kUsPerSecond;
    Annotation a;
    a.type = static_cast<Annotation::Type>(std::clamp(type, 0, 7));
    if (a.type == Annotation::Type::Text || a.type == Annotation::Type::Callout) a.text = a.type == Annotation::Type::Text ? "Title" : "Click here";
    c.annotation = a;
    return c;
}

} // namespace ct::ui
