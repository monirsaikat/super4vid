#define CT_LOG_CATEGORY "capture"
#include <ct/capture/cursor_tracker.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <algorithm>
#include <cstring>

namespace ct::capture {

namespace {

// A 32-bpp top-down DIB section selected into its own memory DC; everything is released by the destructor.
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;

    bool Create(int w, int h) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        void* p = nullptr;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!bmp || !p) return false;
        bits = static_cast<uint32_t*>(p);
        old = SelectObject(dc, bmp);
        return true;
    }
    ~Dib() {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

uint32_t ShapeHash(const CursorShape& s) {
    struct Header {
        uint32_t pixels;
        uint16_t w, h;
        int16_t hx, hy;
    } hdr{Fnv1a32(s.bgra_premul.data(), s.bgra_premul.size() * sizeof(uint32_t)), s.w, s.h, s.hot_x, s.hot_y};
    const uint32_t id = Fnv1a32(&hdr, sizeof(hdr));
    return id == 0 ? 1u : id;
}

} // namespace

bool CursorTracker::CaptureShape(HCURSOR hcursor, CursorShape& out) {
    if (!hcursor) return false;
    ICONINFOEXW ii{};
    ii.cbSize = sizeof(ii);
    if (!GetIconInfoExW(hcursor, &ii)) return false;
    // GetIconInfoExW hands us both bitmaps: delete them on every path (a leak at 120 Hz hits the 10,000 GDI handle limit).
    struct Bitmaps {
        HBITMAP color, mask;
        ~Bitmaps() {
            if (color) DeleteObject(color);
            if (mask) DeleteObject(mask);
        }
    } owned{ii.hbmColor, ii.hbmMask};

    BITMAP bm{};
    if (!ii.hbmMask || GetObjectW(ii.hbmMask, sizeof(bm), &bm) == 0) return false;
    const int w = bm.bmWidth;
    const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // monochrome: AND mask over XOR mask
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;

    Dib black, white;
    if (!black.Create(w, h) || !white.Create(w, h)) return false;
    const size_t n = static_cast<size_t>(w) * h;
    std::memset(black.bits, 0x00, n * 4);
    std::memset(white.bits, 0xFF, n * 4);
    // Frame 0 of animated cursors.
    if (!DrawIconEx(black.dc, 0, 0, hcursor, w, h, 0, nullptr, DI_NORMAL)) return false;
    if (!DrawIconEx(white.dc, 0, 0, hcursor, w, h, 0, nullptr, DI_NORMAL)) return false;
    GdiFlush();

    out.w = static_cast<uint16_t>(w);
    out.h = static_cast<uint16_t>(h);
    out.hot_x = static_cast<int16_t>(ii.xHotspot);
    out.hot_y = static_cast<int16_t>(ii.yHotspot);
    out.bgra_premul.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t b = black.bits[i], wt = white.bits[i];
        const int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
        const int wr = (wt >> 16) & 0xFF, wg = (wt >> 8) & 0xFF, wb = wt & 0xFF;
        uint32_t px;
        if (br > wr || bg > wg || bb > wb) {
            // Inverting pixel (XOR cursors like the I-beam): opaque, drawn with the colour it has over white.
            px = 0xFF000000u | (static_cast<uint32_t>(wr) << 16) | (static_cast<uint32_t>(wg) << 8) | static_cast<uint32_t>(wb);
        } else {
            const int a = std::clamp(255 - (wr - br), 0, 255);
            const int r = std::min(br, a), g = std::min(bg, a), bl = std::min(bb, a);
            px = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                 static_cast<uint32_t>(bl);
        }
        out.bgra_premul[i] = px;
    }
    out.id = ShapeHash(out);
    return true;
}

CursorTracker::CursorTracker() { m_known.reserve(64); }

void CursorTracker::Reset() {
    m_last_hcursor = nullptr;
    m_last_shape_id = 0;
    m_last = CursorSample{};
    m_have_last = false;
    m_known.clear();
    m_polls = 0;
    m_dropped = 0;
}

bool CursorTracker::IsKnown(uint32_t id) const { return std::find(m_known.begin(), m_known.end(), id) != m_known.end(); }

void CursorTracker::Poll(TimeUs t_us, const RectI& region, SpscQueue<CursorSample, 4096>& samples,
                         SpscQueue<CursorShape, 64>& shapes) {
    ++m_polls;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;   // fails briefly on the secure desktop; keep the last sample

    if (ci.hCursor != m_last_hcursor) [[unlikely]] {
        uint32_t id = 0;
        bool retry = false;
        CursorShape shape;
        // A cursor that cannot be captured keeps shape id 0 (the editor draws its default arrow); no retry storm.
        if (ci.hCursor && CaptureShape(ci.hCursor, shape)) {
            id = shape.id;
            if (!IsKnown(id)) {
                if (shapes.Push(std::move(shape))) {
                    m_known.push_back(id);
                } else {
                    retry = true;   // queue full: try again on the next poll
                    ++m_dropped;
                }
            }
        }
        if (!retry) {
            m_last_hcursor = ci.hCursor;
            m_last_shape_id = id;
        }
    }

    CursorSample s;
    s.t_us = t_us;
    s.x = ci.ptScreenPos.x - region.x;
    s.y = ci.ptScreenPos.y - region.y;
    s.shape_id = m_last_shape_id;
    s.visible = (ci.flags & CURSOR_SHOWING) != 0;
    if (m_have_last && s.x == m_last.x && s.y == m_last.y && s.shape_id == m_last.shape_id && s.visible == m_last.visible)
        return;
    if (!samples.Push(s)) {
        ++m_dropped;
        return;
    }
    m_last = s;
    m_have_last = true;
}

} // namespace ct::capture
