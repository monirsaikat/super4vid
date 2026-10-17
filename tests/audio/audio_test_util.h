#pragma once
// Shared helpers for the audio tests: a scoped MTA apartment (the WASAPI classes need COM on the calling thread) and
// a sink that checks stream contiguity.

#include <ct/audio/audio_sink.h>
#include <ct/platform/win.h>

#include <objbase.h>

#include <atomic>
#include <cmath>

namespace ct::audio::test {

class ComScope {
public:
    ComScope() : m_ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (m_ok) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool m_ok;
};

// Runs on the capture thread; the test thread reads the atomics after Stop().
struct CheckingSink : AudioSink {
    std::atomic<int64_t> frames{0};
    std::atomic<int64_t> packets{0};
    std::atomic<int64_t> silence_frames{0};
    std::atomic<bool> contiguous{true};
    std::atomic<bool> bad_format{false};
    std::atomic<long> errors{0};
    // Peak of |x| over frames after `peak_after_frames` (skips the onset).
    int64_t peak_after_frames = 0;
    std::atomic<float> peak{0.0f};

    bool OnAudio(uint32_t, const AudioPacket& p) override {
        const int64_t n = frames.load(std::memory_order_relaxed);
        if (p.t_us != n * 1'000'000 / kSampleRate) contiguous.store(false);
        if (p.channels != 2 || p.frames == 0 || !p.data) bad_format.store(true);
        if (p.silence_fill) silence_frames.fetch_add(p.frames);
        float pk = peak.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < p.frames; ++i) {
            if (n + i < peak_after_frames) continue;
            const float a = std::fabs(p.data[2 * i]) > std::fabs(p.data[2 * i + 1]) ? std::fabs(p.data[2 * i])
                                                                                   : std::fabs(p.data[2 * i + 1]);
            pk = a > pk ? a : pk;
        }
        peak.store(pk, std::memory_order_relaxed);
        frames.fetch_add(p.frames);
        packets.fetch_add(1);
        return true;
    }
    void OnAudioError(uint32_t, HRESULT) override { errors.fetch_add(1); }
};

} // namespace ct::audio::test
