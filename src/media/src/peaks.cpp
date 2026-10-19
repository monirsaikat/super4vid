#define CT_LOG_CATEGORY "media"
#include <ct/media/peaks.h>

#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/media/audio_decoder.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>

#include <shlobj.h>

#include <algorithm>
#include <cstring>

namespace ct::media {

namespace {

constexpr uint32_t kMagic = 0x4B505443;   // "CTPK"
constexpr uint32_t kVersion = 1;

struct Header {
    uint32_t magic;
    uint32_t version;
    int32_t per_second;
    uint32_t count;
    int64_t duration_us;
};

std::string CacheDir() {
    // Paths::Init() (the app) knows about portable mode: <logs dir>\..\cache. Tools and tests fall back to LocalAppData.
    const std::string& logs = platform::Paths::LogsDir();
    if (!logs.empty()) return PathJoin(PathParent(logs), "cache");
    PWSTR local = nullptr;
    std::string dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        dir = PathJoin(PathJoin(WideToUtf8(local), "super4vid"), "cache");
    }
    CoTaskMemFree(local);
    return dir;
}

} // namespace

std::string Peaks::CachePath(std::string_view media_path, int stream) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(Utf8ToWide(media_path).c_str(), GetFileExInfoStandard, &fa)) return std::string();
    const std::string key = Format("%.*s|%lu.%lu|%lu.%lu|%d", static_cast<int>(media_path.size()), media_path.data(),
                                   fa.nFileSizeHigh, fa.nFileSizeLow, fa.ftLastWriteTime.dwHighDateTime,
                                   fa.ftLastWriteTime.dwLowDateTime, stream);
    const std::string dir = CacheDir();
    if (dir.empty()) return std::string();
    return PathJoin(dir, Format("%016llx.peaks", static_cast<unsigned long long>(Fnv1a64(key.data(), key.size()))));
}

HRESULT Peaks::Build(std::string_view path, int stream, Peaks& out, const std::atomic<bool>* stop) {
    out = Peaks{};
    AudioDecoder dec;
    AudioDecoder::Config cfg;
    cfg.path = std::string(path);
    cfg.stream_index = stream;
    CT_RETURN_IF_FAILED(dec.Init(cfg));
    const uint32_t window = static_cast<uint32_t>(AudioDecoder::kRate / out.per_second);   // 480 frames
    if (dec.Duration() > 0) {
        const size_t expect = static_cast<size_t>(dec.Duration() * out.per_second / 1'000'000 + 2);
        out.min.reserve(expect);
        out.max.reserve(expect);
    }
    std::vector<float> buf(static_cast<size_t>(window) * 16 * 2);
    uint32_t in_window = 0;
    float lo = 0, hi = 0;
    uint64_t total = 0;
    for (;;) {
        if (stop && stop->load(std::memory_order_relaxed)) return E_ABORT;
        uint32_t n = 0;
        TimeUs pts = 0;
        const HRESULT hr = dec.Read(buf.data(), window * 16, n, pts);
        CT_RETURN_IF_FAILED(hr);
        if (hr == S_FALSE) break;
        for (uint32_t i = 0; i < n; ++i) {
            const float m = 0.5f * (buf[2 * i] + buf[2 * i + 1]);
            if (in_window == 0) lo = hi = m;
            lo = std::min(lo, m);
            hi = std::max(hi, m);
            if (++in_window == window) {
                out.min.push_back(lo);
                out.max.push_back(hi);
                in_window = 0;
            }
        }
        total += n;
    }
    if (in_window > 0) {
        out.min.push_back(lo);
        out.max.push_back(hi);
    }
    out.duration_us = static_cast<TimeUs>(total * 1'000'000 / AudioDecoder::kRate);
    return S_OK;
}

bool Peaks::LoadCache(std::string_view media_path, int stream, Peaks& out) {
    const std::string file = CachePath(media_path, stream);
    if (file.empty()) return false;
    std::string bytes;
    if (!fs::ReadAll(file, bytes) || bytes.size() < sizeof(Header)) return false;
    Header h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.magic != kMagic || h.version != kVersion || h.per_second <= 0 ||
        bytes.size() != sizeof(Header) + static_cast<size_t>(h.count) * 2 * sizeof(float)) {
        return false;
    }
    out.per_second = h.per_second;
    out.duration_us = h.duration_us;
    out.min.resize(h.count);
    out.max.resize(h.count);
    std::memcpy(out.min.data(), bytes.data() + sizeof(Header), h.count * sizeof(float));
    std::memcpy(out.max.data(), bytes.data() + sizeof(Header) + h.count * sizeof(float), h.count * sizeof(float));
    return true;
}

void Peaks::SaveCache(std::string_view media_path, int stream, const Peaks& p) {
    const std::string file = CachePath(media_path, stream);
    if (file.empty() || p.min.size() != p.max.size()) return;
    Header h{kMagic, kVersion, p.per_second, static_cast<uint32_t>(p.min.size()), p.duration_us};
    std::string bytes(sizeof(Header) + p.min.size() * 2 * sizeof(float), '\0');
    std::memcpy(bytes.data(), &h, sizeof(h));
    std::memcpy(bytes.data() + sizeof(h), p.min.data(), p.min.size() * sizeof(float));
    std::memcpy(bytes.data() + sizeof(h) + p.min.size() * sizeof(float), p.max.data(), p.max.size() * sizeof(float));
    fs::CreateDirectories(PathParent(file));
    if (!fs::WriteAtomic(file, bytes)) CT_LOG_WARN("Peaks: cannot write %s", file.c_str());
}

} // namespace ct::media
