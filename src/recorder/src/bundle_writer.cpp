#define CT_LOG_CATEGORY "recorder"
#include <ct/recorder/bundle_writer.h>

#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <cstdio>
#include <vector>

namespace ct::recorder {

namespace {

std::string Join(std::string_view dir, std::string_view name) {
    std::string out(dir);
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    out += '\\';
    out += name;
    return out;
}

} // namespace

bool BundleWriter::Create(std::string_view output_dir, std::string& bundle_path_out) {
    return CreateNamed(output_dir, UtcTime::LocalFileStamp(), bundle_path_out);
}

bool BundleWriter::CreateNamed(std::string_view output_dir, std::string_view stem, std::string& bundle_path_out) {
    m_path.clear();   // a failed Create must never leave the previous bundle as the one Delete() removes
    if (output_dir.empty() || !fs::CreateDirectories(output_dir)) {
        CT_LOG_ERROR("cannot create the output folder %.*s", static_cast<int>(output_dir.size()), output_dir.data());
        return false;
    }
    for (int n = 1; n < 1000; ++n) {
        std::string name(stem);
        if (n > 1) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), " (%d)", n);
            name += suffix;
        }
        name += ".ctrec";
        const std::string path = Join(output_dir, name);
        if (fs::Exists(path)) continue;
        // CreateDirectoryW fails if another process created it meanwhile: try the next suffix then.
        const std::wstring wpath = fs::ToWinPath(path);
        if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
            if (GetLastError() == ERROR_ALREADY_EXISTS) continue;
            CT_LOG_ERROR("CreateDirectoryW(%s) failed (%lu)", path.c_str(), GetLastError());
            return false;
        }
        m_path = path;
        bundle_path_out = path;
        return true;
    }
    return false;
}

std::string BundleWriter::ScreenPath() const { return Join(m_path, "screen.mp4"); }
std::string BundleWriter::CamPath() const { return Join(m_path, "cam.mp4"); }
std::string BundleWriter::EventsPath() const { return Join(m_path, "events.ctev"); }
std::string BundleWriter::MetaPath() const { return Join(m_path, "meta.json"); }

bool BundleWriter::WriteMeta(const Json& meta) {
    if (m_path.empty()) return false;
    std::string text = meta.Dump(2);
    text += '\n';
    if (!fs::WriteAtomic(MetaPath(), text)) {
        CT_LOG_ERROR("writing %s failed", MetaPath().c_str());
        return false;
    }
    return true;
}

bool BundleWriter::SetHidden(bool hidden) {
    if (m_path.empty()) return false;
    const std::wstring w = fs::ToWinPath(m_path);
    if (!SetFileAttributesW(w.c_str(), hidden ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL)) {
        CT_LOG_WARN("SetFileAttributes(%s) failed (%lu)", m_path.c_str(), GetLastError());
        return false;
    }
    return true;
}

std::string BundleWriter::Stem() const {
    const size_t slash = m_path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? m_path : m_path.substr(slash + 1);
    if (name.size() > 6 && name.ends_with(".ctrec")) name.resize(name.size() - 6);
    return name;
}

bool BundleWriter::Rename(std::string_view stem) {
    if (m_path.empty()) return false;
    const size_t slash = m_path.find_last_of("\\/");
    if (slash == std::string::npos) return false;
    const std::string dir = m_path.substr(0, slash);
    const std::wstring from = fs::ToWinPath(m_path);
    for (int n = 1; n < 1000; ++n) {
        std::string name(stem);
        if (n > 1) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), " (%d)", n);
            name += suffix;
        }
        name += ".ctrec";
        const std::string path = Join(dir, name);
        if (path == m_path) return true;
        if (fs::Exists(path)) continue;
        if (!MoveFileExW(from.c_str(), fs::ToWinPath(path).c_str(), 0)) {
            if (GetLastError() == ERROR_ALREADY_EXISTS) continue;
            CT_LOG_WARN("renaming %s to %s failed (%lu)", m_path.c_str(), path.c_str(), GetLastError());
            return false;
        }
        m_path = path;
        return true;
    }
    return false;
}

