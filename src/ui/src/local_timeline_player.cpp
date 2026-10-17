// TEMPORARY render::TimelinePlayer stand-in (see local_timeline_player.h). Delete when P10b's TimelinePlayer lands.
#define CT_LOG_CATEGORY "ui"
#include "local_timeline_player.h"

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>
#include <ct/playback/cache_budget.h>
#include <ct/project/importer.h>
#include <ct/render/compositor.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ct::ui::detail {

using namespace ct::project;

namespace {

bool IsVideoClip(const Project& p, const Clip& c) {
    if (c.IsAnnotation() || c.media.empty()) return false;
    if (c.source == Clip::Source::Mic || c.source == Clip::Source::System) return false;
    const Track* t = p.FindTrack(c.track);
    if (!t || t->kind != Track::Kind::Video || t->hidden) return false;
    const Media* m = p.FindMedia(c.media);
    return m && m->kind != Media::Kind::Audio && !m->missing;
}

// Liveness token for UI posts that may run after the player is gone.
std::shared_ptr<int> g_dummy;

} // namespace

LocalTimelinePlayer::LocalTimelinePlayer() = default;
LocalTimelinePlayer::~LocalTimelinePlayer() { Shutdown(); }

HRESULT LocalTimelinePlayer::Init(render::RenderContext& rc, audio::AudioDevices&) {
    m_dev = &rc.Device();
    m_clock.Attach(nullptr);   // QPC clock: no audio mix in the stand-in
    return S_OK;
}

void LocalTimelinePlayer::Shutdown() {
    // Opening jobs hold a shared_ptr to their entry and the device pointer: wait for them before closing.
    for (int i = 0; m_pending.load(std::memory_order_acquire) > 0 && i < 10'000; ++i) Sleep(1);
    for (auto& [key, e] : m_entries)
        if (e->cache) e->cache->Close();
    m_entries.clear();
    m_project = nullptr;
}

void LocalTimelinePlayer::SetProject(const Project* p) { m_project = p; }

int LocalTimelinePlayer::PendingOpens() const { return m_pending.load(std::memory_order_relaxed); }

size_t LocalTimelinePlayer::CacheBytes() const { return playback::CacheBudget::Used(); }

LocalTimelinePlayer::Entry* LocalTimelinePlayer::AcquireFile(const std::string& key, std::string bundle, std::string file,
                                                             bool recording, bool camera) {
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        Entry* e = it->second.get();
        return e->state.load(std::memory_order_acquire) == 1 ? e : nullptr;
    }
    if (!m_dev) return nullptr;
    auto e = std::make_shared<Entry>();
    e->cache = std::make_unique<playback::MediaCache>();
    m_entries.emplace(key, e);
    m_pending.fetch_add(1, std::memory_order_acq_rel);
    platform::D3DDevice* dev = m_dev;
    std::atomic<int>* pending = &m_pending;
    std::function<void()>* ready = &on_media_ready;
    Async::Run([e, dev, pending, ready, bundle = std::move(bundle), file = std::move(file), recording, camera]() mutable {
        if (recording) {
            RecordingMeta meta;
            Err err;
            if (ReadRecordingMeta(bundle, &meta, &err)) {
                const std::string& name = camera ? meta.camera_file : meta.screen_file;
                file = bundle + "\\" + (name.empty() ? std::string(camera ? "cam.mp4" : "screen.mp4") : name);
                if (!camera && meta.has_events) e->has_events = e->events.Load(meta.events_file, nullptr);
            } else {
                file = bundle + (camera ? "\\cam.mp4" : "\\screen.mp4");
            }
        }
        e->file = file;
        e->hr = e->cache->Open(*dev, file);
        if (SUCCEEDED(e->hr)) {
            const media::StreamInfo* v = e->cache->Info().Find(e->cache->Info().FirstVideo());
            e->rotation = v ? v->rotation_deg : 0;
        } else {
            CT_LOG_WARN("preview: cannot open %s: %s", file.c_str(), HrToString(e->hr).c_str());
        }
        e->state.store(SUCCEEDED(e->hr) ? 1 : 2, std::memory_order_release);
        // The UI owns `ready` (a member of the player); Shutdown waits for `pending` to reach 0 before the player
        // goes away, and the post below runs on the UI thread before any later Shutdown can complete... except when
        // the player is destroyed with the post still queued, so the post only touches a copy.
        std::function<void()> cb = *ready;
        pending->fetch_sub(1, std::memory_order_acq_rel);
        if (cb) Async::PostToUi(std::move(cb));
    });
    return nullptr;
}

