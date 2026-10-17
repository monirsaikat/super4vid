#include <ct/project/camera.h>

#include <algorithm>
#include <cmath>

namespace ct::project {

namespace {

constexpr float kMinZoom = 1e-3f;

Camera::View Clamped(float zoom, float cx, float cy) {
    return Camera::View{zoom, ClampCentre(cx, zoom), ClampCentre(cy, zoom)};
}

} // namespace

float ClampCentre(float c, float zoom) {
    if (!(zoom > 1.0f)) return 0.5f;
    const float half = 0.5f / zoom;
    return std::clamp(c, half, 1.0f - half);
}

Camera::View Camera::At(TimeUs t) const {
    if (keys.empty()) return View{1, 0.5f, 0.5f};
    const CameraKey& first = keys.front();
    if (t <= first.t_us) return Clamped(first.zoom, first.cx, first.cy);
    const CameraKey& last = keys.back();
    if (t >= last.t_us) return Clamped(last.zoom, last.cx, last.cy);
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](TimeUs tt, const CameraKey& k) { return tt < k.t_us; });
    const CameraKey& b = *it;
    const CameraKey& a = *(it - 1);
    const float u = static_cast<float>(double(t - a.t_us) / double(b.t_us - a.t_us));
    const float e = ApplyEase(a.ease, u);
    const float la = std::log2(std::max(a.zoom, kMinZoom));
    const float lb = std::log2(std::max(b.zoom, kMinZoom));
    const float zoom = std::exp2(la + (lb - la) * e);
    const float cx = a.cx + (b.cx - a.cx) * e;
    const float cy = a.cy + (b.cy - a.cy) * e;
    return Clamped(zoom, cx, cy);
}

void Camera::SetKey(const CameraKey& key) {
    auto it = std::lower_bound(keys.begin(), keys.end(), key.t_us,
                               [](const CameraKey& k, TimeUs t) { return k.t_us < t; });
    if (it != keys.end() && it->t_us == key.t_us)
        *it = key;
    else
        keys.insert(it, key);
}

bool Camera::RemoveKey(TimeUs t) {
    auto it = std::lower_bound(keys.begin(), keys.end(), t, [](const CameraKey& k, TimeUs tt) { return k.t_us < tt; });
    if (it == keys.end() || it->t_us != t) return false;
    keys.erase(it);
    return true;
}

void Camera::ReplaceGenerated(const std::vector<CameraKey>& generated) {
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const CameraKey& k) { return k.generated; }), keys.end());
    for (CameraKey k : generated) {
        auto it = std::lower_bound(keys.begin(), keys.end(), k.t_us,
                                   [](const CameraKey& a, TimeUs t) { return a.t_us < t; });
        if (it != keys.end() && it->t_us == k.t_us) continue;   // manual key wins
        k.generated = true;
        keys.insert(it, k);
    }
}

bool Camera::IsSorted() const {
    for (size_t i = 1; i < keys.size(); ++i)
        if (keys[i - 1].t_us >= keys[i].t_us) return false;
    return true;
}

} // namespace ct::project
