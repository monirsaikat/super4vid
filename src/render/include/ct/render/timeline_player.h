#pragma once
// ct/render/timeline_player.h
// Plays a whole project: one MediaCache per media file (opened lazily), conversion of decoded frames into the
// compositor's FrameInputs, the timeline audio mix (an AudioEngine::Source fed by a mixer thread), the playhead clock
// and the offline audio render used by the export. The editor owns one instance for its project; every ExportJob owns
// another one (never share MediaCaches between the two: their prefetch hints conflict).
//
// Media resolution: Media::Kind::Recording paths are .ctrec bundles: Source::Screen clips (and Mic/System audio) use
// <bundle>/<meta.screen.file>, Source::Camera clips <bundle>/<meta.camera.file>, Screen clips get the bundle's
// events.ctev as VideoLayerInput::events. Other media are opened by path. Canvas::background.image is decoded once.
// Audio-bearing clips are the clips on (unmuted) audio tracks; clips on video tracks are silent (DECISIONS P10).
//
// Thread model: Init/Shutdown/SetProject/Resolve/HintScrub/Play/Pause/Seek/SetRate/StepFrames/Tick/Cache on one
// control thread (the UI thread for the editor, ct-export for the export). Resolve never blocks: media opens on the
// IO pool and missing frames are prefetched (all_exact = false: render again next frame). ResolveExact and
// RenderAudio block (export/tests only, never the UI thread). Pull runs on the audio render thread (no locks taken
// except try-locks, no allocation). The project must not be mutated while a control-thread call runs; the mixer
// thread works on a private plan rebuilt on the control thread whenever the audio-relevant fields change.
// SRV lifetime: the views in FrameInputs stay valid until the next Resolve/ResolveExact call (every view is
// referenced by the player until then, and the caches keep the last 4 frames per file pinned). Draw them with
// D3DDevice::Lock held.
// Lifetime: Init -> SetProject -> ... -> Shutdown (or the destructor); the project outlives its SetProject period.

