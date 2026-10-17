#pragma once
// ct/ui/panels/preview_panel.h
// Preview panel (docs/05 section 4, docs/phases/P09 9.3): the compositor's canvas-size output letterboxed with a 24 px
// margin (fit / 50 / 100 / 200 %, Space+drag or middle-drag pans when zoomed), inspection overlays (safe areas,
// thirds, cursor / annotations on-off: render options only, never the project), on-canvas gizmos (selected video
// clip: move, 8 scale handles, rotation handle; annotations: move/resize, callout pointer, arrow ends, double-click
// inline text edit; selected camera key: dashed visible-area window, drag pans, corner drag zooms) and the transport
// bar (start, frame step, play, end, timecode with click-to-type, rate, fit menu, loop, overlays, mute + volume).
// Every model change goes through EditorState::Edit inside BeginDrag/EndDrag (one undo entry per drag).
// The image comes from the `render` hook (the editor window renders the compositor); without it a styled
// placeholder of the canvas is drawn (demo, tests).
// Thread model: UI thread, inside the "Preview" window.

#include <ct/project/project.h>
#include <ct/render/compositor.h>
#include <ct/render/layer_math.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/shortcuts.h>

#include <imgui.h>

#include <functional>
#include <string>

namespace ct::ui {

// What the preview shows; mapped onto render::RenderOptions (inspection only, the project is never changed).
struct PreviewView {
    bool cursor = true;
    bool annotations = true;
    bool keystrokes = true;
    bool camera = true;   // false while a camera key is edited: the whole canvas is shown with the key's window
};

struct PreviewFrame {
    ImTextureID tex = ImTextureID_Invalid;   // canvas-size texture (ImGui DX11: the SRV)
    bool exact = true;                       // false while a frame is still decoding (the panel redraws)
};

class PreviewPanel {
public:
    enum class Gizmo { None, Move, Scale, Rotate, AnnMove, AnnResize, AnnPointer, AnnArrowStart, AnnArrowEnd, CamPan, CamZoom, Pan };

    std::function<PreviewFrame(EditorState&, const PreviewView&)> render;   // null: placeholder
    std::function<void(Action)> run_action;                                // transport buttons (shell's dispatcher)
    // Displayed size of a clip's video (the decoded frame); null or empty -> the media's width/height.
    std::function<SizeI(const project::Clip&)> video_size;

    void Draw(EditorState& state);
    bool WantsRedraw() const { return m_wants_redraw; }
    // A text field of the panel (time entry, inline annotation text) has keyboard focus.
    bool EditingText() const { return m_time_edit || !m_text_clip.empty(); }

    // View state (also driven by tests).
    int fit_mode = 0;   // 0 fit, 1 50 %, 2 100 %, 3 200 %
    bool safe_areas = false;
    bool thirds = false;
    PreviewView view;

    // Last frame's mapping: screen = ImageMin() + canvas_px * Scale().
    ImVec2 ImageMin() const { return m_img_min; }
    float Scale() const { return m_scale; }
    ImVec2 CanvasToScreen(float x, float y) const { return ImVec2(m_img_min.x + x * m_scale, m_img_min.y + y * m_scale); }
    ImVec2 ScreenToCanvas(ImVec2 s) const;
    Gizmo CurrentGizmo() const { return m_gizmo; }
    // Screen-space corners (TL, TR, BL, BR) of the selected video clip's gizmo, false when none is shown.
    bool ClipGizmoCorners(const EditorState& state, ImVec2 out[4]) const;

private:
    void DrawImage(EditorState& state, ImVec2 area_min, ImVec2 area_size);
    void DrawPlaceholder(const EditorState& state, ImVec2 mn, ImVec2 mx);
    void DrawOverlays(ImDrawList* dl, ImVec2 mn, ImVec2 mx);
    void HandleGizmos(EditorState& state, bool hovered);
    void DrawClipGizmo(EditorState& state, ImDrawList* dl);
    void DrawAnnotationGizmo(EditorState& state, ImDrawList* dl, const project::Clip& c);
    void DrawCameraGizmo(EditorState& state, ImDrawList* dl);
    void DrawInlineText(EditorState& state);
    void DrawTransport(EditorState& state);
    void DrawTimecode(EditorState& state, float h);
    void DrawDragReadout(ImDrawList* dl, const char* text);
    void Run(Action a) {
        if (run_action) run_action(a);
    }

    render::Mat3 Camera(const EditorState& state) const;
    SizeI VideoSize(const EditorState& state, const project::Clip& c) const;
    const project::Clip* GizmoClip(const EditorState& state) const;          // selected video clip at the playhead
    const project::Clip* SelectedAnnotation(const EditorState& state) const; // selected annotation at the playhead
    bool AnnotationRect(const EditorState& state, const project::Clip& c, ImVec2* mn, ImVec2* mx) const;
    const project::Clip* HitTest(const EditorState& state, ImVec2 mouse) const;

    // Drag state (canvas coordinates are pre-camera unless named screen).
    struct Drag {
        project::Id clip;
        ImVec2 press_screen{};
        render::PointF press_pre{};          // mouse at press, pre-camera canvas px
        float x0 = 0, y0 = 0, s0 = 1, r0 = 0; // transform at press (values at the playhead)
        render::PointF centre_pre{};         // layer centre at press
        render::PointF anchor_pre{};         // opposite corner (Alt scale)
        int handle = -1;                     // 0..7 TL,T,TR,R,BR,B,BL,L
        RectF rect0{};              // annotation rect at press (normalized)
        project::PointF pointer0{};
        project::CameraKey key0{};
        ImVec2 pan0{};
    } m_drag;
    Gizmo m_gizmo = Gizmo::None;
    bool m_drag_started = false;
    char m_readout[64] = {};

    ImVec2 m_img_min{}, m_img_max{};
    float m_scale = 1;
    ImVec2 m_pan{};
    ImVec2 m_area_min{}, m_area_max{};
    bool m_wants_redraw = false;
    render::Compositor m_geom;   // geometry only (LayerRectOnCanvas / VideoToCanvas are pure); never Init

    // Time entry.
    bool m_time_edit = false;
    bool m_time_focus = false;
    char m_time_buf[32] = {};
    // Inline annotation text edit.
    project::Id m_text_clip;
    char m_text_buf[2048] = {};
    bool m_text_focus = false;
};

// "HH:MM:SS.FF", "MM:SS.FF", "SS.FF" (FF frames at fps), "12.5" or "12.5s" (seconds) -> microseconds; false when
// unparsable. Exposed for tests.
bool ParseTimecode(const char* s, int fps_num, int fps_den, TimeUs* out);

} // namespace ct::ui
