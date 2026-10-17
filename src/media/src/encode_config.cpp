#include <ct/media/encode_config.h>

#include <algorithm>
#include <cstdlib>

namespace ct::media {

uint32_t DefaultBitrate(const VideoEncodeConfig& c) {
    const double fps = c.fps_den > 0 ? static_cast<double>(c.fps_num) / c.fps_den : 60.0;
    double bps = static_cast<double>(c.size.w) * c.size.h * fps * 0.09;
    bps = std::clamp(bps, 2'000'000.0, 60'000'000.0);
    if (c.codec == VideoCodec::Hevc) bps *= 0.65;
    return static_cast<uint32_t>(bps + 0.5);
}

uint32_t EffectiveBitrate(const VideoEncodeConfig& c) { return c.bitrate_bps ? c.bitrate_bps : DefaultBitrate(c); }

uint32_t EffectiveMaxBitrate(const VideoEncodeConfig& c) {
    const uint32_t mean = EffectiveBitrate(c);
    const uint32_t max = c.max_bitrate_bps ? c.max_bitrate_bps : static_cast<uint32_t>(std::min<uint64_t>(mean * 3ull / 2, UINT32_MAX));
    return std::max(max, mean);
}

uint32_t AacBytesPerSecond(uint32_t bitrate_bps) {
    static constexpr uint32_t kRates[] = {12000, 16000, 20000, 24000};
    const int64_t bytes = bitrate_bps / 8;
    uint32_t best = kRates[0];
    int64_t best_d = INT64_MAX;
    for (uint32_t r : kRates) {
        const int64_t d = std::llabs(bytes - static_cast<int64_t>(r));
        if (d <= best_d) {   // <= : a tie picks the higher rate
            best_d = d;
            best = r;
        }
    }
    return best;
}

int ChooseNextStream(const MuxStreamState* s, int count, TimeUs max_lead_us) {
    int smallest = -1, pressured = -1;
    bool any_active = false;
    TimeUs slowest = 0;
    for (int i = 0; i < count; ++i) {
        if (s[i].active) {
            slowest = any_active ? std::min(slowest, s[i].frontier_us) : s[i].frontier_us;
            any_active = true;
        }
        if (!s[i].has_head) continue;
        if (smallest < 0 || s[i].head_us < s[smallest].head_us) smallest = i;
        if (s[i].pressure && (pressured < 0 || s[i].head_us < s[pressured].head_us)) pressured = i;
    }
    if (smallest < 0) return -1;
    if (!any_active || s[smallest].head_us <= slowest + max_lead_us) return smallest;
    return pressured;   // -1 when nothing is under pressure: wait
}

} // namespace ct::media
