#pragma once
// ct/audio/audio_sink.h
// What capture streams hand to their consumer: packets of interleaved float frames at 48 kHz stereo on the
// session clock, plus the per-stream configuration and counters shared by WasapiCapture and WasapiLoopback.
// Thread model: AudioSink callbacks run on the producing capture thread ("ct-audio-mic" / "ct-audio-sys"); they
// must not block (copy into a queue/ring and return). Lifetime: AudioPacket::data is valid until OnAudio returns.

#include <ct/core/types.h>

#include <cstdint>
#include <string>

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::audio {

constexpr int kSampleRate = 48000;   // every capture stream is delivered at this rate ...
constexpr int kChannels = 2;         // ... and as interleaved stereo

// A packet of interleaved float frames at 48 kHz stereo. data points into a pool slot owned by the producer;
// valid until the sink returns. Packets of one stream are exactly contiguous: next.t_us is the time of frame
// (frames delivered so far), i.e. t_n = start + floor(n * 1e6 / 48000).
struct AudioPacket {
    TimeUs t_us = 0;
    uint32_t frames = 0;
    const float* data = nullptr;
    int channels = kChannels;
    bool silence_fill = false;   // the whole packet is inserted silence (gap, device switch, starvation)
};

struct AudioSink {
    // Returning false means the consumer could not take the packet (counted as a drop by the producer).
    virtual bool OnAudio(uint32_t stream, const AudioPacket&) = 0;
    // Device lost / failed to reopen. The stream keeps delivering silence and retries on its own.
    virtual void OnAudioError(uint32_t /*stream*/, HRESULT) {}
    virtual ~AudioSink() = default;
};

struct CaptureConfig {
    std::wstring device_id;          // empty = default (eConsole) endpoint
    uint32_t stream_id = 0;          // passed back in every AudioSink call
    uint32_t packet_frames = 480;    // 10 ms; clamped to 48..4800
    bool follow_default_device = true;   // empty device_id: move to the new default; explicit id: fall back to the
                                         // default when that device disappears
};

// Snapshot of a capture stream's counters (readable from any thread while it runs).
struct CaptureStats {
    uint64_t device_packets = 0;      // GetBuffer packets read from WASAPI
    uint64_t packets_delivered = 0;   // AudioSink::OnAudio calls
    uint64_t frames_delivered = 0;
    uint64_t leading_pad_frames = 0;  // silence before the first device frame (the stream starts at t = 0)
    uint64_t frames_inserted = 0;     // after the first packet: gap silence + single-frame drift corrections
    uint64_t frames_dropped = 0;      // overlap + drift trims + frames the sink/pool could not take
    uint64_t discontinuities = 0;     // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY / TIMESTAMP_ERROR packets
    uint64_t reinits = 0;             // device switches (default change, removal, invalidation)
    double drift_ms = 0;              // device timestamp minus aligned stream position at the last packet
    TimeUs last_device_end_us = 0;    // media time just after the last device frame received
    double last_reopen_ms = 0;        // duration of the last successful device switch (close + open + start)
    bool device_open = false;
};

} // namespace ct::audio
