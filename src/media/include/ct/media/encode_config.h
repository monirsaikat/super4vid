#pragma once
// ct/media/encode_config.h
// Encoder settings shared by the recorder (P05) and the exporter (P10), the bitrate defaults, the AAC byte-rate
// rounding and the encode thread's interleaving rule as a pure function (unit tested).
// Thread model: plain values and pure functions; safe from any thread.

#include <ct/core/types.h>

#include <cstdint>

namespace ct::media {

enum class VideoCodec { H264, Hevc };
enum class RateControl { Quality, Vbr, Cbr };

struct VideoEncodeConfig {
    VideoCodec codec = VideoCodec::H264;
    SizeI size;                       // even width and height
    int fps_num = 60, fps_den = 1;
    RateControl rc = RateControl::Quality;
    int quality = 70;                 // 0..100 for Quality mode
    uint32_t bitrate_bps = 0;         // 0 = DefaultBitrate()
    uint32_t max_bitrate_bps = 0;     // 0 = 1.5 x bitrate
    int gop_seconds = 2;
    bool bframes = false;
    // CODECAPI_AVEncCommonLowLatency + MF_LOW_LATENCY. Off by default: in low-latency mode the AMD H.264 MFT writes
    // pic_order_cnt_type 2 streams that the Microsoft H.264 decoder (software and DXVA) does not decode (DECISIONS P04).
    bool low_latency = false;
    bool prefer_hardware = true;
};

struct AudioEncodeConfig {
    int sample_rate = 48000;          // only 48000 is supported (every capture stream is 48 kHz)
    int channels = 2;                 // 1 or 2
    uint32_t bitrate_bps = 192000;    // rounded by AacBytesPerSecond
    const char* label = "audio";      // for logs ("mic", "system"); must outlive the writer (use literals)
};

// H.264: clamp(w * h * fps * 0.09, 2 Mbps, 60 Mbps); HEVC: the H.264 value * 0.65. 1080p60 -> 11.2 Mbps.
uint32_t DefaultBitrate(const VideoEncodeConfig&);
// cfg.bitrate_bps, or DefaultBitrate() when 0.
uint32_t EffectiveBitrate(const VideoEncodeConfig&);
// cfg.max_bitrate_bps, or 1.5 x EffectiveBitrate() when 0 (never below EffectiveBitrate()).
uint32_t EffectiveMaxBitrate(const VideoEncodeConfig&);
// The MF AAC encoder accepts only 12000/16000/20000/24000 bytes per second (96/128/160/192 kbps): bitrate / 8 rounded
// to the nearest of these (ties round up; above 24000 clamps to 24000, below 12000 to 12000).
uint32_t AacBytesPerSecond(uint32_t bitrate_bps);

// One stream's view for the interleaving rule (docs/phases/P04 section 4.4).
struct MuxStreamState {
    bool has_head = false;     // an item is queued
    TimeUs head_us = 0;        // timestamp of the queued head item
    TimeUs frontier_us = 0;    // end time of the newest item the producer queued (0 before the first)
    bool active = false;       // the producer is running (pushed recently, or has just started)
    bool pressure = false;     // the queue is nearly full: write it now rather than let the producer drop
};
// The stream whose head the encode thread writes next, or -1 to wait for more input:
// the smallest head timestamp (ties: lower index, i.e. video first), unless it is more than max_lead_us ahead of the
// slowest active stream's frontier; then a stream under pressure is written anyway (smallest such head first).
// With no active stream (all producers stopped) everything drains in timestamp order.
int ChooseNextStream(const MuxStreamState* streams, int count, TimeUs max_lead_us = 500'000);

} // namespace ct::media
