#pragma once
// ct/recorder/event_recorder.h
// Records cursor, clicks, wheel, keys, window focus, markers and pauses into events.ctev (docs/04 section 2):
//   ct-hooks     (platform::HooksThread): LL mouse/keyboard hooks + foreground WinEvent push fixed-size records
//                into an SPSC ring; a 120 Hz timer polls capture::CursorTracker into its own queues.
//   ct-events-io drains everything every 100 ms into ct::events::EventWriter (shapes before the moves that use
//                them, records merged in time order), derives TYPING bursts, fills in window titles/exe names,
//                flushes to disk every 1 s and on Flush(), re-installs the hooks when their loop stalled.
// Times are SessionClock media time (pause-adjusted); positions are relative to the capture region's top-left.
// Thread model: Start/SetPaused/Marker/Flush/Stop from one controlling thread (the session / UI thread).
// LiveInfo()/LastKeyLabel()/GetStats() from any thread.
// Lifetime: the SessionClock must outlive Stop(). The destructor stops a running recorder.

#include <ct/core/error.h>
#include <ct/core/types.h>
#include <ct/platform/win.h>
#include <ct/recorder/session_clock.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ct::recorder {

// Part of the session config (RecordingSession, after P04); defined here because the event recorder owns its meaning.
enum class KeystrokeMode { Off, ShortcutsOnly, All };

class EventRecorder {
public:
    struct Config {
        RectI region;                                   // capture region, virtual-screen px
        KeystrokeMode keystrokes = KeystrokeMode::ShortcutsOnly;
        int cursor_hz = 120;
        bool focus = true;                              // WINDOW_FOCUS records
        // Tests/tools: injected input with this dwExtraInfo is recorded and then swallowed (see HooksThread).
        ULONG_PTR test_swallow_tag = 0;
    };
    struct Live {
        std::atomic<int32_t> cursor_x{0}, cursor_y{0};   // last recorded cursor position (region px)
        std::atomic<uint64_t> records{0};                 // records written so far
    };
    struct Stats {
        uint64_t records = 0;          // records handed to the writer
        uint64_t bytes = 0;            // bytes flushed to the file
        uint64_t ring_dropped = 0;     // hook events lost because the ring was full
        uint64_t cursor_dropped = 0;   // tracker pushes that found its queue full (retried, not lost)
        uint64_t cursor_polls = 0;
        uint32_t hook_reinstalls = 0;
        uint32_t flush_failures = 0;   // OS write errors (disk full); data stays buffered and is retried
        uint64_t typing_bursts = 0;
    };

    EventRecorder();
    ~EventRecorder();
    EventRecorder(const EventRecorder&) = delete;
    EventRecorder& operator=(const EventRecorder&) = delete;

    // Creates events_path (header flushed), starts ct-events-io and ct-hooks. Recording is active immediately.
    bool Start(const Config& config, std::string_view events_path, const SessionClock& clock, Err* err = nullptr);
    // Writes a PAUSE record at the clock's media time (which stands still while paused, so the order relative to
    // SessionClock::Pause/Resume does not matter). While paused nothing else is recorded.
    void SetPaused(bool paused);
    void Marker(std::string_view label, uint32_t kind = 0);
    // Writes everything recorded so far to the OS and waits (up to timeout_ms) until it is done. Also safe from
    // a crash filter while the recorder runs.
    bool Flush(uint32_t timeout_ms = 1000);
    void Stop();   // stops the hooks, drains, closes TYPING, flushes and closes the file
    bool IsRunning() const;

    const Live& LiveInfo() const;
    void LastKeyLabel(char out[32]) const;   // label of the last recorded key-down ("" if none)
    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::recorder
