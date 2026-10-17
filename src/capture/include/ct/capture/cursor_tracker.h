#pragma once
// ct/capture/cursor_tracker.h
// Cursor as data: position/visibility samples (pushed only on change) and de-duplicated shape bitmaps, so the editor
// re-renders a crisp, smoothed, scalable cursor instead of baking it into the video.
// Thread model: one thread calls Reset/Poll (ct-hooks in P05, ct-capture in the bench); the queues' consumer is another
// single thread. CaptureShape is a pure function (GDI only) and safe from any thread.
// Hot path: Poll does no allocation unless the cursor handle changed (then CaptureShape allocates the bitmap).
// Limitation: animated cursors (.ani) are captured at frame 0 only.

#include <ct/core/spsc_queue.h>
#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <cstdint>
#include <vector>

namespace ct::capture {

struct CursorSample {
    TimeUs t_us = 0;
    int32_t x = 0, y = 0;          // hotspot position relative to the capture region's top-left (may be outside it)
    uint32_t shape_id = 0;         // 0 = no shape (cursor hidden by the system, e.g. while typing in some apps)
    bool visible = false;
};

struct CursorShape {
    uint32_t id = 0;               // FNV-1a over pixels + size + hotspot; never 0
    uint16_t w = 0, h = 0;
    int16_t hot_x = 0, hot_y = 0;
    std::vector<uint32_t> bgra_premul;   // w*h pixels, little-endian B,G,R,A bytes, premultiplied alpha
};

class CursorTracker {
public:
    CursorTracker();
    void Reset();   // forgets the last sample and every shape seen (call at session start)
    // Called at 120 Hz on the hooks thread (P05) or the capture thread (bench). region = capture region in virtual-screen px.
    // Pushes a CursorShape the first time a shape id is seen in this session, and a CursorSample when position,
    // visibility or shape changed since the last pushed sample. A full queue is retried on the next poll.
    void Poll(TimeUs t_us, const RectI& region, SpscQueue<CursorSample, 4096>& samples, SpscQueue<CursorShape, 64>& shapes);
    static bool CaptureShape(HCURSOR hcursor, CursorShape& out);   // public for tests

    uint64_t Polls() const { return m_polls; }
    uint64_t DroppedSamples() const { return m_dropped; }   // pushes that found the queue full

private:
    bool IsKnown(uint32_t id) const;

    HCURSOR m_last_hcursor = nullptr;
    uint32_t m_last_shape_id = 0;
    CursorSample m_last{};
    bool m_have_last = false;
    std::vector<uint32_t> m_known;   // shape ids pushed this session (reserved; a session has a few dozen at most)
    uint64_t m_polls = 0;
    uint64_t m_dropped = 0;
};

} // namespace ct::capture
