// webcam_demo: lists webcams and their native modes; with a device, captures through capture::WebcamSource and reports
// fps, timestamp jitter, frames and drops, and saves one frame as PNG (NV12 -> RGB on the CPU via a staging copy).
//
//   webcam_demo                                   list devices + modes
//   webcam_demo --device N [--seconds 5] [--size 1280x720] [--fps 30] [--png out.png]
//   webcam_demo --synthetic [--memory] [...]      same pipeline over the synthetic colour-bar reader (no camera needed)

#include <ct/capture/webcam_source.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/image_io.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    int device = -1;
    bool synthetic = false;
    bool memory = false;
    int seconds = 5;
    SizeI size{1280, 720};
    int fps = 30;
    std::string png = "webcam_demo.png";
};

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (s == "--device" && v) a.device = std::atoi(argv[++i]);
        else if (s == "--synthetic") a.synthetic = true;
        else if (s == "--memory") a.memory = true;
        else if (s == "--seconds" && v) a.seconds = std::max(1, std::atoi(argv[++i]));
        else if (s == "--fps" && v) a.fps = std::max(1, std::atoi(argv[++i]));
        else if (s == "--png" && v) a.png = argv[++i];
        else if (s == "--size" && v) {
            if (std::sscanf(argv[++i], "%dx%d", &a.size.w, &a.size.h) != 2) return false;
        } else return false;
    }
    return true;
}

std::string FourCC(uint32_t f) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xFF);
        s.push_back(c >= 32 && c < 127 ? c : '?');
    }
    return s;
}

struct DemoSink : FrameSink {
    platform::TexturePool* pool = nullptr;
    D3DDevice* dev = nullptr;
    ID3D11Texture2D* staging = nullptr;
    uint64_t snapshot_at = 30;
    bool snapshot_taken = false;
    std::vector<TimeUs> times;
    std::mutex mu;
    int errors = 0;
    HRESULT last_error = S_OK;
    bool OnFrame(ID3D11Texture2D* nv12, uint32_t index, TimeUs t_us) override {
        {
            std::lock_guard<std::mutex> l(mu);
            if (!snapshot_taken && times.size() == snapshot_at && staging) {
                dev->Context()->CopyResource(staging, nv12);   // D3DDevice::Lock is held by the caller
                snapshot_taken = true;
            }
            times.push_back(t_us);
        }
        pool->Release(index);
        return true;
    }
    void OnError(HRESULT hr) override {
        std::lock_guard<std::mutex> l(mu);
        ++errors;
        last_error = hr;
    }
};

uint8_t Clamp8(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5); }