#include <ct/audio/devices.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/types.h>
#include <ct/core/ring_buffer.h>
#include <ct/playback/audio_engine.h>
#include <ct/playback/media_cache.h>
#include <ct/playback/playback_clock.h>
#include <ct/project/project.h>
#include <ct/render/frame_inputs.h>
#include <ct/render/render_context.h>

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ct::render {

namespace detail {
class AudioMixer;   // timeline audio mix (src/render/src/audio_mixer.h)
struct AudioPlan;
} // namespace detail

class TimelinePlayer final : public playback::AudioEngine::Source {
public:
    static constexpr int kAudioRate = playback::AudioEngine::kRate;           // 48000
    static constexpr int kAudioChannels = playback::AudioEngine::kChannels;   // 2

    struct Options {
        bool audio_output = true;    // false: no WASAPI stream (export, tests); the clock runs on QPC
        bool async_open = true;      // false: Resolve opens media synchronously on the control thread (tools/tests)
    };

    TimelinePlayer();
    ~TimelinePlayer() override;
    TimelinePlayer(const TimelinePlayer&) = delete;
    TimelinePlayer& operator=(const TimelinePlayer&) = delete;

    // Editor: audio output on the default render endpoint; without one the clock runs on QPC (not an error).
    HRESULT Init(RenderContext&, audio::AudioDevices&);
    // Addition: devices may be null when !options.audio_output.
    HRESULT Init(RenderContext&, audio::AudioDevices*, const Options&);
    void Shutdown();

    // The pointer stays valid until the next SetProject (or Shutdown). Caches open lazily per media file; media that
    // left the project are closed at the next control-thread call. Pauses and seeks to the project's playhead.
    void SetProject(const project::Project*);
    // Primary file of the media (the recording's screen.mp4, or the file itself); nullptr until it is open.
    playback::MediaCache* Cache(const project::Id& media);

    // Non-blocking frame lookup for every visible video clip at t (+ the clips of active transitions), with the
    // transitions and the background image. all_exact = false when a frame is not the exact one yet (or media is
    // still opening): render it anyway and call again next frame. Returns false without a project.
    bool Resolve(TimeUs t, FrameInputs& out, bool& all_exact);
    // Export path: opens media synchronously and waits up to timeout_ms per frame for the exact frames. False on
    // timeout (out then holds the nearest frames, like Resolve) or without a project.
    bool ResolveExact(TimeUs t, FrameInputs& out, uint32_t timeout_ms);

    playback::PlaybackClock& Clock() { return m_clock; }
    playback::AudioEngine& Audio() { return m_engine; }

    void Play();                 // from Now() (from 0 at the end), at Rate()
    void Pause();
    void Seek(TimeUs);           // clamped to [0, Duration]; keeps playing if playing
    void SetRate(double);        // signed; |rate| 0.25..4 (PlaybackClock::ClampRate); reverse is silent
    void StepFrames(int);        // pauses, then moves by whole canvas frames
    void HintScrub(TimeUs);      // paused prefetch around t for every media visible at t

    // Offline mix of [from, to) into AudioFramesBetween(from, to) interleaved stereo frames at 48 kHz (blocking
    // decode; the same gain/fade/normalize/speed code as playback). Consecutive calls with to == next from are
    // sample-contiguous (decoders are only re-seeked on a jump).
    HRESULT RenderAudio(TimeUs from, TimeUs to, float* interleaved_out);

    // ---- additions ----
    // Frames in [from, to) at 48 kHz: SampleIndex(to) - SampleIndex(from), SampleIndex(t) = floor(t * 48000 / 1e6),
    // so consecutive ranges add up exactly.
    static uint32_t AudioFramesBetween(TimeUs from_us, TimeUs to_us);
    static int64_t AudioSampleIndex(TimeUs t_us);

    // Call once per UI frame: returns the playhead (clock time clamped to [0, Duration]), pauses at the end (or at 0
    // in reverse) and rebuilds the audio plan when the project's audio changed. Cheap when paused.
    TimeUs Tick();
    TimeUs Now() const;          // clock time clamped to [0, Duration]
    bool Playing() const { return m_clock.Playing(); }
    double Rate() const { return m_clock.Rate(); }
    TimeUs Duration() const;     // project duration (0 without a project)
    bool HasAudioOutput() const { return m_engine_ok; }
    // The WASAPI render stream is running (false while paused for more than kEngineIdleStopMs; Play restarts it).
    bool AudioOutputRunning() const { return m_engine_running.load(std::memory_order_acquire); }
    // Media still opening on the IO pool (Resolve returns all_exact = false meanwhile).
    bool MediaPending() const;
    // Camera file of a recording (nullptr when none / not open yet).
    playback::MediaCache* CameraCache(const project::Id& media);
    const events::EventLog* Events(const project::Id& media);
    // Waits until every media referenced by the project is open (tests/tools; never the UI thread).
    bool WaitMediaOpen(uint32_t timeout_ms);
    // Tells the player the project was edited (the audio plan is also re-checked by Tick/Play/Seek on its own).
    void ProjectChanged();

    // AudioEngine::Source (audio render thread).
    uint32_t Pull(float* interleaved, uint32_t frames, TimeUs from_us, double rate) override;

private:
    struct MediaSlot;
    struct Want {   // one frame request of Resolve
        const project::Clip* clip = nullptr;
        playback::MediaCache* cache = nullptr;
        MediaSlot* slot = nullptr;
        TimeUs media_t = 0;    // clip media time (events)
        TimeUs lookup_t = 0;   // frame request time: middle of the nominal frame slot (no 1 us rounding holes)
    };

    MediaSlot* GetSlot(const project::Media&, bool sync);
    void OpenSlot(MediaSlot&);   // blocking (IO pool or control thread)
    void PurgeSlots();
    void CollectWants(TimeUs t, bool sync, bool& all_ready);
    bool FillInputs(TimeUs t, FrameInputs& out, bool wait, uint32_t timeout_ms, bool& all_exact);
    void ResolveBackground(FrameInputs& out, bool sync, bool& all_ready);
    void HintAll(TimeUs t, double rate, bool playing);

    // Audio.
    void RebuildPlanIfChanged(bool force);
    std::shared_ptr<const detail::AudioPlan> BuildPlan(bool realtime);
    uint64_t AudioSignature() const;
    void RestartFeed(TimeUs from_us);
    void FeedMain();

    RenderContext* m_rc = nullptr;
    platform::D3DDevice* m_dev = nullptr;
    Options m_opt;
    const project::Project* m_project = nullptr;

    std::map<project::Id, std::shared_ptr<MediaSlot>> m_slots;
    std::vector<std::shared_ptr<MediaSlot>> m_graveyard;   // removed while their open job ran
    std::atomic<int> m_open_jobs{0};
    Event m_open_idle{true, true};
    std::vector<Want> m_wants;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_held;   // views of the last Resolve

    struct Background {
        std::string path;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        SizeI size;
        std::atomic<int> state{0};   // 0 none, 1 loading, 2 ready, 3 failed
    };
    std::shared_ptr<Background> m_bg;

    // Clock / output.
    playback::AudioEngine m_engine;
    bool m_engine_ok = false;
    playback::PlaybackClock m_clock;

    // Audio plan (control thread builds, mixer thread reads).
    mutable SrwLock m_plan_lock;
    std::shared_ptr<const detail::AudioPlan> m_plan;
    uint64_t m_plan_sig = 0;
    bool m_plan_incomplete = false;   // normalize gains still waiting for peaks
    std::unique_ptr<detail::AudioMixer> m_offline;   // RenderAudio
    std::shared_ptr<const detail::AudioPlan> m_offline_plan;
    uint64_t m_offline_sig = 0;

    // Feed (mixer thread -> ring -> Pull), same protocol as playback::Player.
    struct Feed {
        SrwLock lock;
        AudioRing ring;
        std::vector<float> discard;
        TimeUs next_us = 0;
        std::atomic<uint64_t> req_gen{0};
        std::atomic<int64_t> req_from{0};
        uint64_t data_gen = 0;
    } m_feed;
    Thread m_feed_thread;
    Event m_feed_wake{false, false};
    std::atomic<bool> m_feed_stop{false};
    // Idle power (P11 11.1): while paused the mixer thread sleeps without a timeout and, after kEngineIdleStopMs, stops
    // the WASAPI render stream (100 wake-ups/s rendering silence otherwise). Play restarts it (EnsureEngine). Engine
    // Start/Stop are serialized by m_engine_lock; m_feed_active is written on the control thread.
    static constexpr uint32_t kEngineIdleStopMs = 250;
    void EnsureEngine();      // control thread, before m_clock.Play
    void FeedIdle();          // control thread, after the clock paused
    SrwLock m_engine_lock;
    std::atomic<bool> m_feed_active{false};
    std::atomic<bool> m_engine_running{false};
};

} // namespace ct::render
