#pragma once
// ct/ui/panels/media_panel.h
// Media panel shell (docs/05 section 4, P09 9.2 without thumbnails/probing): a "Media" tab with the project's media
// as cards (thumbnail via TimelineMediaProvider, name, duration, REC/CAM/EVT/OFFLINE badges, drag source "CT_MEDIA"
// = NUL-terminated media id, context menu) and an "Effects" tab with annotation preset cards (drag source
// "CT_ANNOTATION" = int Annotation::Type). File dialogs, probing and relinking are callbacks the editor provides.
// Thread model: UI thread, inside the "Media" window.

#include <ct/project/types.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/timeline_media.h>

#include <functional>

namespace ct::ui {

class MediaPanel {
public:
    std::function<void()> on_import;                          // "Import" button / Ctrl+I (IFileOpenDialog in the editor)
    std::function<void(const project::Id&)> on_reveal;        // Reveal in Explorer
    std::function<void(const project::Id&)> on_relink;        // Relink... (file dialog, clears `missing`)

    void SetMediaProvider(TimelineMediaProvider* p) { m_media = p ? p : &NullMediaProvider::Instance(); }
    void Draw(EditorState& state);

private:
    void DrawMediaTab(EditorState& state);
    void DrawEffectsTab(EditorState& state);
    TimelineMediaProvider* m_media = &NullMediaProvider::Instance();
    project::Id m_ctx;
};

} // namespace ct::ui