LocalTimelinePlayer::Entry* LocalTimelinePlayer::Acquire(const Media& m, Stream s) {
    const bool rec = m.kind == Media::Kind::Recording;
    const bool cam = rec && s == Stream::Camera;
    std::string key = m.path;
    key += cam ? "|c" : "|s";
    return AcquireFile(key, rec ? m.path : std::string(), rec ? std::string() : m.path, rec, cam);
}

playback::MediaCache* LocalTimelinePlayer::Cache(const Id& media) {
    if (!m_project) return nullptr;
    const Media* m = m_project->FindMedia(media);
    if (!m || m->missing) return nullptr;
    Entry* e = Acquire(*m, Stream::Screen);
    return e ? e->cache.get() : nullptr;
}

bool LocalTimelinePlayer::CacheFailed(const Id& media) const {
    if (!m_project) return true;
    const Media* m = m_project->FindMedia(media);
    if (!m || m->missing) return true;
    const auto it = m_entries.find(m->path + "|s");
    return it != m_entries.end() && it->second->state.load(std::memory_order_acquire) == 2;
}

void LocalTimelinePlayer::ResolveLayers(TimeUs t, render::FrameInputs& out, bool& all_exact, bool hint, double rate,
                                        bool playing) {
    const Project& p = *m_project;
    render::Compositor::ActiveTransitions(p, t, out.active_transitions);
    m_clips.clear();
    for (const Clip& c : p.clips)
        if (c.start_us <= t && t < c.End() && IsVideoClip(p, c)) m_clips.push_back(&c);
    for (const auto& [b, tr] : out.active_transitions) {
        for (const Id* id : {&tr->a, &tr->b}) {
            const Clip* c = p.FindClip(*id);
            if (c && IsVideoClip(p, *c) && std::find(m_clips.begin(), m_clips.end(), c) == m_clips.end()) m_clips.push_back(c);
        }
    }
    std::sort(m_clips.begin(), m_clips.end(), [&](const Clip* a, const Clip* b) {
        const int ta = p.TrackIndex(a->track), tb = p.TrackIndex(b->track);
        return ta != tb ? ta < tb : a->start_us < b->start_us;
    });
    for (const Clip* c : m_clips) {
        const Media* m = p.FindMedia(c->media);
        Entry* e = Acquire(*m, c->source == Clip::Source::Camera ? Stream::Camera : Stream::Screen);
        if (!e) {
            auto it = m_entries.find(m->path + (c->source == Clip::Source::Camera && m->kind == Media::Kind::Recording ? "|c" : "|s"));
            if (it == m_entries.end() || it->second->state.load() == 0) all_exact = false;   // still opening
            continue;
        }
        if (!e->cache->HasVideo() && !e->cache->Info().is_image) continue;
        const TimeUs mt = render::Compositor::ClipMediaTime(*c, t);
        if (hint) e->cache->Hint(mt, rate * c->speed, playing);
        playback::FrameRef ref;
        bool exact = false;
        if (!e->cache->GetVideoFrame(mt, ref, exact)) {
            all_exact = false;
            continue;
        }
        if (!exact) all_exact = false;
        render::VideoLayerInput in;
        if (ref.rgba) {
            in.rgba_srv = ref.rgba;
            in.is_rgba = true;
        } else {
            in.y_srv = ref.y;
            in.uv_srv = ref.uv;
        }
        in.rotation_deg = e->rotation;
        in.video_size = (e->rotation == 90 || e->rotation == 270) ? SizeI{ref.size.h, ref.size.w} : ref.size;
        in.media_t_us = mt;
        in.clip = c;
        in.events = (c->source == Clip::Source::Screen && e->has_events) ? &e->events : nullptr;
        out.layers.push_back(in);
    }
    // Wallpaper background.
    const Background& bg = p.canvas.background;
    if (bg.kind == Background::Kind::Image && !bg.image.empty()) {
        if (Entry* e = AcquireFile("bg|" + bg.image, std::string(), bg.image, false, false)) {
            playback::FrameRef ref;
            bool exact = false;
            if (e->cache->GetVideoFrame(0, ref, exact) && ref.rgba) {
                out.background_image = ref.rgba;
                out.background_image_size = ref.size;
            }
        } else if (m_entries["bg|" + bg.image]->state.load() == 0) {
            all_exact = false;
        }
    }
}

