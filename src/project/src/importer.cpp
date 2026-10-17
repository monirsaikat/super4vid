#include <ct/project/importer.h>

#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/strings.h>
#include <ct/project/edit_ops.h>

#include <algorithm>
#include <utility>

namespace ct::project {

namespace {

bool Fail(Err* err, std::string msg) {
    if (err) err->Set(msg);
    return false;
}

// Even, within the canvas limits.
int CanvasDim(int v) {
    v = std::clamp(v, kMinCanvas, kMaxCanvas);
    return v & ~1;
}

Track* FindTrackByName(Project& p, Track::Kind kind, const char* name) {
    for (Track& t : p.tracks)
        if (t.kind == kind && t.name == name) return &t;
    return nullptr;
}

Id EnsureTrack(Project& p, Track::Kind kind, const char* name) {
    if (Track* t = FindTrackByName(p, kind, name)) return t->id;
    Id id;
    ops::AddTrack(p, kind, name, &id);
    return id;
}

} // namespace

bool ReadRecordingMeta(std::string_view bundle_path, RecordingMeta* out, Err* err) {
    const std::string bundle = fs::Absolute(bundle_path);
    const std::string meta_path = PathJoin(bundle, "meta.json");
    std::string text;
    if (!fs::ReadAll(meta_path, text)) return Fail(err, "This is not a Camtesia recording (meta.json is missing): " + bundle);
    Json j;
    std::string perr;
    if (!Json::Parse(text, j, &perr) || !j.IsObject()) return Fail(err, "The recording's meta.json is damaged (" + perr + ")");
    if (j["format"].AsString() != "ctrec") return Fail(err, "meta.json does not describe a .ctrec recording");
    RecordingMeta m;
    m.version = j.Get("version", 0);
    m.duration_us = j.Get<int64_t>("duration_us", 0);
    const Json& screen = j["screen"];
    m.screen_file = screen.Get<std::string>("file", "screen.mp4");
    m.width = screen.Get("width", 0);
    m.height = screen.Get("height", 0);
    m.fps_num = screen.Get("fps_num", 0);
    m.fps_den = screen.Get("fps_den", 1);
    const Json& region = screen["region"];
    m.region = RectI{region.Get("x", 0), region.Get("y", 0), region.Get("w", m.width), region.Get("h", m.height)};
    if (m.duration_us <= 0) return Fail(err, "The recording is empty (duration 0).");
    if (m.width <= 0 || m.height <= 0 || m.fps_num <= 0 || m.fps_den <= 0)
        return Fail(err, "The recording's meta.json has no valid screen size or frame rate.");
    const Json& cam = j["camera"];
    if (cam.IsObject()) {
        m.has_camera = true;
        m.camera_file = cam.Get<std::string>("file", "cam.mp4");
        m.camera_width = cam.Get("width", 0);
        m.camera_height = cam.Get("height", 0);
    }
    const Json& audio = j["audio"];
    if (audio["mic"].IsObject()) m.mic_stream = audio["mic"].Get("stream_index", 1);
    if (audio["system"].IsObject()) m.system_stream = audio["system"].Get("stream_index", m.mic_stream >= 0 ? 2 : 1);
    std::vector<std::pair<int, const char*>> streams;
    if (m.mic_stream >= 0) streams.push_back({m.mic_stream, "mic"});
    if (m.system_stream >= 0) streams.push_back({m.system_stream, "system"});
    std::stable_sort(streams.begin(), streams.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& s : streams) m.audio_streams.push_back(s.second);
    const Json& events = j["events"];
    if (events.IsObject()) {
        const std::string file = PathJoin(bundle, events.Get<std::string>("file", "events.ctev"));
        if (fs::Exists(file)) {
            m.has_events = true;
            m.events_file = file;
        }
    }
    for (const Json& pz : j["pauses"].Elements())
        m.pauses.push_back({pz.Get<int64_t>("at_us", 0), pz.Get<int64_t>("duration_us", 0)});
    *out = std::move(m);
    return true;
}

bool ImportRecording(Project& p, std::string_view bundle_path, Err* err, ImportOptions options) {
    RecordingMeta meta;
    if (!ReadRecordingMeta(bundle_path, &meta, err)) return false;
    Project q = p;   // all-or-nothing
    const bool new_project = q.clips.empty();

    Media media;
    media.kind = Media::Kind::Recording;
    media.path = fs::Absolute(bundle_path);
    media.duration_us = meta.duration_us;
    media.width = meta.width;
    media.height = meta.height;
    media.fps_num = meta.fps_num;
    media.fps_den = meta.fps_den;
    media.audio_streams = meta.audio_streams;
    media.has_events = meta.has_events;
    media.has_camera = meta.has_camera;
    Id media_id;
    ops::AddMedia(q, std::move(media), &media_id);
    if (q.name.empty()) q.name = std::string(PathStem(fs::Absolute(bundle_path)));
    if (q.id.empty()) q.id = Uuid4();

    const bool has_mic = meta.mic_stream >= 0, has_system = meta.system_stream >= 0;
    const Id screen_track = EnsureTrack(q, Track::Kind::Video, "Screen");
    const Id camera_track = meta.has_camera ? EnsureTrack(q, Track::Kind::Video, "Camera") : Id();
    const Id mic_track = has_mic ? EnsureTrack(q, Track::Kind::Audio, "Mic") : Id();
    const Id system_track = has_system ? EnsureTrack(q, Track::Kind::Audio, "System") : Id();
    EnsureTrack(q, Track::Kind::Annotations, "Callouts");

    const TimeUs start = q.Duration();
    auto add = [&](const Id& track, Clip::Source source, Id* out) {
        Clip c;
        c.track = track;
        c.media = media_id;
        c.source = source;
        c.start_us = start;
        c.in_us = 0;
        c.out_us = meta.duration_us;
        if (source == Clip::Source::Camera) c.pip = Pip{};
        return ops::AddClip(q, std::move(c), out, err);
    };
    Id screen_clip;
    if (!add(screen_track, Clip::Source::Screen, &screen_clip)) return false;
    if (meta.has_camera && !add(camera_track, Clip::Source::Camera, nullptr)) return false;
    if (has_mic && !add(mic_track, Clip::Source::Mic, nullptr)) return false;
    if (has_system && !add(system_track, Clip::Source::System, nullptr)) return false;

    if (new_project) {
        Canvas c = options.apply_default_style ? Canvas{} : q.canvas;
        c.width = CanvasDim(meta.width);
        c.height = CanvasDim(meta.height);
        c.fps_num = meta.fps_num;
        c.fps_den = meta.fps_den;
        if (!ops::SetCanvas(q, c, err)) return false;
    }

    std::string why;
    if (!q.Validate(&why)) return Fail(err, "Import produced an invalid project: " + why);
    p = std::move(q);
    return true;
}

} // namespace ct::project
