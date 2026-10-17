// Properties panel: per-selection editors over EditorState::Edit (docs/05 section 4).
#include <ct/ui/panels/properties_panel.h>

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

namespace ct::ui {

using namespace ct::project;

namespace {

// Undo coalescing for the field submitted last (PropFloat & co. submit their drag field last).
void Coalesce(EditorState& s, const char* label) {
    if (ImGui::IsItemActivated()) s.BeginDrag(label);
    if (ImGui::IsItemDeactivated()) s.EndDrag();
}

// Applies fn(project, clip) to every selected clip: one op for a single clip, a copy-and-swap for several.
template <class Fn>
bool ApplyToClips(EditorState& s, const char* label, Fn&& fn) {
    const std::vector<Id> ids = s.selection.clips;
    if (ids.size() == 1) {
        return s.Edit(label, [&](Project& p) {
            const Clip* c = p.FindClip(ids[0]);
            return c && fn(p, *c);
        });
    }
    return s.EditCopy(label, [&](Project& p) {
        bool any = false;
        for (const Id& id : ids) {
            const Clip* c = p.FindClip(id);
            if (!c) continue;
            Clip copy = *c;   // fn may replace the clip in p
            any |= fn(p, copy);
        }
        return any;
    });
}

TimeUs RelTime(const EditorState& s, const Clip& c) {
    return std::clamp<TimeUs>(s.Playhead() - c.start_us, 0, c.Duration());
}

// ◇ toggle for one keyframeable property of the primary clip.
class ClipKeyHook final : public KeyframeHook {
public:
    ClipKeyHook(EditorState& s, const Clip& c, const char* prop) : m_s(s), m_id(c.id), m_prop(prop) {
        const auto it = c.keyframes.find(prop);
        m_track = it != c.keyframes.end() ? &it->second : nullptr;
        m_rel = RelTime(s, c);
        m_tol = s.FrameUs() / 2;
    }
    bool HasKey() const override { return m_track && m_track->Nearest(m_rel, m_tol) != nullptr; }
    bool Animated() const override { return m_track && !m_track->Empty(); }
    void Toggle(float v) override {
        const Keyframe<float>* k = m_track ? m_track->Nearest(m_rel, m_tol) : nullptr;
        const std::string prop = m_prop;
        const Id id = m_id;
        if (k) {
            const TimeUs kt = k->t_us;
            m_s.Edit(str::kUndoKeyframe, [&](Project& p) { return ops::RemoveKeyframe(p, id, prop, kt); });
        } else {
            const TimeUs rel = m_rel;
            m_s.Edit(str::kUndoKeyframe, [&](Project& p) { return ops::SetKeyframe(p, id, prop, rel, v, Ease::Linear); });
        }
    }

private:
    EditorState& m_s;
    Id m_id;
    const char* m_prop;
    const KeyTrack<float>* m_track;
    TimeUs m_rel, m_tol;
};

// Writes a keyframeable float: a key at the playhead while the property has keys, else the static value.
template <class SetStatic>
bool SetKeyed(EditorState& s, const char* prop, float v, SetStatic&& set_static) {
    return ApplyToClips(s, str::kUndoProperty, [&](Project& p, const Clip& c) {
        const auto it = c.keyframes.find(prop);
        if (it != c.keyframes.end() && !it->second.Empty()) {
            const Keyframe<float>* near_key = it->second.Nearest(RelTime(s, c), s.FrameUs() / 2);
            return ops::SetKeyframe(p, c.id, prop, RelTime(s, c), v, near_key ? near_key->ease : Ease::Linear);
        }
        return set_static(p, c, v);
    });
}

const char* const kClickNames[] = {"None", "Ripple", "Highlight", "Both"};
const char* const kKeyPosNames[] = {"Bottom centre", "Bottom left", "Bottom right", "Top centre"};
const char* const kKeyStyleNames[] = {"Pill", "Keycap"};
const char* const kCornerNames[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
const char* const kMaskNames[] = {"None", "Rounded", "Circle"};
const char* const kAnnTypes[] = {"Text", "Callout", "Arrow", "Rectangle", "Ellipse", "Highlight", "Blur", "Keystrokes"};
const char* const kAnimNames[] = {"None", "Fade", "Pop", "Slide"};
const char* const kTransKinds[] = {"Crossfade", "Dip to color", "Slide"};
const char* const kDirNames[] = {"Left", "Right", "Up", "Down"};
const char* const kEaseNames[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Hold"};
const char* const kBgKinds[] = {"Solid", "Gradient", "Image"};
const char* const kSourceNames[] = {"Screen", "Camera", "Microphone", "System audio", "Default"};

struct CanvasPreset { const char* name; int w, h; };
const CanvasPreset kCanvasPresets[] = {
    {"16:9 1080p", 1920, 1080}, {"16:9 1440p", 2560, 1440}, {"16:9 4K", 3840, 2160},
    {"9:16 vertical", 1080, 1920}, {"1:1 square", 1080, 1080}, {"4:5 portrait", 1080, 1350},
};
const int kFpsValues[] = {24, 25, 30, 50, 60};

void SectionGap() { ImGui::Dummy(Dp(0, 6)); }

void HintText(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_faint));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

} // namespace

void PropertiesPanel::Draw(EditorState& state) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(8, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Dp(8, 4));
    state.PruneSelection();
    if (!state.selection.clips.empty()) {
        DrawClipSections(state);
    } else if (state.selection.marker) {
        DrawMarker(state);
    } else if (state.selection.camera_key) {
        DrawCameraKey(state);
        DrawCameraSection(state);
    } else {
        DrawProjectSections(state);
    }
    ImGui::PopStyleVar(2);
}

void PropertiesPanel::DrawMarker(EditorState& state) {
    const TimeUs t = *state.selection.marker;
    const Marker* mk = nullptr;
    for (const Marker& m : state.project.markers)
        if (m.t_us == t) mk = &m;
    if (!mk) return;
    if (!SectionHeader(str::kSecMarker)) return;
    std::string label = mk->label;
    if (PropText(str::kLabel, label)) cmd::RenameMarker(state, t, label.c_str());
    TimeUs nt = t;
    if (PropTime(str::kTime, nt, 0, INT64_MAX / 4) && nt != t) {
        if (state.Edit(str::kUndoMoveMarker, [&](Project& p) { return ops::MoveMarker(p, t, nt); })) state.selection.marker = nt;
    }
    Coalesce(state, str::kUndoMoveMarker);
    ColorF c = mk->color;
    if (PropColor(str::kColorA, c, false)) {
        state.Edit(str::kUndoProperty, [&](Project& p) {
            for (Marker& m : p.markers)
                if (m.t_us == t) {
                    m.color = c;
                    return true;
                }
            return false;
        });
    }
    Coalesce(state, str::kUndoProperty);
}

void PropertiesPanel::DrawCameraKey(EditorState& state) {
    const TimeUs t = *state.selection.camera_key;
    const CameraKey* key = nullptr;
    for (const CameraKey& k : state.project.camera.keys)
        if (k.t_us == t) key = &k;
    if (!key) return;
    if (!SectionHeader(str::kSecCameraKey)) return;
    CameraKey k = *key;
    bool changed = false;
    TimeUs nt = k.t_us;
    if (PropTime(str::kTime, nt, 0, INT64_MAX / 4) && nt != t) {
        const bool ok = state.Edit(str::kUndoCameraKey, [&](Project& p) {
            for (const CameraKey& o : p.camera.keys)
                if (o.t_us == nt) return false;
            CameraKey moved = k;
            moved.t_us = nt;
            moved.generated = false;
            ops::RemoveCameraKey(p, t);
            return ops::SetCameraKey(p, moved);
        });
        if (ok) state.selection.camera_key = nt;
        return;
    }
    Coalesce(state, str::kUndoCameraKey);
    changed |= PropFloat(str::kZoom, k.zoom, 1.0f, 8.0f, "%.2f", "\xC3\x97", nullptr, 1.0f, 0.01f);
    Coalesce(state, str::kUndoCameraKey);
    changed |= PropFloat(str::kCentreX, k.cx, 0.0f, 1.0f, "%.3f", nullptr, nullptr, 0.5f, 0.002f);
    Coalesce(state, str::kUndoCameraKey);
    changed |= PropFloat(str::kCentreY, k.cy, 0.0f, 1.0f, "%.3f", nullptr, nullptr, 0.5f, 0.002f);
    Coalesce(state, str::kUndoCameraKey);
    int ease = static_cast<int>(k.ease);
    if (PropEnum(str::kEase, ease, kEaseNames, 5)) {
        k.ease = static_cast<Ease>(ease);
        changed = true;
    }
    if (key->generated) HintText(str::kGenerated);
    if (changed) {
        k.cx = ClampCentre(k.cx, k.zoom);
        k.cy = ClampCentre(k.cy, k.zoom);
        k.generated = false;
        state.Edit(str::kUndoCameraKey, [&](Project& p) { return ops::SetCameraKey(p, k); });
    }
    SectionGap();
}

void PropertiesPanel::DrawCameraSection(EditorState& state) {
    Camera& cam = state.project.camera;
    char right[32];
    std::snprintf(right, sizeof(right), "%zu keys", cam.keys.size());
    if (!SectionHeader(str::kSecCamera, true, right)) return;
    int mode = cam.mode == Camera::Mode::Auto ? 0 : 1;
    const char* const modes[] = {str::kCameraAuto, str::kCameraManual};
    PropRowBegin(str::kMode);
    if (SegmentedControl("##cammode", &mode, modes, 2)) cmd::SetCameraMode(state, mode == 0 ? Camera::Mode::Auto : Camera::Mode::Manual);
    AutoZoomParams ap = cam.auto_params;
    bool changed = false;
    if (mode == 0) {
        changed |= PropFloat(str::kZoom, ap.zoom, 1.0f, 6.0f, "%.2f", "\xC3\x97", nullptr, 2.0f, 0.01f);
        Coalesce(state, str::kUndoCamera);
        changed |= PropTime(str::kMinHold, ap.min_hold_us, 100'000, 10'000'000);
        Coalesce(state, str::kUndoCamera);
        changed |= PropInt(str::kEase, ap.ease_ms, 50, 3000, "ms", 650);
        Coalesce(state, str::kUndoCamera);
        changed |= PropToggle(str::kFollow, ap.follow);
        if (ap.follow) {
            changed |= PropFloat(str::kDeadzone, ap.follow_deadzone, 0.0f, 0.5f, "%.2f", nullptr, nullptr, 0.15f, 0.002f);
            Coalesce(state, str::kUndoCamera);
        }
        changed |= PropToggle(str::kOnClick, ap.on_click);
        changed |= PropToggle(str::kOnTyping, ap.on_typing);
        if (changed) {
            state.Edit(str::kUndoCamera, [&](Project& p) {
                p.camera.auto_params = ap;
                return true;
            });
        }
        ImGui::Dummy(Dp(0, 2));
        ImGui::BeginDisabled(!on_regenerate);
        if (Button(str::kRegenerate, ButtonKind::Primary, ImVec2(-FLT_MIN, Dp(28))) && on_regenerate) on_regenerate(state);
        ImGui::EndDisabled();
        ItemTooltip(str::kRegenerateHint);
    }
    // Key list.
    ImGui::Dummy(Dp(0, 2));
    if (cam.keys.empty()) {
        HintText(str::kNoKeys);
    } else if (ImGui::BeginChild("##keys", ImVec2(-FLT_MIN, Dp(std::min(180.0f, 26.0f * cam.keys.size() + 8))), ImGuiChildFlags_Borders)) {
        TimeUs remove = -1;
        for (const CameraKey& k : cam.keys) {
            ImGui::PushID(static_cast<int>(k.t_us % 0x7FFFFFFF));
            char t[24], line[64];
            FormatDuration(t, sizeof(t), k.t_us);
            std::snprintf(line, sizeof(line), "%s   %.2f\xC3\x97%s", t, k.zoom, k.generated ? "   auto" : "");
            const bool sel = state.selection.camera_key && *state.selection.camera_key == k.t_us;
            if (ImGui::Selectable(line, sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, Dp(20)))) {
                state.selection.Clear();
                state.selection.camera_key = k.t_us;
                state.playback->Seek(k.t_us);
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - Dp(22));
            if (IconButton("del", ICON_DELETE, nullptr, false, nullptr, Dp(20, 20))) remove = k.t_us;
            ImGui::PopID();
        }
        if (remove >= 0) {
            state.Edit(str::kUndoCameraKey, [&](Project& p) { return ops::RemoveCameraKey(p, remove); });
            state.PruneSelection();
        }
    }
    if (!cam.keys.empty()) ImGui::EndChild();
    SectionGap();
}

void PropertiesPanel::DrawProjectSections(EditorState& state) {
    Project& pr = state.project;
    if (SectionHeader(str::kSecProject)) {
        std::string name = pr.name;
        if (PropText(str::kName, name)) {
            state.Edit(str::kUndoProperty, [&](Project& p) {
                p.name = name;
                return true;
            });
        }
        Coalesce(state, str::kUndoProperty);
        Canvas cv = pr.canvas;
        int preset = -1;
        for (int i = 0; i < 6; ++i)
            if (kCanvasPresets[i].w == cv.width && kCanvasPresets[i].h == cv.height) preset = i;
        const char* names[7];
        for (int i = 0; i < 6; ++i) names[i] = kCanvasPresets[i].name;
        names[6] = "Custom";
        int sel = preset < 0 ? 6 : preset;
        bool changed = false;
        if (PropEnum(str::kPreset, sel, names, 7) && sel < 6) {
            cv.width = kCanvasPresets[sel].w;
            cv.height = kCanvasPresets[sel].h;
            changed = true;
        }
        char size[32];
        std::snprintf(size, sizeof(size), "%d \xC3\x97 %d", cv.width, cv.height);
        PropValue(str::kSize, size);
        int fps_i = 4;
        const char* fps_names[] = {"24 fps", "25 fps", "30 fps", "50 fps", "60 fps"};
        for (int i = 0; i < 5; ++i)
            if (cv.fps_den == 1 && cv.fps_num == kFpsValues[i]) fps_i = i;
        if (PropEnum(str::kFps, fps_i, fps_names, 5)) {
            cv.fps_num = kFpsValues[fps_i];
            cv.fps_den = 1;
            changed = true;
        }
        if (changed) {
            Err err;
            if (!state.Edit(str::kUndoCanvas, [&](Project& p) { return ops::SetCanvas(p, cv, &err); })) state.last_error = err.message;
        }
        SectionGap();
    }
    if (SectionHeader(str::kSecBackground)) {
        Canvas cv = pr.canvas;
        bool changed = false;
        int kind = static_cast<int>(cv.background.kind);
        PropRowBegin(str::kKind);
        if (SegmentedControl("##bgkind", &kind, kBgKinds, 3)) {
            cv.background.kind = static_cast<Background::Kind>(kind);
            changed = true;
        }
        changed |= PropColor(str::kColorA, cv.background.color_a, false);
        Coalesce(state, str::kUndoCanvas);
        if (cv.background.kind == Background::Kind::Gradient) {
            changed |= PropColor(str::kColorB, cv.background.color_b, false);
            Coalesce(state, str::kUndoCanvas);
            changed |= PropFloat(str::kAngle, cv.background.angle_deg, 0, 360, "%.0f", "\xC2\xB0", nullptr, 135, 0.5f);
            Coalesce(state, str::kUndoCanvas);
        }
        changed |= PropInt(str::kPadding, cv.padding_px, 0, 600, "px", 96);
        Coalesce(state, str::kUndoCanvas);
        changed |= PropInt(str::kCornerRadius, cv.corner_radius_px, 0, 200, "px", 18);
        Coalesce(state, str::kUndoCanvas);
        changed |= PropToggle(str::kShadow, cv.shadow.enabled);
        if (cv.shadow.enabled) {
            changed |= PropFloat(str::kBlur, cv.shadow.blur_px, 0, 200, "%.0f", "px", nullptr, 48, 0.25f);
            Coalesce(state, str::kUndoCanvas);
            changed |= PropFloat(str::kOffsetY, cv.shadow.offset_y_px, -100, 100, "%.0f", "px", nullptr, 24, 0.25f);
            Coalesce(state, str::kUndoCanvas);
            float op = cv.shadow.opacity * 100;
            if (PropFloat(str::kOpacity, op, 0, 100, "%.0f", "%", nullptr, 45, 0.25f)) {
                cv.shadow.opacity = op / 100;
                changed = true;
            }
            Coalesce(state, str::kUndoCanvas);
        }
        if (changed) {
            Err err;
            state.Edit(str::kUndoCanvas, [&](Project& p) { return ops::SetCanvas(p, cv, &err); });
        }
        SectionGap();
    }
    DrawCameraSection(state);
}

void PropertiesPanel::DrawClipSections(EditorState& state) {
    Project& pr = state.project;
    const Clip* cp = pr.FindClip(state.selection.clips.front());
    if (!cp) return;
    const Clip c = *cp;   // copy: edits below may reallocate the clip vector
    const Track* tr = pr.FindTrack(c.track);
    const bool annotation = c.IsAnnotation();
    const bool audio_track = tr && tr->kind == Track::Kind::Audio;
    const bool visual = !audio_track;
    const bool screen = c.source == Clip::Source::Screen;
    const bool camera = c.source == Clip::Source::Camera;
    const size_t n = state.selection.clips.size();
    const TimeUs now = state.Playhead();

    if (n > 1) {
        char b[48];
        std::snprintf(b, sizeof(b), "%zu %s", n, str::kMultiSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextUnformatted(b);
        ImGui::PopStyleColor();
    }

    // Clip
    char range[48];
    {
        char a[24], b[24];
        std::snprintf(range, sizeof(range), "%s - %s", FormatDuration(a, sizeof(a), c.start_us), FormatDuration(b, sizeof(b), c.End()));
    }
    if (SectionHeader(str::kSecClip, true, range)) {
        if (n == 1) {
            std::string name = c.name;
            if (PropText(str::kName, name)) {
                state.Edit(str::kUndoProperty, [&](Project& p) {
                    Clip* pc = p.FindClip(c.id);
                    if (!pc) return false;
                    pc->name = name;
                    return true;
                });
            }
            Coalesce(state, str::kUndoProperty);
            TimeUs start = c.start_us;
            if (PropTime(str::kStart, start, 0, INT64_MAX / 4) && start != c.start_us) {
                Err err;
                if (!state.Edit(str::kUndoMove, [&](Project& p) { return ops::MoveClip(p, c.id, start, "", &err); })) state.last_error = err.message;
            }
            Coalesce(state, str::kUndoMove);
        }
        char dur[24];
        PropValue(str::kDuration, FormatDuration(dur, sizeof(dur), c.Duration()));
        if (!annotation) {
            float speed = static_cast<float>(c.speed);
            if (PropFloat(str::kSpeedProp, speed, static_cast<float>(kMinSpeed), static_cast<float>(kMaxSpeed), "%.2f", "\xC3\x97", nullptr, 1.0f, 0.005f)) {
                Err err;
                ApplyToClips(state, "Change speed", [&](Project& p, const Clip& k) { return ops::SetSpeed(p, k.id, speed, &err); });
            }
            Coalesce(state, "Change speed");
            PropValue(str::kSource, kSourceNames[static_cast<int>(c.source)]);
        }
        ColorF color = c.color.a > 0 ? c.color : ColorF{0.17f, 0.42f, 0.69f, 1};
        if (PropColor(str::kColor, color, false)) {
            ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                Clip* pc = p.FindClip(k.id);
                if (!pc) return false;
                pc->color = ColorF{color.r, color.g, color.b, 1};
                return true;
            });
        }
        Coalesce(state, str::kUndoProperty);
        SectionGap();
    }