bool LocalTimelinePlayer::Resolve(TimeUs t, render::FrameInputs& out, bool& all_exact) {
    out.layers.clear();
    out.active_transitions.clear();
    out.background_image = nullptr;
    out.background_image_size = {};
    all_exact = true;
    if (!m_project) return false;
    const bool playing = m_clock.Playing();
    ResolveLayers(t, out, all_exact, playing, m_clock.Rate(), playing);
    if (playing) {
        // Prefetch clips starting within 1.5 s whose cache no active clip is using.
        const TimeUs ahead = t + static_cast<TimeUs>(1'500'000 * std::max(1.0, std::abs(m_clock.Rate())));
        for (const Clip& c : m_project->clips) {
            if (c.start_us <= t || c.start_us > ahead || !IsVideoClip(*m_project, c)) continue;
            bool shared = false;
            for (const Clip* a : m_clips) shared |= a->media == c.media && a->source == c.source;
            if (shared) continue;
            const Media* m = m_project->FindMedia(c.media);
            if (Entry* e = Acquire(*m, c.source == Clip::Source::Camera ? Stream::Camera : Stream::Screen))
                if (e->cache->HasVideo()) e->cache->Hint(c.in_us, m_clock.Rate() * c.speed, false);
        }
    }
    return true;
}

bool LocalTimelinePlayer::ResolveExact(TimeUs t, render::FrameInputs& out, uint32_t timeout_ms) {
    const int64_t until = Clock::NowUs() + int64_t(timeout_ms) * 1000;
    bool exact = false;
    do {
        Resolve(t, out, exact);
        if (exact) return true;
        Sleep(2);
    } while (Clock::NowUs() < until);
    return false;
}

void LocalTimelinePlayer::HintAt(TimeUs t, double rate, bool playing) {
    if (!m_project) return;
    for (const Clip& c : m_project->clips) {
        if (c.start_us > t || t >= c.End() || !IsVideoClip(*m_project, c)) continue;
        const Media* m = m_project->FindMedia(c.media);
        if (Entry* e = Acquire(*m, c.source == Clip::Source::Camera ? Stream::Camera : Stream::Screen))
            if (e->cache->HasVideo()) e->cache->Hint(render::Compositor::ClipMediaTime(c, t), rate * c.speed, playing);
    }
}

void LocalTimelinePlayer::Play() {
    m_clock.Play(m_clock.Now(), m_clock.Rate());
    HintAt(m_clock.Now(), m_clock.Rate(), true);
}

void LocalTimelinePlayer::Pause() {
    m_clock.Pause();
    HintAt(m_clock.Now(), 1.0, false);
}

void LocalTimelinePlayer::Seek(TimeUs t) {
    m_clock.Seek(std::max<TimeUs>(0, t));
    HintAt(m_clock.Now(), m_clock.Playing() ? m_clock.Rate() : 1.0, m_clock.Playing());
}

void LocalTimelinePlayer::SetRate(double r) { m_clock.SetRate(r); }

void LocalTimelinePlayer::StepFrames(int n) {
    m_clock.Pause();
    TimeUs frame = 16'667;
    if (m_project && m_project->canvas.fps_num > 0) frame = std::max<TimeUs>(1, FrameTimeUs(1, m_project->canvas.fps_num, m_project->canvas.fps_den));
    Seek(m_clock.Now() + frame * n);
}

void LocalTimelinePlayer::HintScrub(TimeUs t) { HintAt(t, 1.0, false); }

HRESULT LocalTimelinePlayer::RenderAudio(TimeUs from, TimeUs to, float* out) {
    if (to <= from || !out) return E_INVALIDARG;
    const int64_t frames = (to - from) * playback::AudioEngine::kRate / 1'000'000;
    std::memset(out, 0, static_cast<size_t>(frames) * playback::AudioEngine::kChannels * sizeof(float));
    return S_OK;
}

} // namespace ct::ui::detail
