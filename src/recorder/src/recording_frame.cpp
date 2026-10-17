#define CT_LOG_CATEGORY "overlay"
#include <ct/recorder/recording_frame.h>

#include "overlay_style.h"

#include <ct/platform/layered_window.h>
#include <ct/recorder/overlay_geometry.h>

#include <algorithm>

namespace ct::recorder {

struct RecordingFrame::Impl {
    platform::LayeredWindow edges[4];
    bool visible = false;
};

RecordingFrame::RecordingFrame() : m(std::make_unique<Impl>()) {}
RecordingFrame::~RecordingFrame() { Hide(); }

bool RecordingFrame::Show(const RectI& region, std::span<const RectI> display_rects, unsigned dpi) {
    Hide();
    if (region.IsEmpty()) return false;
    const int32_t t = std::max(overlay::kFrameThickness,
                               static_cast<int32_t>(overlay::kFrameThickness * static_cast<float>(dpi ? dpi : 96) / 96.f + 0.5f));
    const overlay::FrameEdges f = overlay::FrameEdgesFor(region, display_rects, t);
    for (int i = 0; i < 4; ++i) {
        platform::LayeredWindow& w = m->edges[i];
        if (!w.Create(f.edge[i], true, true, true)) {
            Hide();
            return false;
        }
        if (ID2D1RenderTarget* rt = w.BeginDraw()) {
            rt->Clear(style::kRecord);
            w.EndDrawAndPresent();
        }
        w.Show(true, false);
    }
    m->visible = true;
    return true;
}

void RecordingFrame::Hide() {
    for (auto& w : m->edges) w.Destroy();
    m->visible = false;
}

bool RecordingFrame::IsVisible() const { return m->visible; }

bool RecordingFrame::ExcludedFromCapture() const {
    return std::all_of(std::begin(m->edges), std::end(m->edges), [](const platform::LayeredWindow& w) { return w.ExcludedFromCapture(); });
}

} // namespace ct::recorder
