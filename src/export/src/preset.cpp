#define CT_LOG_CATEGORY "export"
#include <ct/export/preset.h>

#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/paths.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace ct::exportng {

namespace {

constexpr const char* kContainers[] = {"mp4", "gif", "png", "m4a"};
constexpr const char* kExtensions[] = {".mp4", ".gif", ".png", ".m4a"};
constexpr const char* kCodecs[] = {"h264", "hevc"};
constexpr const char* kRateControls[] = {"quality", "vbr", "cbr"};
constexpr const char* kProfiles[] = {"baseline", "main", "high"};

template <class E, size_t N>
const char* EnumName(E e, const char* const (&names)[N]) {
    const size_t i = static_cast<size_t>(e);
    return i < N ? names[i] : names[0];
}

// Reads members of one JSON object; the first problem is kept in `error` and every later call is a no-op.
class Reader {
public:
    Reader(const Json& obj, std::string prefix, std::string& error) : m_obj(obj), m_prefix(std::move(prefix)), m_error(error) {}

    const Json* Member(std::string_view key) {
        if (!m_error.empty() || !m_obj.Has(key)) return nullptr;
        return &m_obj[key];
    }
    void Fail(std::string_view key, std::string_view what) {
        if (m_error.empty()) m_error = m_prefix + std::string(key) + ": " + std::string(what);
    }
    void String(std::string_view key, std::string& out, size_t max_len) {
        const Json* v = Member(key);
        if (!v) return;
        if (!v->IsString()) return Fail(key, "expected a string");
        if (v->AsString().size() > max_len) return Fail(key, "too long");
        out = v->AsString();
    }
    void Bool(std::string_view key, bool& out) {
        const Json* v = Member(key);
        if (!v) return;
        if (!v->IsBool()) return Fail(key, "expected true or false");
        out = v->AsBool();
    }
    template <class T>
    void Int(std::string_view key, T& out, int64_t lo, int64_t hi) {
        const Json* v = Member(key);
        if (!v) return;
        if (!v->IsNumber()) return Fail(key, "expected a number");
        const double d = v->AsNumber();
        if (!(d >= static_cast<double>(lo) && d <= static_cast<double>(hi)) || std::floor(d) != d)
            return Fail(key, Format("expected an integer in [%lld, %lld]", static_cast<long long>(lo), static_cast<long long>(hi)));
        out = static_cast<T>(v->AsInt64());
    }
    template <class E, size_t N>
    void Enum(std::string_view key, E& out, const char* const (&names)[N]) {
        const Json* v = Member(key);
        if (!v) return;
        if (!v->IsString()) return Fail(key, "expected a string");
        for (size_t i = 0; i < N; ++i)
            if (v->AsString() == names[i]) {
                out = static_cast<E>(i);
                return;
            }
        Fail(key, "unknown value \"" + v->AsString() + "\"");
    }
    // A nested object, or nullptr (missing). A non-object member fails.
    const Json* Object(std::string_view key) {
        const Json* v = Member(key);
        if (!v) return nullptr;
        if (!v->IsObject()) {
            Fail(key, "expected an object");
            return nullptr;
        }
        return v;
    }

private:
    const Json& m_obj;
    std::string m_prefix;
    std::string& m_error;
};

bool ParseInt(std::string_view s, int& out) {
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc() && p == end;
}

// "source" | integer | "num/den"
void ReadFps(Reader& r, Preset::Video& v) {
    const Json* f = r.Member("fps");
    if (!f) return;
    int num = 0, den = 1;
    if (f->IsString()) {
        const std::string& s = f->AsString();
        if (s == "source") {
            v.fps_num = 0;
            v.fps_den = 1;
            return;
        }
        const size_t slash = s.find('/');
        if (slash == std::string::npos || !ParseInt(std::string_view(s).substr(0, slash), num) ||
            !ParseInt(std::string_view(s).substr(slash + 1), den))
            return r.Fail("fps", "expected \"source\", a number or \"num/den\"");
    } else if (f->IsNumber()) {
        const double d = f->AsNumber();
        if (!(d >= 1 && d <= 240) || std::floor(d) != d) return r.Fail("fps", "expected an integer in [1, 240]");
        num = static_cast<int>(d);
    } else {
        return r.Fail("fps", "expected \"source\", a number or \"num/den\"");
    }
    if (num < 1 || den < 1 || den > 1001 || num > 240 * den) return r.Fail("fps", "out of range (1..240 fps)");
    v.fps_num = num;
    v.fps_den = den;
}

Json FpsToJson(const Preset::Video& v) {
    if (v.fps_num <= 0) return Json("source");
    if (v.fps_den == 1) return Json(v.fps_num);
    return Json(Format("%d/%d", v.fps_num, v.fps_den));
}

Preset MakeMp4(const char* id, const char* name, VideoCodec codec, int w, int h, uint32_t kbps, uint32_t max_kbps) {
    Preset p;
    p.id = id;
    p.name = name;
    p.container = Preset::Container::Mp4;
    p.video.codec = codec;
    p.video.width = w;
    p.video.height = h;
    p.video.bitrate_kbps = kbps;
    p.video.max_bitrate_kbps = max_kbps;
    p.video.profile = codec == VideoCodec::Hevc ? "main" : "high";
    return p;
}

Preset MakeGif(const char* id, const char* name, int w, int h, int fps) {
    Preset p;
    p.id = id;
    p.name = name;
    p.container = Preset::Container::Gif;
    p.video.width = w;
    p.video.height = h;
    p.gif.fps = fps;
    return p;
}

} // namespace

