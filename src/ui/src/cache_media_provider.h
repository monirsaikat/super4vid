#pragma once
// src/ui/src/cache_media_provider.h (private to ct_ui)
// TimelineMediaProvider backed by playback::MediaCache (P09 9.2/9.4): 160x90 thumbnails from the cache's IO-pool
// thumbnail decoder and waveform peaks (100/s, disk-cached) interleaved into min/max pairs. Never blocks.
// SRVs handed to ImGui this frame are pinned (AddRef) until BeginFrame() of the next frame, so a cache eviction can
// never free a texture the draw list still references (P09 pitfalls).
// Thread model: UI thread.

#include <ct/playback/media_cache.h>
#include <ct/project/project.h>
#include <ct/ui/timeline_media.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ct::ui::detail {

class CacheMediaProvider final : public TimelineMediaProvider {
public:
    // Returns the media's cache, or nullptr while it opens (*failed = false) or when it cannot be opened (*failed = true).
    using CacheFn = std::function<playback::MediaCache*(const project::Id&, bool* failed)>;
    CacheMediaProvider(const project::Project* p, CacheFn fn) : m_project(p), m_cache(std::move(fn)) {}

    MediaStatus Thumbnail(const project::Id& media, TimeUs t, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) override;
    const float* PeaksMinMax(const project::Id& media, int stream, size_t* pair_count, int* per_second,
                             MediaStatus* status) override;
    void BeginFrame() { m_pinned.clear(); }
    void Clear();   // project replaced / caches closed
    size_t Pinned() const { return m_pinned.size(); }

private:
    struct PeakEntry {
        std::vector<float> pairs;
        double first_request_s = -1;
        bool unavailable = false;
        int per_second = 100;
    };
    const project::Project* m_project;
    CacheFn m_cache;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_pinned;
    std::map<std::string, PeakEntry> m_peaks;   // "<path>#<stream>"
    std::string m_key;                          // scratch
};

} // namespace ct::ui::detail
