#define CT_LOG_CATEGORY "render"
#include <ct/render/timeline_player.h>

#include "audio_mixer.h"

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/events/event_log.h>
#include <ct/media/image_io.h>
#include <ct/platform/win.h>
#include <ct/project/importer.h>
#include <ct/render/compositor.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ct::render {

using Microsoft::WRL::ComPtr;

namespace {
constexpr TimeUs kEndSlackUs = 250'000;   // a decode that stops this close to the container's end is "the end"
constexpr size_t kRingFrames = 14'400;   // 300 ms: edits reach the speakers quickly
constexpr uint32_t kFeedChunk = 480;     // 10 ms per mix step
constexpr int64_t kPrepareSamples = 48'000;   // open decoders of clips starting within 1 s

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
template <class T>
uint64_t FnvV(uint64_t h, const T& v) { return Fnv(h, &v, sizeof(v)); }
uint64_t FnvS(uint64_t h, const std::string& s) { return Fnv(FnvV(h, s.size()), s.data(), s.size()); }

bool IsVideoTrack(const project::Project& p, const project::Id& track, int* index) {
    for (size_t i = 0; i < p.tracks.size(); ++i)
        if (p.tracks[i].id == track) {
            *index = static_cast<int>(i);
            return p.tracks[i].kind == project::Track::Kind::Video && !p.tracks[i].hidden;
        }
    return false;
}
} // namespace

struct TimelinePlayer::MediaSlot {
    project::Id id;
    std::string path;
    project::Media::Kind kind = project::Media::Kind::Video;
    std::unique_ptr<playback::MediaCache> main, cam;
    int main_rotation = 0, cam_rotation = 0;
    events::EventLog events;
    bool has_events = false;
    std::atomic<int> state{0};   // 0 new, 1 opening, 2 ready, 3 failed
    // Media time from which a file stopped delivering exact frames (decoded video shorter than the container says).
    TimeUs main_end = INT64_MAX, cam_end = INT64_MAX;
};

TimelinePlayer::TimelinePlayer() = default;
TimelinePlayer::~TimelinePlayer() { Shutdown(); }