const char* ContainerName(Preset::Container c) { return EnumName(c, kContainers); }
const char* ContainerExtension(Preset::Container c) { return EnumName(c, kExtensions); }

std::vector<Preset> Preset::Builtin() {
    std::vector<Preset> list;
    list.push_back(MakeMp4("mp4-720p", "MP4 720p (H.264)", VideoCodec::H264, 1280, 720, 5000, 8000));
    list.push_back(MakeMp4("mp4-1080p", "MP4 1080p (H.264)", VideoCodec::H264, 1920, 1080, 10000, 16000));
    list.push_back(MakeMp4("mp4-1440p", "MP4 1440p (H.264)", VideoCodec::H264, 2560, 1440, 16000, 25000));
    list.push_back(MakeMp4("mp4-4k", "MP4 4K (H.264)", VideoCodec::H264, 3840, 2160, 35000, 50000));
    list.push_back(MakeMp4("mp4-hevc-1080p", "MP4 1080p (HEVC)", VideoCodec::Hevc, 1920, 1080, 6000, 10000));
    list.push_back(MakeMp4("mp4-hevc-4k", "MP4 4K (HEVC)", VideoCodec::Hevc, 3840, 2160, 20000, 32000));
    list.push_back(MakeGif("gif-720p", "GIF 720p", 1280, 720, 20));
    list.push_back(MakeGif("gif-480p", "GIF 480p", 854, 480, 15));
    Preset png;
    png.id = "png-frame";
    png.name = "PNG (current frame)";
    png.container = Container::Png;
    list.push_back(png);
    Preset m4a;
    m4a.id = "m4a-audio";
    m4a.name = "M4A (audio only)";
    m4a.container = Container::M4a;
    list.push_back(m4a);
    return list;
}

const Preset* Preset::FindBuiltin(std::string_view id) {
    static const std::vector<Preset> builtin = Builtin();
    for (const Preset& p : builtin)
        if (p.id == id) return &p;
    return nullptr;
}

bool Preset::IsValidId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

Json Preset::ToJson() const {
    Json j = Json::Object();
    j.Set("id", id);
    j.Set("name", name);
    j.Set("container", ContainerName(container));
    Json v = Json::Object();
    v.Set("codec", EnumName(video.codec, kCodecs));
    v.Set("width", video.width);
    v.Set("height", video.height);
    v.Set("fps", FpsToJson(video));
    v.Set("rate_control", EnumName(video.rc, kRateControls));
    v.Set("bitrate_kbps", video.bitrate_kbps);
    v.Set("max_bitrate_kbps", video.max_bitrate_kbps);
    v.Set("quality", video.quality);
    v.Set("gop_s", video.gop_s);
    v.Set("bframes", video.bframes);
    v.Set("profile", video.profile);
    j.Set("video", std::move(v));
    Json a = Json::Object();
    a.Set("codec", "aac");
    a.Set("bitrate_kbps", audio.bitrate_kbps);
    a.Set("sample_rate", audio.sample_rate);
    a.Set("channels", audio.channels);
    j.Set("audio", std::move(a));
    Json g = Json::Object();
    g.Set("max_colors", gif.max_colors);
    g.Set("dither", gif.dither);
    g.Set("per_frame_palette", gif.per_frame_palette);
    g.Set("fps", gif.fps);
    j.Set("gif", std::move(g));
    j.Set("fast_start", fast_start);
    return j;
}