int BundleWriter::DeleteStaleArmed(std::string_view output_dir) {
    if (output_dir.empty()) return 0;
    const std::wstring pattern = fs::ToWinPath(Join(output_dir, "*.ctrec"));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::string> stale;
    do {
        const DWORD a = fd.dwFileAttributes;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY) || !(a & FILE_ATTRIBUTE_HIDDEN)) continue;
        const std::string path = Join(output_dir, WideToUtf8(fd.cFileName));
        if (fs::Exists(Join(path, "meta.json")) || fs::Exists(Join(path, "events.ctev"))) continue;
        stale.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    int removed = 0;
    for (const std::string& p : stale) {
        if (fs::DeleteTree(p)) {
            ++removed;
            CT_LOG_INFO("removed an unused armed bundle left behind: %s", p.c_str());
        }
    }
    return removed;
}

bool BundleWriter::Delete() {
    if (m_path.empty()) return true;
    const bool ok = fs::DeleteTree(m_path);
    if (!ok) CT_LOG_WARN("could not delete %s", m_path.c_str());
    return ok;
}

Json BuildMetaJson(const BundleMeta& m) {
    Json j = Json::Object();
    j.Set("format", "ctrec");
    j.Set("version", 1);
    j.Set("app_version", m.app_version);
    j.Set("created_utc", m.created_utc);
    Json epoch = Json::Object();
    epoch.Set("qpc", m.epoch_qpc);
    epoch.Set("qpc_frequency", m.qpc_frequency);
    j.Set("epoch", std::move(epoch));
    j.Set("duration_us", m.duration_us);
    if (m.incomplete) j.Set("incomplete", true);

    Json screen = Json::Object();
    screen.Set("file", m.screen_file);
    screen.Set("width", m.width);
    screen.Set("height", m.height);
    screen.Set("fps_num", m.fps_num);
    screen.Set("fps_den", m.fps_den);
    screen.Set("codec", m.codec);
    Json region = Json::Object();
    region.Set("x", m.region.x);
    region.Set("y", m.region.y);
    region.Set("w", m.region.w);
    region.Set("h", m.region.h);
    screen.Set("region", std::move(region));
    Json monitor = Json::Object();
    monitor.Set("device", m.monitor_device);
    monitor.Set("name", m.monitor_name);
    monitor.Set("origin_x", m.monitor_origin_x);
    monitor.Set("origin_y", m.monitor_origin_y);
    monitor.Set("dpi", m.monitor_dpi);
    monitor.Set("hdr", m.monitor_hdr);
    screen.Set("monitor", std::move(monitor));
    screen.Set("scale", m.scale);
    j.Set("screen", std::move(screen));

    if (m.has_camera) {
        Json cam = Json::Object();
        cam.Set("file", m.camera_file);
        cam.Set("width", m.camera_width);
        cam.Set("height", m.camera_height);
        cam.Set("fps_num", m.camera_fps_num);
        cam.Set("fps_den", m.camera_fps_den);
        cam.Set("device", m.camera_device);
        j.Set("camera", std::move(cam));
    } else {
        j.Set("camera", nullptr);
    }

    Json audio = Json::Object();
    const auto audio_json = [](const BundleMeta::Audio& a) -> Json {
        if (!a.present) return Json(nullptr);
        Json o = Json::Object();
        o.Set("stream_index", a.stream_index);
        o.Set("device", a.device);
        o.Set("sample_rate", a.sample_rate);
        o.Set("channels", a.channels);
        return o;
    };
    audio.Set("mic", audio_json(m.mic));
    audio.Set("system", audio_json(m.system_audio));
    j.Set("audio", std::move(audio));

    Json events = Json::Object();
    events.Set("file", m.events_file);
    events.Set("cursor_hz", m.cursor_hz);
    events.Set("keystrokes", m.keystrokes);
    j.Set("events", std::move(events));

    Json system = Json::Object();
    system.Set("os", m.os);
    system.Set("gpu", m.gpu);
    system.Set("encoder", m.encoder);
    j.Set("system", std::move(system));

    Json pauses = Json::Array();
    for (const BundleMeta::Pause& p : m.pauses) {
        Json o = Json::Object();
        o.Set("at_us", p.at_us);
        o.Set("duration_us", p.duration_us);
        pauses.Push(std::move(o));
    }
    j.Set("pauses", std::move(pauses));
    return j;
}

std::string OsVersionString() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                    : nullptr;
    if (!fn || fn(&vi) != 0) return "unknown";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    return buf;
}

} // namespace ct::recorder
