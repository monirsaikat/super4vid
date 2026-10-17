#pragma once
// src/render/src/audio_mixer.h (internal)
// The timeline audio mix shared by playback (TimelinePlayer's mixer thread) and export (TimelinePlayer::RenderAudio):
// one sequential AudioDecoder per audio clip, speed through audio::Resampler (rate 48000 * speed -> 48000, pitch
// follows like tape), gain dB with keyframes, linear fades, normalize gain from waveform peaks.
// Thread model: an AudioMixer belongs to one thread (blocking decode). AudioPlan is immutable once built and shared.

#include <ct/audio/resampler.h>
#include <ct/core/types.h>
#include <ct/media/audio_decoder.h>
#include <ct/project/project.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ct::render::detail {

struct AudioClipPlan {
    project::Id clip, media;
    std::string media_path;                 // as in the project (a .ctrec bundle for recordings)
    project::Media::Kind kind = project::Media::Kind::Audio;
    int audio_index = 0;                    // which of the file's audio streams (file order): 0 = first
    TimeUs start_us = 0, end_us = 0, in_us = 0, out_us = 0;
    double speed = 1.0;
    float gain_db = 0;
    project::KeyTrack<float> gain_keys;     // "audio.gain_db", times relative to start_us
    TimeUs fade_in_us = 0, fade_out_us = 0;
    bool normalize = false;
    float norm_gain = -1.f;                 // < 0: unknown (offline mixers compute it from peaks; realtime uses 1)

    float GainAt(double t_us) const;        // linear gain incl. fades and normalize (norm < 0 counts as 1)
};

struct AudioPlan {
    std::vector<AudioClipPlan> clips;       // muted clips and clips on muted tracks are left out
};

// Which audio stream (index among the file's audio streams) a clip plays: recordings map Mic/System through
// Media::audio_streams ("mic", "system"); everything else plays the first stream.
int AudioIndexFor(const project::Media&, const project::Clip&);
// The file that holds a media's audio/screen video: <bundle>/<meta.screen.file> for recordings, else the path.
// Blocking (reads meta.json). Empty on failure.
std::string ResolveMainFile(const project::Media::Kind kind, const std::string& path);
// Normalize gain from peaks over [in_us, out_us): -1 dBFS target, capped at +20 dB; 1 for silence.
float NormalizeGain(const std::vector<float>& mins, const std::vector<float>& maxs, int per_second, TimeUs in_us, TimeUs out_us);

class AudioMixer {
public:
    explicit AudioMixer(bool realtime);
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // Writes `frames` interleaved stereo frames for timeline samples [sample0, sample0 + frames). Clip decoders are
    // only seeked when the request is not contiguous with the previous one for that clip.
    void Mix(const AudioPlan&, int64_t sample0, uint32_t frames, float* out);
    // Opens (and positions) the decoders of clips that start within (sample0, sample0 + lookahead]: keeps decoder
    // creation off the boundary chunk during playback.
    void Prepare(const AudioPlan&, int64_t sample0, int64_t lookahead);
    void Reset();   // closes every decoder
    size_t OpenDecoders() const { return m_states.size(); }

private:
    struct FileInfo {
        bool ok = false;
        std::string file;                // resolved media file
        std::vector<int> audio_streams;  // MediaInfo stream indices in file order
    };
    struct State;
    const FileInfo& File(const AudioClipPlan&);
    State* Get(const AudioClipPlan&);
    bool Open(State&, const AudioClipPlan&);
    void Position(State&, const AudioClipPlan&, int64_t sample);
    void Produce(State&, uint32_t frames, float* out);

    bool m_realtime;
    std::map<std::string, FileInfo> m_files;                 // by media kind + path
    std::map<project::Id, std::unique_ptr<State>> m_states;  // by clip id
    std::vector<float> m_scratch;
};

} // namespace ct::render::detail
