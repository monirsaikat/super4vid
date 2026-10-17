#include <ct/recorder/overlay_geometry.h>

#include <ct/core/strings.h>

#include <algorithm>

namespace ct::recorder::overlay {

namespace {
int64_t Area(const RectI& r) { return r.IsEmpty() ? 0 : static_cast<int64_t>(r.w) * r.h; }
bool Inside(PointI p, const RectI& r) { return p.x >= r.x && p.x < r.Right() && p.y >= r.y && p.y < r.Bottom(); }
} // namespace

RectI MakeEven(RectI r) {
    r.w = std::max(0, r.w) & ~1;
    r.h = std::max(0, r.h) & ~1;
    return r;
}

RectI RectFromDrag(PointI a, PointI b) {
    if (a.x == b.x && a.y == b.y) return RectI{a.x, a.y, 0, 0};
    const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int32_t x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    return MakeEven(RectI{x0, y0, x1 - x0 + 1, y1 - y0 + 1});
}

PointI HandleCenter(const RectI& sel, Handle h) {
    const int32_t l = sel.x, t = sel.y, r = sel.Right(), b = sel.Bottom();
    const int32_t cx = sel.x + sel.w / 2, cy = sel.y + sel.h / 2;
    switch (h) {
    case Handle::TopLeft: return {l, t};
    case Handle::Top: return {cx, t};
    case Handle::TopRight: return {r, t};
    case Handle::Right: return {r, cy};
    case Handle::BottomRight: return {r, b};
    case Handle::Bottom: return {cx, b};
    case Handle::BottomLeft: return {l, b};
    case Handle::Left: return {l, cy};
    default: return {cx, cy};
    }
}

RectI HandleRect(const RectI& sel, Handle h, int32_t handle_px) {
    const PointI c = HandleCenter(sel, h);
    return RectI{c.x - handle_px / 2, c.y - handle_px / 2, handle_px, handle_px};
}

Handle HitTest(const RectI& sel, PointI p, int32_t handle_px, int32_t slop) {
    if (sel.IsEmpty()) return Handle::None;
    static constexpr Handle kOrder[] = {Handle::TopLeft, Handle::TopRight, Handle::BottomRight, Handle::BottomLeft,
                                        Handle::Top, Handle::Right, Handle::Bottom, Handle::Left};
    for (Handle h : kOrder) {
        if (Inside(p, Inflate(HandleRect(sel, h, handle_px), slop))) return h;
    }
    return Inside(p, sel) ? Handle::Inside : Handle::None;
}

RectI ApplyDrag(const RectI& start, Handle h, PointI d, const RectI& bounds, int32_t min_size) {
    if (h == Handle::None) return start;
    if (h == Handle::Inside) {
        RectI r = start;
        r.x = std::clamp(start.x + d.x, bounds.x, std::max(bounds.x, bounds.Right() - start.w));
        r.y = std::clamp(start.y + d.y, bounds.y, std::max(bounds.y, bounds.Bottom() - start.h));
        return r;
    }
    int32_t l = start.x, t = start.y, r = start.Right(), b = start.Bottom();
    const bool left = h == Handle::Left || h == Handle::TopLeft || h == Handle::BottomLeft;
    const bool right = h == Handle::Right || h == Handle::TopRight || h == Handle::BottomRight;
    const bool top = h == Handle::Top || h == Handle::TopLeft || h == Handle::TopRight;
    const bool bottom = h == Handle::Bottom || h == Handle::BottomLeft || h == Handle::BottomRight;
    if (left) l = std::clamp(start.x + d.x, bounds.x, std::max(bounds.x, r - min_size));
    if (right) r = std::clamp(start.Right() + d.x, std::min(bounds.Right(), l + min_size), bounds.Right());
    if (top) t = std::clamp(start.y + d.y, bounds.y, std::max(bounds.y, b - min_size));
    if (bottom) b = std::clamp(start.Bottom() + d.y, std::min(bounds.Bottom(), t + min_size), bounds.Bottom());
    const int32_t w = std::max(0, r - l) & ~1, hh = std::max(0, b - t) & ~1;
    // The dragged edge absorbs the even rounding so the fixed edge never moves.
    if (left) l = r - w;
    if (top) t = b - hh;
    return RectI{l, t, w, hh};
}

RectI Nudge(const RectI& sel, int32_t dx, int32_t dy, bool resize, const RectI& bounds) {
    if (!resize) return ApplyDrag(sel, Handle::Inside, PointI{dx, dy}, bounds);
    return ApplyDrag(sel, Handle::BottomRight, PointI{dx, dy}, bounds);
}

bool IsSnappable(const SnapWindow& w) {
    return w.visible && !w.cloaked && !w.tool && !w.minimized && !w.own_process && !w.rect.IsEmpty();
}

int PickSnapWindow(std::span<const SnapWindow> zorder, PointI p) {
    for (size_t i = 0; i < zorder.size(); ++i) {
        if (IsSnappable(zorder[i]) && Inside(p, zorder[i].rect)) return static_cast<int>(i);
    }
    return -1;
}

RectI SnapRectAt(std::span<const SnapWindow> zorder, PointI p, const RectI& display_rect) {
    const int i = PickSnapWindow(zorder, p);
    if (i < 0) return RectI{};
    const RectI r = zorder[static_cast<size_t>(i)].rect.Intersect(display_rect);
    return r.IsEmpty() ? RectI{} : MakeEven(r);
}

FinalRegion Finalize(const RectI& sel, std::span<const RectI> displays) {
    FinalRegion out;
    if (sel.IsEmpty()) return out;
    const PointI centre{sel.x + sel.w / 2, sel.y + sel.h / 2};
    int64_t best = 0;
    bool best_has_centre = false;
    for (size_t i = 0; i < displays.size(); ++i) {
        const int64_t a = Area(sel.Intersect(displays[i]));
        if (a <= 0) continue;
        const bool has_centre = Inside(centre, displays[i]);
        if (a > best || (a == best && has_centre && !best_has_centre)) {
            best = a;
            best_has_centre = has_centre;
            out.display = static_cast<int>(i);
        }
    }
    if (out.display < 0) return out;
    const RectI& d = displays[static_cast<size_t>(out.display)];
    RectI r = MakeEven(sel.Intersect(d));
    if (r.w < kMinRegion) r.w = std::min(kMinRegion, d.w & ~1);
    if (r.h < kMinRegion) r.h = std::min(kMinRegion, d.h & ~1);
    if (r.Right() > d.Right()) r.x = d.Right() - r.w;
    if (r.Bottom() > d.Bottom()) r.y = d.Bottom() - r.h;
    r.x = std::max(r.x, d.x);
    r.y = std::max(r.y, d.y);
    out.region = r;
    return out;
}

std::string SizeLabel(int32_t w, int32_t h) { return Format("%d \xC3\x97 %d", w, h); }

RectI LabelRect(const RectI& sel, SizeI label, const RectI& bounds, int32_t margin) {
    RectI r{sel.Right() - label.w, sel.Bottom() + margin, label.w, label.h};
    if (r.Bottom() > bounds.Bottom()) r.y = sel.y - margin - label.h;
    if (r.y < bounds.y) r.y = sel.Bottom() - margin - label.h;
    r.x = std::clamp(r.x, bounds.x, std::max(bounds.x, bounds.Right() - label.w));
    r.y = std::clamp(r.y, bounds.y, std::max(bounds.y, bounds.Bottom() - label.h));
    return r;
}

int NextMonitor(int current, int count) {
    if (count <= 0) return -1;
    if (current < 0 || current >= count - 1) return 0;
    return current + 1;
}

FrameEdges FrameEdgesFor(const RectI& g, std::span<const RectI> displays, int32_t t) {
    auto visible = [&](const RectI& e) {
        for (const RectI& d : displays)
            if (!e.Intersect(d).IsEmpty()) return true;
        return false;
    };
    FrameEdges f;
    const RectI outside[4] = {
        {g.x - t, g.y - t, g.w + 2 * t, t},   // top
        {g.x - t, g.Bottom(), g.w + 2 * t, t},   // bottom
        {g.x - t, g.y, t, g.h},   // left
        {g.Right(), g.y, t, g.h},   // right
    };
    const RectI inside[4] = {
        {g.x, g.y, g.w, t},
        {g.x, g.Bottom() - t, g.w, t},
        {g.x, g.y, t, g.h},
        {g.Right() - t, g.y, t, g.h},
    };
    for (int i = 0; i < 4; ++i) f.edge[i] = visible(outside[i]) ? outside[i] : inside[i];
    return f;
}

RectI CenteredSquare(const RectI& region, int32_t diameter) {
    const int32_t d = std::max(0, std::min({diameter, region.w, region.h}));
    return RectI{region.x + (region.w - d) / 2, region.y + (region.h - d) / 2, d, d};
}

RectI Union(const RectI& a, const RectI& b) {
    if (a.IsEmpty()) return b;
    if (b.IsEmpty()) return a;
    const int32_t l = std::min(a.x, b.x), t = std::min(a.y, b.y);
    const int32_t r = std::max(a.Right(), b.Right()), btm = std::max(a.Bottom(), b.Bottom());
    return RectI{l, t, r - l, btm - t};
}

RectI Inflate(const RectI& r, int32_t by) { return RectI{r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by}; }

} // namespace ct::recorder::overlay
