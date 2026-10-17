// MediaCache-backed thumbnails and peaks for the timeline and the media panel.
#include "cache_media_provider.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ct::ui::detail {

using namespace ct::project;

namespace {
constexpr double kPeaksGiveUpS = 30.0;   // a peaks job that never lands (decode error) stops the loading shimmer
}

void CacheMediaProvider::Clear() {
    m_pinned.clear();
    m_peaks.clear();
}

MediaStatus CacheMediaProvider::Thumbnail(const Id& media, TimeUs t, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) {
    const Media* m = m_project ? m_project->FindMedia(media) : nullptr;
    if (!m || m->missing || m->kind == Media::Kind::Audio) return MediaStatus::Unavailable;
    bool failed = false;
    playback::MediaCache* c = m_cache ? m_cache(media, &failed) : nullptr;
    if (!c) return failed ? MediaStatus::Unavailable : MediaStatus::Loading;   // opening on the IO pool
    if (!c->HasVideo() && !c->Info().is_image) return MediaStatus::Unavailable;
    ID3D11ShaderResourceView* srv = nullptr;
    if (!c->Thumbnail(t, srv) || !srv) return MediaStatus::Loading;
    m_pinned.emplace_back(srv);   // AddRef until the frame is rendered
    tex = reinterpret_cast<ImTextureID>(srv);
    uv0 = ImVec2(0, 0);
    uv1 = ImVec2(1, 1);
    return MediaStatus::Ready;
}

const float* CacheMediaProvider::PeaksMinMax(const Id& media, int stream, size_t* n, int* per_second, MediaStatus* status) {
    *n = 0;
    *per_second = 0;
    *status = MediaStatus::Unavailable;
    const Media* m = m_project ? m_project->FindMedia(media) : nullptr;
    if (!m || m->missing || m->kind == Media::Kind::Image) return nullptr;
    m_key = m->path;
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "#%d", stream);
    m_key += suffix;
    PeakEntry& e = m_peaks[m_key];
    if (e.unavailable) return nullptr;
    if (!e.pairs.empty()) {
        *n = e.pairs.size() / 2;
        *per_second = e.per_second;
        *status = MediaStatus::Ready;
        return e.pairs.data();
    }
    const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    if (e.first_request_s < 0) e.first_request_s = now;
    *status = MediaStatus::Loading;
    bool failed = false;
    playback::MediaCache* c = m_cache ? m_cache(media, &failed) : nullptr;
    if (!c && failed) {
        *status = MediaStatus::Unavailable;
        return {};
    }
    if (!c) return nullptr;
    const std::vector<int> streams = c->Info().AudioStreams();
    if (streams.empty()) {
        e.unavailable = true;
        *status = MediaStatus::Unavailable;
        return nullptr;
    }
    // stream = index into Media::audio_streams (MP4 order) = index into the reader's audio streams in file order.
    const int reader_stream = streams[static_cast<size_t>(std::clamp(stream, 0, static_cast<int>(streams.size()) - 1))];
    const media::Peaks* pk = c->GetPeaks(reader_stream);
    if (!pk) {
        if (now - e.first_request_s > kPeaksGiveUpS) {
            e.unavailable = true;
            *status = MediaStatus::Unavailable;
        }
        return nullptr;
    }
    const size_t count = std::min(pk->min.size(), pk->max.size());
    if (count == 0) {
        e.unavailable = true;
        *status = MediaStatus::Unavailable;
        return nullptr;
    }
    e.pairs.resize(count * 2);
    for (size_t i = 0; i < count; ++i) {
        e.pairs[2 * i] = pk->min[i];
        e.pairs[2 * i + 1] = pk->max[i];
    }
    *n = count;
    e.per_second = pk->per_second > 0 ? pk->per_second : 100;
    *per_second = e.per_second;
    *status = MediaStatus::Ready;
    return e.pairs.data();
}

} // namespace ct::ui::detail