bool SavePng(D3DDevice& dev, ID3D11Texture2D* staging, const std::string& path) {
    D3D11_TEXTURE2D_DESC d{};
    staging->GetDesc(&d);
    std::vector<uint8_t> rgba(static_cast<size_t>(d.Width) * d.Height * 4);
    {
        D3DDevice::Lock lock(dev);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(dev.Context()->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
        const auto* y_plane = static_cast<const uint8_t*>(m.pData);
        const uint8_t* uv_plane = y_plane + static_cast<size_t>(m.RowPitch) * d.Height;
        for (UINT r = 0; r < d.Height; ++r) {
            for (UINT c = 0; c < d.Width; ++c) {
                // BT.709 limited range -> full-range RGB.
                const double y = (y_plane[r * m.RowPitch + c] - 16.0) * (255.0 / 219.0);
                const uint8_t* uv = uv_plane + (r / 2) * m.RowPitch + (c & ~1u);
                const double u = (uv[0] - 128.0) * (255.0 / 224.0);
                const double v = (uv[1] - 128.0) * (255.0 / 224.0);
                uint8_t* px = &rgba[(static_cast<size_t>(r) * d.Width + c) * 4];
                px[0] = Clamp8(y + 1.5748 * v);
                px[1] = Clamp8(y - 0.1873 * u - 0.4681 * v);
                px[2] = Clamp8(y + 1.8556 * u);
                px[3] = 255;
            }
        }
        dev.Context()->Unmap(staging, 0);
    }
    return platform::ImageIo::WritePngRgba(path.c_str(), rgba.data(), static_cast<int>(d.Width), static_cast<int>(d.Height),
                                           static_cast<int>(d.Width * 4));
}

int ListDevices(const std::vector<WebcamInfo>& cams) {
    if (cams.empty()) {
        std::printf("no webcams found\n");
        return 0;
    }
    for (size_t i = 0; i < cams.size(); ++i) {
        const WebcamInfo& c = cams[i];
        std::printf("[%zu] %s\n     %ls\n     sizes:", i, c.name.c_str(), c.symbolic_link.c_str());
        for (SizeI s : c.sizes) std::printf(" %dx%d", s.w, s.h);
        std::printf("\n");
        for (const WebcamMode& m : c.modes)
            std::printf("     mode %2u: %4dx%-4d %6.2f fps %s\n", m.native_index, m.size.w, m.size.h, m.Fps(),
                        FourCC(m.fourcc).c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::fprintf(stderr, "usage: webcam_demo [--device N | --synthetic [--memory]] [--seconds 5] [--size 1280x720] "
                             "[--fps 30] [--png out.png]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());

    std::vector<WebcamInfo> cams;
    HRESULT hr = WebcamSource::Enumerate(cams, a.device < 0 && !a.synthetic);
    if (FAILED(hr)) {
        std::fprintf(stderr, "Enumerate: %s\n", HrToString(hr).c_str());
        return 1;
    }
    if (a.device < 0 && !a.synthetic) return ListDevices(cams);
    if (!a.synthetic && a.device >= static_cast<int>(cams.size())) {
        std::fprintf(stderr, "device %d not found (%zu webcams)\n", a.device, cams.size());
        return 1;
    }

    D3DDevice dev;
    hr = dev.Init(false);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11 device: %s\n", HrToString(hr).c_str());
        return 1;
    }
    WebcamSource cam;
    const int64_t t_init = Clock::QpcNow();
    if (a.synthetic) {
        hr = cam.InitWithReader(dev, CreateSyntheticWebcamReader(dev, a.size, a.fps, !a.memory), a.size,
                                static_cast<uint32_t>(a.fps), 1);
    } else {
        WebcamSource::Config cfg;
        cfg.symbolic_link = cams[static_cast<size_t>(a.device)].symbolic_link;
        cfg.preferred_size = a.size;
        cfg.preferred_fps = a.fps;
        hr = cam.Init(dev, cfg);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "WebcamSource::Init: %s\n", HrToString(hr).c_str());
        return 1;
    }
    const WebcamMode& nm = cam.NativeMode();
    std::printf("%s: %dx%d NV12 @ %u/%u fps (native %dx%d %.2f fps %s); init %.0f ms\n", cam.DeviceName().c_str(),
                cam.ActualSize().w, cam.ActualSize().h, cam.ActualFpsNum(), cam.ActualFpsDen(), nm.size.w, nm.size.h, nm.Fps(),
                nm.fourcc ? FourCC(nm.fourcc).c_str() : "-", Clock::QpcToUs(Clock::QpcNow(), t_init) / 1000.0);

    D3D11_TEXTURE2D_DESC sd = cam.Nv12Pool().Desc();
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    CT_LOG_IF_FAILED(dev.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()));

    DemoSink sink;
    sink.pool = &cam.Nv12Pool();
    sink.dev = &dev;
    sink.staging = staging.Get();
    sink.snapshot_at = static_cast<uint64_t>(std::max(1, cam.ActualFps()));   // ~1 s in (auto exposure settled)
    sink.times.reserve(static_cast<size_t>(a.seconds) * 120 + 16);
    QpcMediaTimeSource clock(Clock::QpcNow());
    if (!cam.Start(clock, sink)) {
        std::fprintf(stderr, "WebcamSource::Start failed\n");
        return 1;
    }
    Sleep(static_cast<DWORD>(a.seconds) * 1000);
    const int64_t t_stop = Clock::QpcNow();
    cam.Stop();
    const double stop_ms = Clock::QpcToUs(Clock::QpcNow(), t_stop) / 1000.0;

    const WebcamStats& st = cam.Stats();
    std::vector<TimeUs> t;
    {
        std::lock_guard<std::mutex> l(sink.mu);
        t = sink.times;
    }
    const uint64_t drops = st.dropped_pool_empty + st.dropped_sink + st.dropped_late + st.dropped_bad_buffer;
    std::printf("frames %zu, samples %llu, drops %llu (pool %llu sink %llu late %llu buffer %llu), stream ticks %llu, "
                "device ts %llu / fallback %llu, cpu uploads %llu, stop %.1f ms, errors %d\n",
                t.size(), (unsigned long long)st.samples.load(), (unsigned long long)drops,
                (unsigned long long)st.dropped_pool_empty.load(), (unsigned long long)st.dropped_sink.load(),
                (unsigned long long)st.dropped_late.load(), (unsigned long long)st.dropped_bad_buffer.load(),
                (unsigned long long)st.stream_ticks.load(), (unsigned long long)st.device_timestamps.load(),
                (unsigned long long)st.fallback_timestamps.load(), (unsigned long long)st.cpu_uploads.load(), stop_ms,
                sink.errors);
    if (t.size() >= 3) {
        std::vector<double> d;
        for (size_t i = 1; i < t.size(); ++i) d.push_back(static_cast<double>(t[i] - t[i - 1]));
        double mean = 0;
        for (double x : d) mean += x;
        mean /= static_cast<double>(d.size());
        double var = 0, max_dev = 0;
        for (double x : d) {
            var += (x - mean) * (x - mean);
            max_dev = std::max(max_dev, std::abs(x - mean));
        }
        const double sd_us = std::sqrt(var / static_cast<double>(d.size()));
        std::printf("fps %.2f (first %.3f s, last %.3f s); interval mean %.2f ms, jitter sd %.2f ms, max dev %.2f ms, "
                    "min %.2f ms, max %.2f ms\n",
                    1e6 / mean, t.front() / 1e6, t.back() / 1e6, mean / 1000.0, sd_us / 1000.0, max_dev / 1000.0,
                    *std::min_element(d.begin(), d.end()) / 1000.0, *std::max_element(d.begin(), d.end()) / 1000.0);
    }
    int rc = sink.errors ? 1 : 0;
    if (sink.snapshot_taken && staging) {
        if (SavePng(dev, staging.Get(), a.png)) std::printf("saved frame %llu to %s\n", (unsigned long long)sink.snapshot_at, a.png.c_str());
        else {
            std::fprintf(stderr, "PNG write failed\n");
            rc = 1;
        }
    } else {
        std::printf("no frame saved (fewer than %llu frames)\n", (unsigned long long)sink.snapshot_at + 1);
    }
    cam.Shutdown();
    Log::Shutdown();
    return rc;
}