int64_t TimelinePlayer::AudioSampleIndex(TimeUs t) {
    const int64_t num = t * kAudioRate;
    return num >= 0 ? num / 1'000'000 : -((-num + 999'999) / 1'000'000);
}

uint32_t TimelinePlayer::AudioFramesBetween(TimeUs from, TimeUs to) {
    const int64_t n = AudioSampleIndex(to) - AudioSampleIndex(from);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

HRESULT TimelinePlayer::Init(RenderContext& rc, audio::AudioDevices& devices) { return Init(rc, &devices, Options{}); }

HRESULT TimelinePlayer::Init(RenderContext& rc, audio::AudioDevices* devices, const Options& opt) {
    Shutdown();
    if (!rc.IsInitialized()) return E_INVALIDARG;
    m_rc = &rc;
    m_dev = &rc.Device();
    m_opt = opt;
    m_offline = std::make_unique<detail::AudioMixer>(false);
    m_engine_ok = false;
    if (opt.audio_output && devices) {
        m_feed.ring.Init(kRingFrames, kAudioChannels);
        m_feed.discard.assign(static_cast<size_t>(4800) * kAudioChannels, 0.0f);
        if (SUCCEEDED(m_engine.Init(*devices))) {
            m_feed_stop = false;
            Thread::Options to;
            to.name = "ct-audio-mix";
            to.priority = 1;   // above normal
            m_engine_ok = m_feed_thread.Start(to, [this] { FeedMain(); });
            if (m_engine_ok && !m_engine.Start(*this)) {
                m_feed_stop = true;
                m_feed_wake.Set();
                m_feed_thread.Join();
                m_engine_ok = false;
            }
        }
        if (!m_engine_ok) CT_LOG_WARN("TimelinePlayer: no audio output; the clock runs on QPC");
    }
    m_clock = playback::PlaybackClock{};
    m_clock.Attach(m_engine_ok ? &m_engine : nullptr);
    m_clock.Seek(0);
    return S_OK;
}

void TimelinePlayer::Shutdown() {
    m_engine.Stop();
    m_feed_stop = true;
    m_feed_wake.Set();
    m_feed_thread.Join();
    m_engine_ok = false;
    while (m_open_jobs.load() > 0) m_open_idle.Wait(100);
    m_held.clear();
    m_wants.clear();
    m_slots.clear();
    m_graveyard.clear();
    m_bg.reset();
    {
        ExclusiveGuard g(m_plan_lock);
        m_plan.reset();
    }
    m_plan_sig = 0;
    m_offline.reset();
    m_offline_plan.reset();
    m_offline_sig = 0;
    m_project = nullptr;
    m_rc = nullptr;
    m_dev = nullptr;
}

void TimelinePlayer::SetProject(const project::Project* p) {
    if (m_clock.Playing()) m_clock.Pause();
    m_held.clear();
    m_project = p;
    PurgeSlots();
    RebuildPlanIfChanged(true);
    if (m_offline) m_offline->Reset();
    m_offline_sig = 0;
    m_clock.Seek(p ? std::clamp<TimeUs>(p->ui.playhead_us, 0, std::max<TimeUs>(p->Duration(), 0)) : 0);
}

void TimelinePlayer::ProjectChanged() {
    PurgeSlots();
    RebuildPlanIfChanged(true);
}

TimeUs TimelinePlayer::Duration() const { return m_project ? m_project->Duration() : 0; }

// --- media slots ---------------------------------------------------------------------------------------------------

void TimelinePlayer::PurgeSlots() {
    for (auto it = m_graveyard.begin(); it != m_graveyard.end();) {
        if ((*it)->state.load(std::memory_order_acquire) == 1) ++it;
        else it = m_graveyard.erase(it);
    }
    for (auto it = m_slots.begin(); it != m_slots.end();) {
        const project::Media* m = m_project ? m_project->FindMedia(it->first) : nullptr;
        if (m && m->path == it->second->path && m->kind == it->second->kind) {
            ++it;
            continue;
        }
        if (it->second->state.load(std::memory_order_acquire) == 1) m_graveyard.push_back(it->second);
        it = m_slots.erase(it);
    }
}

void TimelinePlayer::OpenSlot(MediaSlot& s) {
    std::string main_file = s.path, cam_file;
    if (s.kind == project::Media::Kind::Recording) {
        project::RecordingMeta meta;
        Err err;
        if (!project::ReadRecordingMeta(s.path, &meta, &err)) {
            CT_LOG_WARN("TimelinePlayer: %s", err.message.c_str());
            s.state.store(3, std::memory_order_release);
            return;
        }
        main_file = PathJoin(s.path, meta.screen_file);
        if (meta.has_camera && !meta.camera_file.empty()) cam_file = PathJoin(s.path, meta.camera_file);
        if (meta.has_events) {
            Err eerr;
            s.has_events = s.events.Load(meta.events_file, &eerr);
            if (!s.has_events) CT_LOG_WARN("TimelinePlayer: events %s: %s", meta.events_file.c_str(), eerr.message.c_str());
        }
    }
    auto open = [&](const std::string& file, std::unique_ptr<playback::MediaCache>& cache, int& rotation) {
        auto c = std::make_unique<playback::MediaCache>();
        const HRESULT hr = c->Open(*m_dev, file);
        if (FAILED(hr)) {
            CT_LOG_WARN("TimelinePlayer: cannot open %s: %s", file.c_str(), HrToString(hr).c_str());
            return false;
        }
        const int v = c->Info().FirstVideo();
        if (const media::StreamInfo* si = v >= 0 ? c->Info().Find(v) : nullptr) rotation = si->rotation_deg;
        cache = std::move(c);
        return true;
    };
    const bool ok = open(main_file, s.main, s.main_rotation);
    if (!cam_file.empty()) open(cam_file, s.cam, s.cam_rotation);
    s.state.store(ok ? 2 : 3, std::memory_order_release);
}

TimelinePlayer::MediaSlot* TimelinePlayer::GetSlot(const project::Media& m, bool sync) {
    auto it = m_slots.find(m.id);
    std::shared_ptr<MediaSlot> slot;
    if (it != m_slots.end() && it->second->path == m.path && it->second->kind == m.kind) {
        slot = it->second;
    } else {
        if (it != m_slots.end()) {
            if (it->second->state.load() == 1) m_graveyard.push_back(it->second);
            m_slots.erase(it);
        }
        slot = std::make_shared<MediaSlot>();
        slot->id = m.id;
        slot->path = m.path;
        slot->kind = m.kind;
        m_slots[m.id] = slot;
    }
    int st = slot->state.load(std::memory_order_acquire);
    if (st == 0) {
        if (m.missing) {
            slot->state.store(3);
            return slot.get();
        }
        slot->state.store(1);
        if (sync || !m_opt.async_open) {
            OpenSlot(*slot);
        } else {
            if (m_open_jobs.fetch_add(1) == 0) m_open_idle.Reset();
            Async::Run([this, slot] {
                OpenSlot(*slot);
                if (m_open_jobs.fetch_sub(1) == 1) m_open_idle.Set();
            });
        }
    } else if (st == 1 && sync) {
        while (slot->state.load(std::memory_order_acquire) == 1) m_open_idle.Wait(20);
    }
    return slot.get();
}

playback::MediaCache* TimelinePlayer::Cache(const project::Id& media) {
    auto it = m_slots.find(media);
    if (it == m_slots.end() || it->second->state.load(std::memory_order_acquire) != 2) return nullptr;
    return it->second->main.get();
}

playback::MediaCache* TimelinePlayer::CameraCache(const project::Id& media) {
    auto it = m_slots.find(media);
    if (it == m_slots.end() || it->second->state.load(std::memory_order_acquire) != 2) return nullptr;
    return it->second->cam.get();
}

const events::EventLog* TimelinePlayer::Events(const project::Id& media) {
    auto it = m_slots.find(media);
    if (it == m_slots.end() || it->second->state.load(std::memory_order_acquire) != 2 || !it->second->has_events) return nullptr;
    return &it->second->events;
}

bool TimelinePlayer::MediaPending() const { return m_open_jobs.load() > 0; }

bool TimelinePlayer::WaitMediaOpen(uint32_t timeout_ms) {
    if (!m_project) return true;
    for (const project::Media& m : m_project->media) GetSlot(m, false);
    Stopwatch sw;
    sw.Start();
    while (m_open_jobs.load() > 0) {
        const double left = timeout_ms - sw.ElapsedMs();
        if (left <= 0) return false;
        m_open_idle.Wait(static_cast<uint32_t>(std::min(left, 50.0)) + 1);
    }
    return true;
}

// --- video ---------------------------------------------------------------------------------------------------------

void TimelinePlayer::CollectWants(TimeUs t, bool sync, bool& all_ready) {
    m_wants.clear();
    const project::Project& p = *m_project;
    struct Item { int track; TimeUs start; const project::Clip* clip; };
    std::vector<Item> all;
    std::vector<std::pair<const project::Clip*, const project::Transition*>> trs;
    Compositor::ActiveTransitions(p, t, trs);
    for (const project::Clip& c : p.clips) {
        if (c.IsAnnotation() || c.media.empty()) continue;
        int ti = 0;
        if (!IsVideoTrack(p, c.track, &ti)) continue;
        bool active = c.start_us <= t && t < c.End();
        if (!active)
            for (const auto& tr : trs)
                if (tr.second->a == c.id || tr.second->b == c.id) active = true;
        if (active) all.push_back(Item{ti, c.start_us, &c});
    }
    std::sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.track != b.track ? a.track < b.track : a.start < b.start; });
    for (const Item& it : all) {
        const project::Media* m = p.FindMedia(it.clip->media);
        if (!m) continue;
        MediaSlot* slot = GetSlot(*m, sync);
        const int st = slot->state.load(std::memory_order_acquire);
        if (st == 1) {
            all_ready = false;
            continue;
        }
        if (st != 2) continue;
        playback::MediaCache* cache = it.clip->source == project::Clip::Source::Camera && slot->cam ? slot->cam.get() : slot->main.get();
        if (m->kind == project::Media::Kind::Recording && it.clip->source == project::Clip::Source::Camera && !slot->cam) continue;
        if (!cache || (!cache->HasVideo() && !cache->Info().is_image)) continue;
        Want w;
        w.clip = it.clip;
        w.cache = cache;
        w.slot = slot;
        w.media_t = Compositor::ClipMediaTime(*it.clip, t);
        w.lookup_t = w.media_t;
        if (cache->HasVideo() && cache->FpsNum() > 0 && cache->FpsDen() > 0) {
            // Decoded durations are rounded (16666 us at 60 fps) so frame ends leave 1 us holes before the next pts;
            // asking for the middle of the nominal slot never lands in one.
            const int64_t idx = FrameIndexAt(w.media_t, cache->FpsNum(), cache->FpsDen());
            const TimeUs a = FrameTimeUs(idx, cache->FpsNum(), cache->FpsDen());
            const TimeUs b = FrameTimeUs(idx + 1, cache->FpsNum(), cache->FpsDen());
            w.lookup_t = a + (b - a) / 2;
        }
        m_wants.push_back(w);
    }
}

void TimelinePlayer::ResolveBackground(FrameInputs& out, bool sync, bool& all_ready) {
    out.background_image = nullptr;
    out.background_image_size = {};
    const project::Background& b = m_project->canvas.background;
    if (b.kind != project::Background::Kind::Image || b.image.empty()) return;
    if (!m_bg || m_bg->path != b.image) {
        if (m_bg && m_bg->state.load() == 1) {
            // let the running load finish; it owns its own shared_ptr
        }
        m_bg = std::make_shared<Background>();
        m_bg->path = b.image;
    }
    std::shared_ptr<Background> bg = m_bg;
    int st = bg->state.load(std::memory_order_acquire);
    if (st == 0) {
        bg->state.store(1);
        auto load = [dev = m_dev, bg] {
            ComPtr<ID3D11Texture2D> tex;
            SizeI size;
            HRESULT hr = media::ImageIo::LoadToTexture(*dev, bg->path, tex, size);
            ComPtr<ID3D11ShaderResourceView> srv;
            if (SUCCEEDED(hr)) hr = dev->Device()->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
            if (FAILED(hr)) {
                CT_LOG_WARN("TimelinePlayer: background image %s: %s", bg->path.c_str(), HrToString(hr).c_str());
                bg->state.store(3, std::memory_order_release);
                return;
            }
            bg->tex = tex;
            bg->srv = srv;
            bg->size = size;
            bg->state.store(2, std::memory_order_release);
        };
        if (sync || !m_opt.async_open) {
            load();
        } else {
            if (m_open_jobs.fetch_add(1) == 0) m_open_idle.Reset();
            Async::Run([this, load] {
                load();
                if (m_open_jobs.fetch_sub(1) == 1) m_open_idle.Set();
            });
        }
        st = bg->state.load(std::memory_order_acquire);
    }
    if (st == 1 && sync)
        while ((st = bg->state.load(std::memory_order_acquire)) == 1) m_open_idle.Wait(20);
    if (st == 1) all_ready = false;
    if (st == 2) {
        out.background_image = bg->srv.Get();
        out.background_image_size = bg->size;
        m_held.push_back(bg->srv);
    }
}

bool TimelinePlayer::FillInputs(TimeUs t, FrameInputs& out, bool wait, uint32_t timeout_ms, bool& all_exact) {
    out.layers.clear();
    out.active_transitions.clear();
    out.background_image = nullptr;
    m_held.clear();
    all_exact = true;
    if (!m_project) return false;
    PurgeSlots();
    bool ready = true;
    CollectWants(t, wait, ready);
    ResolveBackground(out, wait, ready);
    Compositor::ActiveTransitions(*m_project, t, out.active_transitions);
    if (!ready) all_exact = false;

    const double rate = wait ? 1.0 : m_clock.Rate();
    const bool playing = !wait && m_clock.Playing();
    // One hint per cache (the first clip using it): different times on one cache would fight over its window.
    playback::MediaCache* hinted[16];
    size_t nh = 0;
    for (const Want& w : m_wants) {
        bool done = false;
        for (size_t i = 0; i < nh; ++i) done |= hinted[i] == w.cache;
        if (done) continue;
        w.cache->Hint(w.lookup_t, rate, playing);
        if (nh < 16) hinted[nh++] = w.cache;
    }
    bool ok = true;
    struct Got { playback::MediaCache* cache; TimeUs t; playback::FrameRef f; bool exact; bool found; };
    std::vector<Got> got;
    got.reserve(m_wants.size());
    for (const Want& w : m_wants) {
        const bool cam = w.cache == w.slot->cam.get();
        TimeUs& known_end = cam ? w.slot->cam_end : w.slot->main_end;
        const Got* prev = nullptr;
        for (const Got& g : got)
            if (g.cache == w.cache && g.t == w.lookup_t) prev = &g;
        playback::FrameRef f;
        bool exact = false, found = false;
        if (prev) {
            f = prev->f;
            exact = prev->exact;
            found = prev->found;
        } else {
            found = w.cache->GetVideoFrame(w.lookup_t, f, exact);
            if (!exact && wait && w.lookup_t < known_end) {
                // Wait for the exact frame; retry while it is evicted between the wait and the lookup (several caches
                // compete for the global CacheBudget).
                Stopwatch ws;
                ws.Start();
                // Close to the container's end the frame may never come (decoded stream shorter): wait less there.
                const TimeUs dur_hint = w.cache->DurationUs();
                const uint32_t budget = (dur_hint > 0 && w.lookup_t >= dur_hint - kEndSlackUs) ? std::min<uint32_t>(timeout_ms, 2000) : timeout_ms;
                for (;;) {
                    const double left = static_cast<double>(budget) - ws.ElapsedMs();
                    const bool arrived = left > 0 && w.cache->WaitForFrame(w.lookup_t, static_cast<uint32_t>(left));
                    found = w.cache->GetVideoFrame(w.lookup_t, f, exact);
                    if (exact) break;
                    if (!arrived) {
                        // Timed out. Near the end of the file the decoded stream is shorter than the container says
                        // (AAC padding): remember that and keep showing the last frame. Anywhere else it is a failure.
                        const TimeUs dur = w.cache->DurationUs();
                        if (found && dur > 0 && w.lookup_t >= dur - kEndSlackUs) known_end = std::min(known_end, w.lookup_t);
                        else ok = false;
                        break;
                    }
                }
            }
            if (!exact && w.lookup_t >= known_end && found) exact = true;   // past the real end: last frame is right
            got.push_back(Got{w.cache, w.lookup_t, f, exact, found});
        }
        if (!exact) all_exact = false;
        if (!found) continue;
        VideoLayerInput L;
        const int rot = cam ? w.slot->cam_rotation : w.slot->main_rotation;
        if (f.rgba) {
            L.rgba_srv = f.rgba;
            L.is_rgba = true;
            L.rgba_premultiplied = true;   // ImageIo decodes to premultiplied BGRA
            m_held.emplace_back(f.rgba);
        } else {
            L.y_srv = f.y;
            L.uv_srv = f.uv;
            m_held.emplace_back(f.y);
            m_held.emplace_back(f.uv);
        }
        L.rotation_deg = f.rgba ? 0 : rot;
        L.video_size = (L.rotation_deg == 90 || L.rotation_deg == 270) ? SizeI{f.size.h, f.size.w} : f.size;
        L.media_t_us = w.media_t;
        L.clip = w.clip;
        L.events = (w.clip->source == project::Clip::Source::Screen && !cam && w.slot->has_events) ? &w.slot->events : nullptr;
        out.layers.push_back(L);
    }
    return ok;
}

bool TimelinePlayer::Resolve(TimeUs t, FrameInputs& out, bool& all_exact) {
    if (!m_project) {
        out.layers.clear();
        out.active_transitions.clear();
        all_exact = true;
        return false;
    }
    FillInputs(t, out, false, 0, all_exact);
    return true;
}

bool TimelinePlayer::ResolveExact(TimeUs t, FrameInputs& out, uint32_t timeout_ms) {
    bool exact = false;
    if (!m_project) {
        out.layers.clear();
        out.active_transitions.clear();
        return false;
    }
    const bool ok = FillInputs(t, out, true, timeout_ms, exact);
    return ok && exact;
}

void TimelinePlayer::HintAll(TimeUs t, double rate, bool playing) {
    if (!m_project) return;
    bool ready = true;
    CollectWants(t, false, ready);
    for (const Want& w : m_wants) w.cache->Hint(w.lookup_t, rate, playing);
}

void TimelinePlayer::HintScrub(TimeUs t) { HintAll(t, 1.0, false); }

// --- transport -----------------------------------------------------------------------------------------------------

TimeUs TimelinePlayer::Now() const { return std::clamp<TimeUs>(m_clock.Now(), 0, std::max<TimeUs>(Duration(), 0)); }

TimeUs TimelinePlayer::Tick() {
    RebuildPlanIfChanged(false);
    const TimeUs end = Duration();
    TimeUs now = m_clock.Now();
    if (m_clock.Playing() && ((m_clock.Rate() > 0 && now >= end) || (m_clock.Rate() < 0 && now <= 0))) {
        m_clock.Pause();
        m_clock.Seek(m_clock.Rate() > 0 ? end : 0);
        now = m_clock.Now();
    }
    return std::clamp<TimeUs>(now, 0, std::max<TimeUs>(end, 0));
}

void TimelinePlayer::Play() {
    if (m_clock.Playing()) return;
    RebuildPlanIfChanged(false);
    TimeUs from = Now();
    if (m_clock.Rate() > 0 && from >= Duration()) from = 0;
    RestartFeed(from);
    m_clock.Play(from, m_clock.Rate());
    HintAll(from, m_clock.Rate(), true);
}

void TimelinePlayer::Pause() {
    if (!m_clock.Playing()) return;
    m_clock.Pause();
    HintAll(Now(), m_clock.Rate(), false);
}

void TimelinePlayer::Seek(TimeUs t) {
    t = std::clamp<TimeUs>(t, 0, std::max<TimeUs>(Duration(), 0));
    RebuildPlanIfChanged(false);
    if (m_clock.Playing()) RestartFeed(t);
    m_clock.Seek(t);
    HintAll(t, m_clock.Rate(), m_clock.Playing());
}

void TimelinePlayer::SetRate(double rate) {
    if (m_clock.Playing()) RestartFeed(Now());
    m_clock.SetRate(rate);
}

void TimelinePlayer::StepFrames(int delta) {
    if (m_clock.Playing()) m_clock.Pause();
    const int num = m_project && m_project->canvas.fps_num > 0 ? m_project->canvas.fps_num : 60;
    const int den = m_project && m_project->canvas.fps_den > 0 ? m_project->canvas.fps_den : 1;
    const int64_t idx = FrameIndexAt(Now(), num, den);
    Seek(FrameTimeUs(std::max<int64_t>(idx + delta, 0), num, den));
}

// --- audio plan ----------------------------------------------------------------------------------------------------

uint64_t TimelinePlayer::AudioSignature() const {
    uint64_t h = 1469598103934665603ull;
    if (!m_project) return h;
    const project::Project& p = *m_project;
    for (const project::Track& t : p.tracks) {
        if (t.kind != project::Track::Kind::Audio) continue;
        h = FnvS(h, t.id);
        h = FnvV(h, t.muted);
    }
    for (const project::Clip& c : p.clips) {
        const project::Track* tr = p.FindTrack(c.track);
        if (!tr || tr->kind != project::Track::Kind::Audio) continue;
        h = FnvS(h, c.id);
        h = FnvS(h, c.media);
        h = FnvV(h, c.source);
        h = FnvV(h, c.start_us);
        h = FnvV(h, c.in_us);
        h = FnvV(h, c.out_us);
        h = FnvV(h, c.speed);
        h = FnvV(h, c.audio.gain_db);
        h = FnvV(h, c.audio.muted);
        h = FnvV(h, c.audio.fade_in_us);
        h = FnvV(h, c.audio.fade_out_us);
        h = FnvV(h, c.audio.normalize);
        auto k = c.keyframes.find("audio.gain_db");
        if (k != c.keyframes.end())
            for (const auto& key : k->second.keys) {
                h = FnvV(h, key.t_us);
                h = FnvV(h, key.v);
                h = FnvV(h, key.ease);
            }
        if (const project::Media* m = p.FindMedia(c.media)) {
            h = FnvS(h, m->path);
            h = FnvV(h, m->kind);
        }
    }
    return h;
}

std::shared_ptr<const detail::AudioPlan> TimelinePlayer::BuildPlan(bool realtime) {
    auto plan = std::make_shared<detail::AudioPlan>();
    if (!m_project) return plan;
    const project::Project& p = *m_project;
    if (realtime) m_plan_incomplete = false;
    for (const project::Clip& c : p.clips) {
        const project::Track* tr = p.FindTrack(c.track);
        if (!tr || tr->kind != project::Track::Kind::Audio || tr->muted || c.audio.muted || c.media.empty()) continue;
        const project::Media* m = p.FindMedia(c.media);
        if (!m || m->missing || m->kind == project::Media::Kind::Image) continue;
        detail::AudioClipPlan a;
        a.clip = c.id;
        a.media = c.media;
        a.media_path = m->path;
        a.kind = m->kind;
        a.audio_index = detail::AudioIndexFor(*m, c);
        a.start_us = c.start_us;
        a.end_us = c.End();
        a.in_us = c.in_us;
        a.out_us = c.out_us;
        a.speed = c.speed;
        a.gain_db = c.audio.gain_db;
        auto k = c.keyframes.find("audio.gain_db");
        if (k != c.keyframes.end()) a.gain_keys = k->second;
        a.fade_in_us = c.audio.fade_in_us;
        a.fade_out_us = c.audio.fade_out_us;
        a.normalize = c.audio.normalize;
        if (a.normalize && realtime) {
            // Peaks come from the media's cache (built on the IO pool); until then the clip plays at unity gain.
            const media::Peaks* peaks = nullptr;
            if (MediaSlot* s = GetSlot(*m, false); s && s->state.load() == 2 && s->main) {
                const std::vector<int> streams = s->main->Info().AudioStreams();
                if (!streams.empty())
                    peaks = s->main->GetPeaks(streams[static_cast<size_t>(std::clamp(a.audio_index, 0, static_cast<int>(streams.size()) - 1))]);
            }
            if (peaks) a.norm_gain = detail::NormalizeGain(peaks->min, peaks->max, peaks->per_second, c.in_us, c.out_us);
            else m_plan_incomplete = true;
        }
        plan->clips.push_back(std::move(a));
    }
    return plan;
}

void TimelinePlayer::RebuildPlanIfChanged(bool force) {
    if (!m_engine_ok) return;   // no playback audio: only RenderAudio mixes (its own plan)
    const uint64_t sig = AudioSignature();
    if (!force && sig == m_plan_sig && !m_plan_incomplete) return;
    std::shared_ptr<const detail::AudioPlan> plan = BuildPlan(true);
    m_plan_sig = sig;
    ExclusiveGuard g(m_plan_lock);
    m_plan = std::move(plan);
}

void TimelinePlayer::RestartFeed(TimeUs from_us) {
    if (!m_engine_ok) return;
    m_feed.req_from.store(from_us, std::memory_order_relaxed);
    m_feed.req_gen.fetch_add(1, std::memory_order_release);
    m_feed_wake.Set();
}

uint32_t TimelinePlayer::Pull(float* out, uint32_t frames, TimeUs from_us, double) {
    const size_t ch = kAudioChannels;
    std::memset(out, 0, sizeof(float) * frames * ch);
    if (!m_feed.lock.TryLockExclusive()) return 0;
    if (m_feed.data_gen != m_feed.req_gen.load(std::memory_order_acquire)) {
        m_feed.lock.UnlockExclusive();
        return 0;
    }
    // next_us holds the ring's next *sample index* (exact bookkeeping, no rounding drift).
    const int64_t want = AudioSampleIndex(from_us);
    int64_t& next = m_feed.next_us;
    while (want > next + 1) {
        const size_t behind = static_cast<size_t>(want - next);
        const size_t n = m_feed.ring.Read(m_feed.discard.data(), std::min<size_t>(behind, m_feed.discard.size() / ch));
        if (n == 0) break;
        next += static_cast<int64_t>(n);
    }
    if (want > next + 1) {
        m_feed.lock.UnlockExclusive();
        return 0;
    }
    uint32_t written = 0;
    if (want + 1 < next) written = static_cast<uint32_t>(std::min<int64_t>(next - want, frames));
    const size_t n = m_feed.ring.Read(out + static_cast<size_t>(written) * ch, frames - written);
    next += static_cast<int64_t>(n);
    m_feed.lock.UnlockExclusive();
    return written + static_cast<uint32_t>(n);
}

void TimelinePlayer::FeedMain() {
    detail::AudioMixer mixer(true);
    std::vector<float> buf(static_cast<size_t>(kFeedChunk) * kAudioChannels);
    uint64_t gen = 0;
    int64_t sample = 0;
    int64_t prepared_at = INT64_MIN;
    while (!m_feed_stop.load()) {
        const uint64_t want = m_feed.req_gen.load(std::memory_order_acquire);
        if (want != gen) {
            gen = want;
            const TimeUs from = m_feed.req_from.load(std::memory_order_relaxed);
            sample = AudioSampleIndex(from);
            {
                ExclusiveGuard g(m_feed.lock);
                m_feed.ring.Clear();
                m_feed.next_us = sample;
                m_feed.data_gen = gen;
            }
            prepared_at = INT64_MIN;
        }
        bool wrote = false;
        while (m_feed.ring.Free() >= kFeedChunk && m_feed.req_gen.load(std::memory_order_acquire) == gen && !m_feed_stop.load()) {
            std::shared_ptr<const detail::AudioPlan> plan;
            {
                SharedGuard g(m_plan_lock);
                plan = m_plan;
            }
            if (!plan) break;
            if (sample - prepared_at >= 4800) {   // every 100 ms
                mixer.Prepare(*plan, sample, kPrepareSamples);
                prepared_at = sample;
            }
            mixer.Mix(*plan, sample, kFeedChunk, buf.data());
            if (m_feed.req_gen.load(std::memory_order_acquire) != gen) break;
            m_feed.ring.Write(buf.data(), kFeedChunk);
            sample += kFeedChunk;
            wrote = true;
        }
        if (m_feed.req_gen.load(std::memory_order_acquire) != gen) continue;
        m_feed_wake.Wait(wrote ? 5 : 10);
    }
}

HRESULT TimelinePlayer::RenderAudio(TimeUs from, TimeUs to, float* out) {
    const uint32_t frames = AudioFramesBetween(from, to);
    if (frames == 0) return S_OK;
    if (!out) return E_POINTER;
    if (!m_project || !m_offline) {
        std::memset(out, 0, sizeof(float) * frames * kAudioChannels);
        return m_project ? E_UNEXPECTED : S_FALSE;
    }
    const uint64_t sig = AudioSignature();
    if (!m_offline_plan || sig != m_offline_sig) {
        m_offline_plan = BuildPlan(false);
        m_offline_sig = sig;
    }
    m_offline->Mix(*m_offline_plan, AudioSampleIndex(from), frames, out);
    return S_OK;
}

} // namespace ct::render