bool Preset::FromJson(const Json& j, Preset& out, Err* err) {
    if (!j.IsObject()) {
        if (err) err->Set("preset: expected a JSON object");
        return false;
    }
    std::string error;
    Preset p;
    Reader r(j, "preset.", error);
    r.String("id", p.id, 64);
    r.String("name", p.name, 256);
    r.Enum("container", p.container, kContainers);
    r.Bool("fast_start", p.fast_start);
    if (error.empty() && !IsValidId(p.id)) error = "preset.id: expected 1-64 characters of [a-z0-9_-]";
    if (error.empty() && p.name.empty()) p.name = p.id;

    if (const Json* vj = r.Object("video")) {
        Reader v(*vj, "preset.video.", error);
        v.Enum("codec", p.video.codec, kCodecs);
        v.Int("width", p.video.width, 0, 8192);
        v.Int("height", p.video.height, 0, 8192);
        ReadFps(v, p.video);
        v.Enum("rate_control", p.video.rc, kRateControls);
        v.Int("bitrate_kbps", p.video.bitrate_kbps, 100, 500000);
        v.Int("max_bitrate_kbps", p.video.max_bitrate_kbps, 100, 500000);
        v.Int("quality", p.video.quality, 0, 100);
        v.Int("gop_s", p.video.gop_s, 0, 60);
        v.Bool("bframes", p.video.bframes);
        if (const Json* pr = v.Member("profile")) {
            int idx = -1;
            if (pr->IsString())
                for (int i = 0; i < 3; ++i)
                    if (pr->AsString() == kProfiles[i]) idx = i;
            if (idx < 0) v.Fail("profile", "expected \"baseline\", \"main\" or \"high\"");
            else p.video.profile = kProfiles[idx];
        }
        if (error.empty()) {
            const bool w0 = p.video.width == 0, h0 = p.video.height == 0;
            if (w0 != h0) error = "preset.video: width and height must both be 0 (canvas) or both set";
            else if (!w0 && (p.video.width < 16 || p.video.height < 16 || (p.video.width & 1) || (p.video.height & 1)))
                error = "preset.video: width and height must be even and at least 16";
            else if (p.video.max_bitrate_kbps < p.video.bitrate_kbps)
                error = "preset.video.max_bitrate_kbps: must be >= bitrate_kbps";
        }
    }
    if (const Json* aj = r.Object("audio")) {
        Reader a(*aj, "preset.audio.", error);
        if (const Json* c = a.Member("codec"))
            if (!c->IsString() || c->AsString() != "aac") a.Fail("codec", "only \"aac\" is supported");
        a.Int("bitrate_kbps", p.audio.bitrate_kbps, 64, 320);
        a.Int("sample_rate", p.audio.sample_rate, 44100, 48000);
        a.Int("channels", p.audio.channels, 1, 2);
        if (error.empty() && p.audio.sample_rate != 44100 && p.audio.sample_rate != 48000)
            error = "preset.audio.sample_rate: expected 44100 or 48000";
    }
    if (const Json* gj = r.Object("gif")) {
        Reader g(*gj, "preset.gif.", error);
        g.Int("max_colors", p.gif.max_colors, 2, 256);
        g.Bool("dither", p.gif.dither);
        g.Bool("per_frame_palette", p.gif.per_frame_palette);
        g.Int("fps", p.gif.fps, 1, 50);
    }
    if (!error.empty()) {
        if (err) err->Set(error);
        return false;
    }
    out = std::move(p);
    return true;
}

std::string Preset::UserDir() {
    const std::string& settings = platform::Paths::SettingsDir();
    if (settings.empty()) return std::string();
    return PathJoin(settings, "presets");
}

std::vector<Preset> Preset::LoadUser() {
    const std::string dir = UserDir();
    if (dir.empty()) return {};
    return LoadUserFrom(dir);
}

