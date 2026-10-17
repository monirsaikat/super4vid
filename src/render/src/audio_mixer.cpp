#define CT_LOG_CATEGORY "render"
#include "audio_mixer.h"

#include <ct/audio/dsp.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/media/media_info.h>
#include <ct/media/peaks.h>
#include <ct/platform/win.h>
#include <ct/project/importer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ct::render::detail {

namespace {
constexpr int kRate = 48000;
constexpr int kCh = 2;
constexpr uint32_t kReadFrames = 1024;
constexpr uint32_t kGainBlock = 256;
constexpr double kMinAudioSpeed = 0.125, kMaxAudioSpeed = 8.0;   // Resampler ratio limits

double SampleTimeUs(int64_t sample) { return static_cast<double>(sample) * 1e6 / kRate; }
int64_t SampleAt(TimeUs t) {
    // floor(t * 48000 / 1e6) for any sign
    const int64_t num = t * kRate;
    return num >= 0 ? num / 1'000'000 : -((-num + 999'999) / 1'000'000);
}
} // namespace

float AudioClipPlan::GainAt(double t) const {
    const TimeUs rel = static_cast<TimeUs>(t) - start_us;
    const float db = gain_keys.Empty() ? gain_db : gain_keys.At(rel, gain_db);
    float g = std::pow(10.f, db / 20.f);
    const double into = t - static_cast<double>(start_us);
    const double left = static_cast<double>(end_us) - t;
    if (fade_in_us > 0 && into < static_cast<double>(fade_in_us)) g *= static_cast<float>(std::clamp(into / fade_in_us, 0.0, 1.0));
    if (fade_out_us > 0 && left < static_cast<double>(fade_out_us)) g *= static_cast<float>(std::clamp(left / fade_out_us, 0.0, 1.0));
    if (normalize && norm_gain >= 0.f) g *= norm_gain;
    return g;
}

int AudioIndexFor(const project::Media& m, const project::Clip& c) {
    const char* label = nullptr;
    if (c.source == project::Clip::Source::Mic) label = "mic";
    else if (c.source == project::Clip::Source::System) label = "system";
    if (!label) return 0;
    for (size_t i = 0; i < m.audio_streams.size(); ++i)
        if (m.audio_streams[i] == label) return static_cast<int>(i);
    // Recording without labels: mic first, system second (meta.json order).
    if (c.source == project::Clip::Source::System && m.audio_streams.size() > 1) return 1;
    return 0;
}

std::string ResolveMainFile(project::Media::Kind kind, const std::string& path) {
    if (kind != project::Media::Kind::Recording) return path;
    project::RecordingMeta meta;
    Err err;
    if (!project::ReadRecordingMeta(path, &meta, &err)) {
        CT_LOG_WARN("audio: %s", err.message.c_str());
        return std::string();
    }
    return PathJoin(path, meta.screen_file);
}

float NormalizeGain(const std::vector<float>& mins, const std::vector<float>& maxs, int per_second, TimeUs in_us, TimeUs out_us) {
    const size_t n = std::min(mins.size(), maxs.size());
    if (n == 0 || per_second <= 0) return 1.f;
    size_t i0 = static_cast<size_t>(std::max<int64_t>(0, in_us * per_second / 1'000'000));
    size_t i1 = static_cast<size_t>(std::max<int64_t>(0, (out_us * per_second + 999'999) / 1'000'000));
    i1 = std::min(i1, n);
    if (i0 >= i1) {
        i0 = 0;
        i1 = n;
    }
    float peak = 0;
    for (size_t i = i0; i < i1; ++i) peak = std::max(peak, std::max(std::fabs(mins[i]), std::fabs(maxs[i])));
    if (peak < 1e-5f) return 1.f;
    const float target = std::pow(10.f, -1.f / 20.f);
    return std::min(target / peak, 10.f);   // +20 dB cap
}

struct AudioMixer::State {
    // What the decoder was opened/positioned for; a change of any of these re-seeks.
    std::string file;
    int stream = -1;
    TimeUs start_us = 0, in_us = 0;
    double speed = 1.0;
    media::AudioDecoder dec;
    bool ok = false;
    bool use_rs = false;
    audio::Resampler rs;
    int rs_in_rate = 0;
    std::vector<float> in_buf;
    size_t in_pos = 0, in_len = 0;
    bool eos = false, flushed = false;
    int64_t next_sample = INT64_MIN;   // timeline sample the decoder delivers next
    float norm = -1.f;                 // offline normalize gain
};

AudioMixer::AudioMixer(bool realtime) : m_realtime(realtime) { audio::dsp::Init(); }
AudioMixer::~AudioMixer() = default;

void AudioMixer::Reset() { m_states.clear(); }

const AudioMixer::FileInfo& AudioMixer::File(const AudioClipPlan& c) {
    const std::string key = std::to_string(static_cast<int>(c.kind)) + "|" + c.media_path;
    auto it = m_files.find(key);
    if (it != m_files.end()) return it->second;
    FileInfo fi;
    fi.file = ResolveMainFile(c.kind, c.media_path);
    if (!fi.file.empty()) {
        media::MediaInfo info;
        const HRESULT hr = media::MediaInfo::Load(fi.file, info);
        if (SUCCEEDED(hr)) {
            fi.audio_streams = info.AudioStreams();
            fi.ok = !fi.audio_streams.empty();
        } else {
            CT_LOG_WARN("audio: cannot open %s: %s", fi.file.c_str(), HrToString(hr).c_str());
        }
    }
    return m_files.emplace(key, std::move(fi)).first->second;
}

bool AudioMixer::Open(State& s, const AudioClipPlan& c) {
    const FileInfo& fi = File(c);
    s.ok = false;
    s.file = fi.file;
    s.start_us = c.start_us;
    s.in_us = c.in_us;
    s.speed = c.speed;
    s.next_sample = INT64_MIN;
    if (!fi.ok) return false;
    const int idx = std::clamp(c.audio_index, 0, static_cast<int>(fi.audio_streams.size()) - 1);
    s.stream = fi.audio_streams[static_cast<size_t>(idx)];
    if (c.speed < kMinAudioSpeed || c.speed > kMaxAudioSpeed) return false;   // silent outside the resampler range
    media::AudioDecoder::Config cfg;
    cfg.path = fi.file;
    cfg.stream_index = s.stream;
    s.dec.Shutdown();
    const HRESULT hr = s.dec.Init(cfg);
    if (FAILED(hr)) {
        CT_LOG_WARN("audio: decoder for %s stream %d: %s", fi.file.c_str(), s.stream, HrToString(hr).c_str());
        return false;
    }
    s.rs_in_rate = static_cast<int>(std::lround(kRate * c.speed));
    s.use_rs = s.rs_in_rate != kRate;
    if (s.use_rs && !s.rs.Init(s.rs_in_rate, kRate, kCh)) return false;
    s.in_buf.assign(static_cast<size_t>(kReadFrames) * kCh, 0.f);
    if (c.normalize && !m_realtime && c.norm_gain < 0.f && s.norm < 0.f) {
        media::Peaks p;
        if (media::Peaks::LoadCache(fi.file, s.stream, p) || SUCCEEDED(media::Peaks::Build(fi.file, s.stream, p))) {
            media::Peaks::SaveCache(fi.file, s.stream, p);
            s.norm = NormalizeGain(p.min, p.max, p.per_second, c.in_us, c.out_us);
        } else {
            s.norm = 1.f;
        }
    }
    s.ok = true;
    return true;
}

AudioMixer::State* AudioMixer::Get(const AudioClipPlan& c) {
    auto it = m_states.find(c.clip);
    State* s = nullptr;
    if (it == m_states.end()) {
        auto st = std::make_unique<State>();
        s = st.get();
        m_states.emplace(c.clip, std::move(st));
        Open(*s, c);
        return s;
    }
    s = it->second.get();
    const FileInfo& fi = File(c);
    if (s->file != fi.file || s->speed != c.speed) Open(*s, c);
    else if (s->start_us != c.start_us || s->in_us != c.in_us) {
        s->start_us = c.start_us;
        s->in_us = c.in_us;
        s->next_sample = INT64_MIN;   // position mapping changed: seek
    }
    return s;
}

void AudioMixer::Position(State& s, const AudioClipPlan& c, int64_t sample) {
    const double media_t = static_cast<double>(c.in_us) + (SampleTimeUs(sample) - static_cast<double>(c.start_us)) * c.speed;
    const HRESULT hr = s.dec.Seek(static_cast<TimeUs>(std::llround(std::max(0.0, media_t))));
    s.eos = FAILED(hr);
    s.flushed = false;
    if (s.use_rs) s.rs.Reset();
    s.in_pos = s.in_len = 0;
    s.next_sample = sample;
}

void AudioMixer::Produce(State& s, uint32_t n, float* out) {
    uint32_t got = 0;
    if (!s.use_rs) {
        while (got < n && !s.eos) {
            uint32_t r = 0;
            TimeUs pts = 0;
            const HRESULT hr = s.dec.Read(out + static_cast<size_t>(got) * kCh, n - got, r, pts);
            if (hr != S_OK || r == 0) {
                s.eos = true;
                break;
            }
            got += r;
        }
    } else {
        while (got < n) {
            if (s.in_pos == s.in_len) {
                if (s.eos) {
                    if (!s.flushed) {
                        const size_t k = s.rs.Flush(out + static_cast<size_t>(got) * kCh, n - got);
                        got += static_cast<uint32_t>(k);
                        if (got < n) s.flushed = true;
                        continue;
                    }
                    break;
                }
                uint32_t r = 0;
                TimeUs pts = 0;
                const HRESULT hr = s.dec.Read(s.in_buf.data(), kReadFrames, r, pts);
                if (hr != S_OK || r == 0) {
                    s.eos = true;
                    continue;
                }
                s.in_pos = 0;
                s.in_len = r;
            }
            size_t consumed = 0;
            const size_t k = s.rs.Process(s.in_buf.data() + s.in_pos * kCh, s.in_len - s.in_pos,
                                          out + static_cast<size_t>(got) * kCh, n - got, &consumed);
            s.in_pos += consumed;
            got += static_cast<uint32_t>(k);
            if (k == 0 && consumed == 0) break;   // defensive: no progress
        }
    }
    if (got < n) std::memset(out + static_cast<size_t>(got) * kCh, 0, sizeof(float) * (n - got) * kCh);
}

void AudioMixer::Prepare(const AudioPlan& plan, int64_t sample0, int64_t lookahead) {
    for (const AudioClipPlan& c : plan.clips) {
        const int64_t s0 = SampleAt(c.start_us);
        if (s0 <= sample0 || s0 > sample0 + lookahead) continue;
        State* s = Get(c);
        if (s->ok && s->next_sample != s0) Position(*s, c, s0);
    }
}

void AudioMixer::Mix(const AudioPlan& plan, int64_t sample0, uint32_t frames, float* out) {
    std::memset(out, 0, sizeof(float) * frames * kCh);
    if (m_scratch.size() < static_cast<size_t>(frames) * kCh) m_scratch.resize(static_cast<size_t>(frames) * kCh);
    const int64_t sample1 = sample0 + frames;
    for (const AudioClipPlan& c : plan.clips) {
        const int64_t cs0 = SampleAt(c.start_us);
        const int64_t cs1 = SampleAt(c.end_us);
        const int64_t a = std::max(cs0, sample0);
        const int64_t b = std::min(cs1, sample1);
        if (a >= b) continue;
        State* s = Get(c);
        if (!s->ok) continue;
        if (s->next_sample != a) Position(*s, c, a);
        const uint32_t n = static_cast<uint32_t>(b - a);
        Produce(*s, n, m_scratch.data());
        s->next_sample = b;
        // Gain envelope: exact at every 256-frame (5.3 ms) boundary, linear in between.
        const float extra = (c.normalize && c.norm_gain < 0.f && s->norm >= 0.f) ? s->norm : 1.f;
        float g0 = c.GainAt(SampleTimeUs(a)) * extra;
        for (uint32_t k = 0; k < n; k += kGainBlock) {
            const uint32_t m = std::min(kGainBlock, n - k);
            const float g1 = c.GainAt(SampleTimeUs(a + k + m)) * extra;
            audio::dsp::ApplyGainRamp(m_scratch.data() + static_cast<size_t>(k) * kCh, m, kCh, g0, g1);
            g0 = g1;
        }
        audio::dsp::MixAdd(out + static_cast<size_t>(a - sample0) * kCh, m_scratch.data(), static_cast<size_t>(n) * kCh, 1.f);
    }
    // Close decoders of clips that left the plan or ended a while ago.
    const int64_t keep_after = sample0 - (m_realtime ? 2 * kRate : 0);
    for (auto it = m_states.begin(); it != m_states.end();) {
        bool keep = false;
        for (const AudioClipPlan& c : plan.clips)
            if (c.clip == it->first) {
                keep = SampleAt(c.end_us) >= keep_after;
                break;
            }
        if (keep) ++it;
        else it = m_states.erase(it);
    }
}

} // namespace ct::render::detail
