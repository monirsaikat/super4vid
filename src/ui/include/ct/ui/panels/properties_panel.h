#pragma once
// ct/ui/panels/properties_panel.h
// Properties panel (docs/05 section 4, docs/phases/P09 9.5): per-selection sections (Clip, Transform, Crop, Corners &
// shadow, Audio, Cursor, Keystrokes, PIP, Annotation, Transition), marker / camera-key editors, and the Project,
// Canvas/Background and Camera sections when nothing is selected. Multi-selection edits apply the changed field to
// every selected clip in one undo entry. Keyframeable fields have a ◇ toggle; while a property has keys, editing it
// sets a key at the playhead. Drags coalesce into one undo entry.
// Thread model: UI thread, inside the "Properties" window.

#include <ct/ui/editor_state.h>

#include <functional>

namespace ct::ui {

class PropertiesPanel {
public:
    // "Regenerate" in the Camera section (needs the recordings' event logs; the editor provides it). Null = disabled.
    std::function<bool(EditorState&)> on_regenerate;

    void Draw(EditorState& state);
    // A debounced Auto-Zoom regeneration (300 ms after the last parameter change) is pending.
    bool WantsRedraw() const { return m_regen_due >= 0; }

private:
    double m_regen_due = -1;
    void DrawClipSections(EditorState& state);
    void DrawProjectSections(EditorState& state);
    void DrawCameraSection(EditorState& state);
    void DrawMarker(EditorState& state);
    void DrawCameraKey(EditorState& state);
};

} // namespace ct::ui