std::vector<Preset> Preset::LoadUserFrom(std::string_view dir) {
    std::vector<Preset> list;
    std::vector<fs::Entry> entries;
    if (!fs::List(dir, entries)) return list;
    for (const fs::Entry& e : entries) {
        if (e.is_dir || !IEquals(PathExt(e.name), ".json")) continue;
        const std::string path = PathJoin(dir, e.name);
        std::string text, parse_error;
        Json j;
        Err err;
        Preset p;
        if (e.size > 1024 * 1024 || !fs::ReadAll(path, text)) {
            CT_LOG_WARN("preset %s: unreadable or larger than 1 MB", path.c_str());
        } else if (!Json::Parse(text, j, &parse_error)) {
            CT_LOG_WARN("preset %s: %s", path.c_str(), parse_error.c_str());
        } else if (!FromJson(j, p, &err)) {
            CT_LOG_WARN("preset %s: %s", path.c_str(), err.message.c_str());
        } else {
            list.push_back(std::move(p));
        }
    }
    std::sort(list.begin(), list.end(), [](const Preset& a, const Preset& b) {
        const std::string la = ToLowerAscii(a.name), lb = ToLowerAscii(b.name);
        return la != lb ? la < lb : a.id < b.id;
    });
    return list;
}

bool Preset::SaveUser(Err* err) const {
    const std::string dir = UserDir();
    if (dir.empty()) {
        if (err) err->Set("The settings folder is not available.");
        return false;
    }
    return SaveUserTo(dir, err);
}

bool Preset::SaveUserTo(std::string_view dir, Err* err) const {
    Preset check;
    Err verr;
    if (!FromJson(ToJson(), check, &verr)) {   // never write a file LoadUser would reject
        if (err) err->Set("Invalid preset: " + verr.message);
        return false;
    }
    if (!fs::CreateDirectories(dir)) {
        if (err) err->Set("Could not create the presets folder.");
        return false;
    }
    const std::string path = PathJoin(dir, id + ".json");
    if (!fs::WriteAtomic(path, ToJson().Dump(2) + "\n")) {
        if (err) err->Set("Could not write " + path + ".");
        return false;
    }
    return true;
}

uint64_t Preset::EstimateBytes(TimeUs duration, SizeI canvas, int canvas_fps) const {
    if (duration < 0) duration = 0;
    const double seconds = static_cast<double>(duration) / kUsPerSecond;
    const double w = video.width > 0 ? video.width : (canvas.w > 0 ? canvas.w : 1920);
    const double h = video.height > 0 ? video.height : (canvas.h > 0 ? canvas.h : 1080);
    const double fps = video.fps_num > 0 ? static_cast<double>(video.fps_num) / video.fps_den
                                         : (canvas_fps > 0 ? canvas_fps : 60);
    const double audio_bps = audio.bitrate_kbps * 1000.0;
    double bytes = 0;
    switch (container) {
    case Container::Mp4: {
        double video_bps = video.bitrate_kbps * 1000.0;
        if (video.rc == RateControl::Quality) {
            // Screen content at quality q: about 0.02 bits per pixel at q=50 up to 0.2 at q=100 (H.264); HEVC ~0.6x.
            const double q = video.quality / 100.0;
            double bpp = 0.005 + 0.195 * q * q * q;
            if (video.codec == VideoCodec::Hevc) bpp *= 0.6;
            video_bps = w * h * fps * bpp;
        }
        bytes = (video_bps + audio_bps) * seconds / 8.0 * 1.01;
        break;
    }
    case Container::Gif: {
        // Frame-differenced screen content: ~0.12 bytes per pixel per frame (dithered more), plus palettes.
        const double gif_fps = gif.fps > 0 ? gif.fps : 20;
        const double frames = std::ceil(seconds * gif_fps);
        const double per_pixel = gif.dither ? 0.15 : 0.1;
        bytes = frames * (w * h * per_pixel + (gif.per_frame_palette ? 3.0 * gif.max_colors : 0) + 20) + 3.0 * 256 + 64;
        break;
    }
    case Container::Png: bytes = w * h * 1.5 + 1024; break;
    case Container::M4a: bytes = audio_bps * seconds / 8.0 * 1.02 + 1024; break;
    }
    return static_cast<uint64_t>(bytes);
}

} // namespace ct::exportng
