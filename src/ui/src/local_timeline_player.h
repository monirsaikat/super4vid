#pragma once
// src/ui/src/local_timeline_player.h (private to ct_ui)
// TEMPORARY stand-in for render::TimelinePlayer (P10b), with the interface of docs/phases/P09 "Core structures":
// Init, SetProject, Cache, Resolve, ResolveExact, Clock, Audio, Play/Pause/Seek/SetRate/StepFrames, HintScrub,
// RenderAudio. Video only: one playback::MediaCache per media file (opened on the IO pool), FrameRef ->
// VideoLayerInput (DECISIONS P08 8.1), events per recording; no audio mix (the clock runs on QPC, RenderAudio writes
// silence). Delete this file when render::TimelinePlayer is on master and switch `EditorPlayer` in editor_player.h.
// Thread model: UI thread (the owner); IO-pool jobs open caches and hand them over through an atomic state.

#include <ct/audio/devices.h>
#include <ct/core/types.h>
#include <ct/events/event_log.h>
#include <ct/playback/audio_engine.h>
#include <ct/playback/media_cache.h>
#include <ct/playback/playback_clock.h>
#include <ct/project/project.h>
#include <ct/render/frame_inputs.h>
#include <ct/render/render_context.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ct::ui::detail {

class LocalTimelinePlayer {
public:
    LocalTimelinePlayer();
    ~LocalTimelinePlayer();
    LocalTimelinePlayer(const LocalTimelinePlayer&) = delete;
    LocalTimelinePlayer& operator=(const LocalTimelinePlayer&) = delete;

    HRESULT Init(render::RenderContext&, audio::AudioDevices&);
    void Shutdown();   // waits for pending opens, closes every cache
    void SetProject(const project::Project*);
    // Screen (or only) video file of the media; nullptr while opening / when it has no video.
    playback::MediaCache* Cache(const project::Id& media);
    bool CacheFailed(const project::Id& media) const;   // adapter extra: the media file could not be opened
    bool Resolve(TimeUs t, render::FrameInputs& out, bool& all_exact);
    bool ResolveExact(TimeUs t, render::FrameInputs& out, uint32_t timeout_ms);
    playback::PlaybackClock& Clock() { return m_clock; }
    playback::AudioEngine& Audio() { return m_audio; }
    void Play();
    void Pause();
    void Seek(TimeUs);
    void SetRate(double);
    void StepFrames(int);
    void HintScrub(TimeUs);
    HRESULT RenderAudio(TimeUs from, TimeUs to, float* interleaved_out);

    // Adapter extras (the editor's status bar / stats).
    size_t CacheBytes() const;
    int OpenCaches() const { return static_cast<int>(m_entries.size()); }
    int PendingOpens() const;
    std::function<void()> on_media_ready;   // UI thread, after a cache finished opening (redraw)

private:
    struct Entry {
        std::string file;                       // resolved video/image file
        std::unique_ptr<playback::MediaCache> cache;
        events::EventLog events;
        bool has_events = false;
        int rotation = 0;
        std::atomic<int> state{0};              // 0 opening, 1 ready, 2 failed
        HRESULT hr = S_OK;
    };
    enum class Stream { Screen, Camera };
    Entry* Acquire(const project::Media& m, Stream s);
    Entry* AcquireFile(const std::string& key, std::string bundle, std::string file, bool recording, bool camera);
    void ResolveLayers(TimeUs t, render::FrameInputs& out, bool& all_exact, bool hint, double rate, bool playing);
    void HintAt(TimeUs t, double rate, bool playing);

    platform::D3DDevice* m_dev = nullptr;
    const project::Project* m_project = nullptr;
    std::map<std::string, std::shared_ptr<Entry>> m_entries;   // key: media path + "|s" / "|c", or "bg|" + path
    std::atomic<int> m_pending{0};
    playback::PlaybackClock m_clock;
    playback::AudioEngine m_audio;
    std::vector<const project::Clip*> m_clips;   // Resolve scratch
};

} // namespace ct::ui::detail
