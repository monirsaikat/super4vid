#pragma once
// ct/recorder/recording_frame.h
// The thin record-colored (#FF4D4F) 2 px frame around the capture region for the whole recording (docs/05 section 3):
// four click-through, topmost layered windows (one per edge, so a 4K region does not need a 4K surface), excluded
// from capture. Edges that would fall outside every display are drawn just inside the region instead.
// Thread model: UI thread only. Lifetime: Hide() or the destructor destroys the windows.

#include <ct/core/types.h>

#include <memory>
#include <span>

namespace ct::recorder {

class RecordingFrame {
public:
    RecordingFrame();
    ~RecordingFrame();
    RecordingFrame(const RecordingFrame&) = delete;
    RecordingFrame& operator=(const RecordingFrame&) = delete;

    // display_rects: every display's virtual-screen rect (for the inside/outside decision). dpi scales the thickness.
    bool Show(const RectI& region, std::span<const RectI> display_rects, unsigned dpi = 96);
    void Hide();
    bool IsVisible() const;
    bool ExcludedFromCapture() const;   // all four edges accepted WDA_EXCLUDEFROMCAPTURE

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::recorder