    if (visual) {
        if (SectionHeader(str::kSecTransform)) {
            Transform t = c.transform;
            const auto setter = [](auto field) {
                return [field](Project& p, const Clip& k, float v) {
                    Transform tt = k.transform;
                    tt.*field = v;
                    return ops::SetTransform(p, k.id, tt);
                };
            };
            struct Row { const char* label; const char* prop; float Transform::*field; float min, max, scale, reset; const char* fmt; const char* unit; float speed; };
            const Row rows[] = {
                {str::kPosX, "transform.x", &Transform::x, -2, 2, 1, 0, "%.3f", nullptr, 0.002f},
                {str::kPosY, "transform.y", &Transform::y, -2, 2, 1, 0, "%.3f", nullptr, 0.002f},
                {str::kScale, "transform.scale", &Transform::scale, 1, 1000, 100, 100, "%.0f", "%", 0.5f},
                {str::kRotation, "transform.rotation_deg", &Transform::rotation_deg, -360, 360, 1, 0, "%.1f", "\xC2\xB0", 0.2f},
                {str::kOpacity, "transform.opacity", &Transform::opacity, 0, 100, 100, 100, "%.0f", "%", 0.5f},
            };
            for (const Row& r : rows) {
                ClipKeyHook hook(state, c, r.prop);
                float v = c.Prop(r.prop, now, t.*(r.field)) * r.scale;
                if (PropFloat(r.label, v, r.min, r.max, r.fmt, r.unit, n == 1 ? &hook : nullptr, r.reset, r.speed))
                    SetKeyed(state, r.prop, v / r.scale, setter(r.field));
                Coalesce(state, str::kUndoProperty);
            }
            SectionGap();
        }
        if (!annotation && SectionHeader(str::kSecCrop, false)) {
            Crop cr = c.crop;
            int* fields[] = {&cr.left, &cr.top, &cr.right, &cr.bottom};
            const char* labels[] = {str::kLeft, str::kTop, str::kRight, str::kBottom};
            for (int i = 0; i < 4; ++i) {
                if (PropInt(labels[i], *fields[i], 0, 8192, "px", 0)) {
                    const int v = *fields[i];
                    ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                        Crop kc = k.crop;
                        int* kf[] = {&kc.left, &kc.top, &kc.right, &kc.bottom};
                        *kf[i] = v;
                        return ops::SetCrop(p, k.id, kc);
                    });
                }
                Coalesce(state, str::kUndoProperty);
            }
            SectionGap();
        }
        if (!annotation && SectionHeader(str::kSecStyle, false)) {
            bool use_default = !c.corner_radius_px.has_value();
            if (PropToggle(str::kUseCanvasStyle, use_default)) {
                const int canvas_r = pr.canvas.corner_radius_px;
                ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                    Clip* pc = p.FindClip(k.id);
                    if (!pc) return false;
                    if (use_default) {
                        pc->corner_radius_px.reset();
                        pc->shadow.reset();
                    } else {
                        pc->corner_radius_px = canvas_r;
                        pc->shadow = p.canvas.shadow;
                    }
                    return true;
                });
            }
            if (!use_default) {
                int r = c.corner_radius_px.value_or(pr.canvas.corner_radius_px);
                if (PropInt(str::kCornerRadius, r, 0, 400, "px", 18)) {
                    ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                        Clip* pc = p.FindClip(k.id);
                        if (!pc) return false;
                        pc->corner_radius_px = r;
                        return true;
                    });
                }
                Coalesce(state, str::kUndoProperty);
                Shadow sh = c.shadow.value_or(pr.canvas.shadow);
                bool changed = PropToggle(str::kShadow, sh.enabled);
                if (sh.enabled) {
                    changed |= PropFloat(str::kBlur, sh.blur_px, 0, 200, "%.0f", "px", nullptr, 48, 0.25f);
                    Coalesce(state, str::kUndoProperty);
                    changed |= PropFloat(str::kOffsetY, sh.offset_y_px, -100, 100, "%.0f", "px", nullptr, 24, 0.25f);
                    Coalesce(state, str::kUndoProperty);
                }
                if (changed) {
                    ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                        Clip* pc = p.FindClip(k.id);
                        if (!pc) return false;
                        pc->shadow = sh;
                        return true;
                    });
                }
            }
            SectionGap();
        }
    }

    // Audio: audio-track clips and video clips with sound.
    const Media* media = pr.FindMedia(c.media);
    const bool has_audio = audio_track || (media && !media->audio_streams.empty() && !screen);
    if (has_audio && !annotation && SectionHeader(str::kSecAudio)) {
        AudioProps a = c.audio;
        {
            ClipKeyHook hook(state, c, "audio.gain_db");
            float g = c.Prop("audio.gain_db", now, a.gain_db);
            if (PropFloat(str::kGain, g, -60, 24, "%.1f", "dB", n == 1 ? &hook : nullptr, 0, 0.1f)) {
                SetKeyed(state, "audio.gain_db", g, [](Project& p, const Clip& k, float v) {
                    AudioProps ka = k.audio;
                    ka.gain_db = v;
                    return ops::SetAudio(p, k.id, ka);
                });
            }
            Coalesce(state, str::kUndoProperty);
        }
        auto apply = [&](auto&& mutate) {
            ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) {
                AudioProps ka = k.audio;
                mutate(ka);
                return ops::SetAudio(p, k.id, ka);
            });
        };
        if (PropToggle(str::kMuted, a.muted)) apply([&](AudioProps& x) { x.muted = a.muted; });
        if (PropTime(str::kFadeIn, a.fade_in_us, 0, c.Duration())) apply([&](AudioProps& x) { x.fade_in_us = a.fade_in_us; });
        Coalesce(state, str::kUndoProperty);
        if (PropTime(str::kFadeOut, a.fade_out_us, 0, c.Duration())) apply([&](AudioProps& x) { x.fade_out_us = a.fade_out_us; });
        Coalesce(state, str::kUndoProperty);
        if (PropToggle(str::kNormalize, a.normalize)) apply([&](AudioProps& x) { x.normalize = a.normalize; });
        bool gate = a.noise_gate_db.has_value();
        if (PropToggle(str::kNoiseGate, gate)) apply([&](AudioProps& x) { x.noise_gate_db = gate ? std::optional<float>(-45.0f) : std::nullopt; });
        if (gate) {
            float th = a.noise_gate_db.value_or(-45.0f);
            if (PropFloat(str::kThreshold, th, -90, 0, "%.0f", "dB", nullptr, -45, 0.2f)) apply([&](AudioProps& x) { x.noise_gate_db = th; });
            Coalesce(state, str::kUndoProperty);
        }
        SectionGap();
    }

    if (screen && SectionHeader(str::kSecCursor)) {
        CursorStyle cs = c.cursor;
        bool changed = PropToggle(str::kVisible, cs.visible);
        changed |= PropFloat(str::kSize, cs.scale, 0.25f, 5.0f, "%.2f", "\xC3\x97", nullptr, 1.5f, 0.01f);
        Coalesce(state, str::kUndoProperty);
        changed |= PropFloat(str::kSmoothing, cs.smoothing, 0.0f, 1.0f, "%.2f", nullptr, nullptr, 0.35f, 0.002f);
        Coalesce(state, str::kUndoProperty);
        int click = static_cast<int>(cs.click);
        if (PropEnum(str::kClickEffect, click, kClickNames, 4)) {
            cs.click = static_cast<CursorStyle::Click>(click);
            changed = true;
        }
        changed |= PropColor(str::kClickColor, cs.click_color, false);
        Coalesce(state, str::kUndoProperty);
        TimeUs idle = cs.hide_when_idle_us;
        if (PropTime(str::kHideIdle, idle, 0, 60 * kUsPerSecond)) {
            cs.hide_when_idle_us = idle;
            changed = true;
        }
        Coalesce(state, str::kUndoProperty);
        changed |= PropToggle(str::kSpotlight, cs.spotlight);
        changed |= PropToggle(str::kShadow, cs.shadow);
        if (changed) ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) { return k.source == Clip::Source::Screen && ops::SetCursorStyle(p, k.id, cs); });
        SectionGap();
    }
    if (screen && SectionHeader(str::kSecKeystrokes, false)) {
        KeystrokeStyle ks = c.keystrokes;
        bool changed = PropToggle(str::kEnabled, ks.enabled);
        int pos = static_cast<int>(ks.pos), style = static_cast<int>(ks.style);
        if (PropEnum(str::kPosition, pos, kKeyPosNames, 4)) {
            ks.pos = static_cast<KeystrokeStyle::Pos>(pos);
            changed = true;
        }
        PropRowBegin(str::kStyle);
        if (SegmentedControl("##kstyle", &style, kKeyStyleNames, 2)) {
            ks.style = static_cast<KeystrokeStyle::Style>(style);
            changed = true;
        }
        changed |= PropInt(str::kHold, ks.hold_ms, 100, 10000, "ms", 1200);
        Coalesce(state, str::kUndoProperty);
        if (changed) ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) { return k.source == Clip::Source::Screen && ops::SetKeystrokeStyle(p, k.id, ks); });
        SectionGap();
    }
    if (camera && SectionHeader(str::kSecPip)) {
        bool on = c.pip.has_value();
        Pip pip = c.pip.value_or(Pip{});
        bool changed = PropToggle(str::kEnabled, on);
        if (on) {
            int corner = static_cast<int>(pip.corner), mask = static_cast<int>(pip.mask);
            if (PropEnum(str::kCorner, corner, kCornerNames, 4)) {
                pip.corner = static_cast<Pip::Corner>(corner);
                changed = true;
            }
            float size = pip.size * 100;
            if (PropFloat(str::kSize, size, 5, 100, "%.0f", "%", nullptr, 22, 0.2f)) {
                pip.size = size / 100;
                changed = true;
            }
            Coalesce(state, str::kUndoProperty);
            changed |= PropInt(str::kMargin, pip.margin_px, 0, 400, "px", 32);
            Coalesce(state, str::kUndoProperty);
            PropRowBegin(str::kMask);
            if (SegmentedControl("##mask", &mask, kMaskNames, 3)) {
                pip.mask = static_cast<Pip::Mask>(mask);
                changed = true;
            }
            if (pip.mask == Pip::Mask::Rounded) {
                changed |= PropInt(str::kCornerRadius, pip.radius_px, 0, 400, "px", 24);
                Coalesce(state, str::kUndoProperty);
            }
            changed |= PropToggle(str::kShadow, pip.shadow);
            changed |= PropToggle(str::kMirror, pip.mirror);
        }
        if (changed) {
            const std::optional<Pip> v = on ? std::optional<Pip>(pip) : std::nullopt;
            ApplyToClips(state, str::kUndoProperty, [&](Project& p, const Clip& k) { return ops::SetPip(p, k.id, v); });
        }
        SectionGap();
    }
    if (annotation && SectionHeader(str::kSecAnnotation)) {
        Annotation an = *c.annotation;
        bool changed = false;
        int type = static_cast<int>(an.type);
        if (PropEnum(str::kType, type, kAnnTypes, 8)) {
            an.type = static_cast<Annotation::Type>(type);
            changed = true;
        }
        const bool texty = an.type == Annotation::Type::Text || an.type == Annotation::Type::Callout;
        if (texty) {
            changed |= PropText(str::kText, an.text, true);
            Coalesce(state, str::kUndoProperty);
            changed |= PropInt(str::kSize, an.size_px, 6, 400, "px", 48);
            Coalesce(state, str::kUndoProperty);
            changed |= PropInt(str::kWeight, an.weight, 100, 900, nullptr, 600);
            Coalesce(state, str::kUndoProperty);
            changed |= PropColor(str::kTextColor, an.color);
            Coalesce(state, str::kUndoProperty);
        }
        if (an.type != Annotation::Type::Blur) {
            changed |= PropColor(str::kFill, an.fill);
            Coalesce(state, str::kUndoProperty);
        }
        if (an.type != Annotation::Type::Blur && an.type != Annotation::Type::Highlight) {
            changed |= PropInt(str::kStrokeWidth, an.stroke_px, 0, 50, "px", 0);
            Coalesce(state, str::kUndoProperty);
            if (an.stroke_px > 0) {
                changed |= PropColor(str::kStroke, an.stroke);
                Coalesce(state, str::kUndoProperty);
            }
        }
        if (an.type == Annotation::Type::Rect || an.type == Annotation::Type::Callout || an.type == Annotation::Type::Text) {
            changed |= PropInt(str::kCornerRadius, an.radius_px, 0, 200, "px", 12);
            Coalesce(state, str::kUndoProperty);
        }
        if (an.type == Annotation::Type::Arrow) {
            changed |= PropFloat(str::kArrowHead, an.arrow_head_px, 4, 200, "%.0f", "px", nullptr, 24, 0.25f);
            Coalesce(state, str::kUndoProperty);
        }
        if (an.type == Annotation::Type::Blur) {
            changed |= PropInt(str::kBlur, an.blur_px, 1, 200, "px", 24);
            Coalesce(state, str::kUndoProperty);
            changed |= PropToggle(str::kPixelate, an.pixelate);
        }
        int ain = static_cast<int>(an.anim_in), aout = static_cast<int>(an.anim_out);
        if (PropEnum(str::kAnimIn, ain, kAnimNames, 4)) {
            an.anim_in = static_cast<Annotation::Anim>(ain);
            changed = true;
        }
        if (PropEnum(str::kAnimOut, aout, kAnimNames, 4)) {
            an.anim_out = static_cast<Annotation::Anim>(aout);
            changed = true;
        }
        changed |= PropInt(str::kAnimMs, an.anim_ms, 0, 3000, "ms", 220);
        Coalesce(state, str::kUndoProperty);
        if (changed && n == 1) state.Edit(str::kUndoProperty, [&](Project& p) { return ops::SetAnnotation(p, c.id, an); });
        SectionGap();
    }

    // Transition out of this clip.
    const Transition* trans = nullptr;
    for (const Transition& t : pr.transitions)
        if (t.a == c.id) trans = &t;
    if (trans && n == 1 && SectionHeader(str::kSecTransition)) {
        Transition t = *trans;
        bool changed = false;
        int kind = static_cast<int>(t.kind);
        if (PropEnum(str::kKind, kind, kTransKinds, 3)) {
            t.kind = static_cast<Transition::Kind>(kind);
            changed = true;
        }
        if (PropTime(str::kDuration, t.duration_us, TimeUs(100'000), INT64_MAX / 4)) changed = true;
        Coalesce(state, str::kUndoProperty);
        if (t.kind == Transition::Kind::Dip) {
            changed |= PropColor(str::kColorA, t.color, false);
            Coalesce(state, str::kUndoProperty);
        }
        if (t.kind == Transition::Kind::Slide) {
            int dir = static_cast<int>(t.dir);
            if (PropEnum(str::kDirection, dir, kDirNames, 4)) {
                t.dir = static_cast<Transition::Dir>(dir);
                changed = true;
            }
        }
        if (changed) {
            Err err;
            state.EditCopy(str::kUndoProperty, [&](Project& p) {
                ops::RemoveTransition(p, t.a, t.b);
                return ops::AddTransition(p, t, &err);
            });
        }
        if (Button(str::kDelete, ButtonKind::Danger, ImVec2(-FLT_MIN, 0)))
            state.Edit(str::kUndoProperty, [&](Project& p) { return ops::RemoveTransition(p, t.a, t.b); });
        SectionGap();
    }
}

} // namespace ct::ui
