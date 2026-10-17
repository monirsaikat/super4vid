// ExportJob (MP4/PNG/GIF/M4A, cancel), Scaler, and the helpers, on a fixture project built from tests/data patterns:
// 3 video layers (screen with cursor + keystrokes from a synthetic event log, camera PIP, scaled overlay), 2 annotations,
// mic audio.
#include "../common/gif_reader.h"
#include "../common/nv12_readback.h"
#include "../common/test_device.h"
#include "../common/test_media.h"
#include "../common/test_project.h"

#include <ct/audio/devices.h>
#include <ct/capture/color_convert.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/export/export_job.h>
#include <ct/export/scaler.h>
#include <ct/media/audio_decoder.h>
#include <ct/media/image_io.h>
#include <ct/media/media_info.h>
#include <ct/media/video_decoder.h>
#include <ct/platform/win.h>
#include <ct/render/compositor.h>
#include <ct/render/readback.h>
#include <ct/render/render_context.h>
#include <ct/render/timeline_player.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::exportng;
using Microsoft::WRL::ComPtr;

namespace {

constexpr TimeUs kPatternUs = 5'000'000;

struct Env {
    platform::D3DDevice* dev = nullptr;
    render::RenderContext rc;
    audio::AudioDevices devices;
    std::string bundle, dir;
    bool Init(platform::D3DDevice* d, const std::string& pattern) {
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        dev = d;
        bundle = fs::TempPath(".ctrec");
        dir = fs::TempPath("");
        fs::CreateDirectories(dir);
        return SUCCEEDED(rc.Init(*d)) && test::MakeBundle(bundle, pattern, 1920, 1080, kPatternUs, true);
    }
    ~Env() {
        if (!bundle.empty()) fs::DeleteTree(bundle);
        if (!dir.empty()) fs::DeleteTree(dir);
    }
    std::string Out(const char* name) const { return PathJoin(dir, name); }
};

bool RunJob(Env& env, ExportRequest req, ExportJob::Progress const** out_progress, ExportJob& job, double cancel_at = -1) {
    if (FAILED(job.Start(*env.dev, env.rc, env.devices, std::move(req)))) return false;
    if (cancel_at >= 0) {
        while (!job.GetProgress().finished.load() && job.GetProgress().fraction.load() < cancel_at) Sleep(5);
        job.Cancel();
    }
    if (!job.Join(300'000)) return false;
    *out_progress = &job.GetProgress();
    return true;
}

// The preview of frame t: TimelinePlayer + Compositor at canvas size, read back as BGRA.
bool Preview(Env& env, const project::Project& p, TimeUs t, std::vector<uint8_t>& bgra, ComPtr<ID3D11Texture2D>* keep = nullptr) {
    render::TimelinePlayer tp;
    render::TimelinePlayer::Options o;
    o.audio_output = false;
    o.async_open = false;
    if (FAILED(tp.Init(env.rc, nullptr, o))) return false;
    tp.SetProject(&p);
    render::FrameInputs in;
    if (!tp.ResolveExact(t, in, 10'000)) return false;
    render::Compositor comp;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(p.canvas.width);
    d.Height = static_cast<UINT>(p.canvas.height);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(env.dev->Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf()))) return false;
    if (FAILED(env.dev->Device()->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf()))) return false;
    {
        platform::D3DDevice::Lock lock(*env.dev);
        if (FAILED(comp.Init(env.rc)) || FAILED(comp.RenderFrame(p, t, in, rtv.Get()))) return false;
    }
    SizeI size;
    const bool ok = SUCCEEDED(render::Readback::ToBgra8(*env.dev, tex.Get(), bgra, size));
    {
        platform::D3DDevice::Lock lock(*env.dev);
        comp.Shutdown();
    }
    tp.Shutdown();
    if (keep) *keep = tex;
    return ok;
}

double Psnr(const uint8_t* a, const uint8_t* b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = double(a[i]) - double(b[i]);
        se += d * d;
    }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}

ExportRequest Request(const project::Project& p, const char* preset_id, const std::string& out, TimeUs a = 0, TimeUs b = 0) {
    ExportRequest r;
    r.project_snapshot = p;
    const Preset* pr = Preset::FindBuiltin(preset_id);
    if (pr) r.preset = *pr;
    r.output_path = out;
    r.range_start = a;
    r.range_end = b;
    return r;
}

} // namespace

// ---- unit ---------------------------------------------------------------------------------------------------------

CT_TEST(export_output_size_fps_and_frame_count) {
    project::Canvas c;
    c.width = 1920;
    c.height = 1080;
    Preset p;
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).w, 1920);
    p.video.width = 1280;
    p.video.height = 720;
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).h, 720);
    c.width = 1080;
    c.height = 1920;   // vertical canvas: width kept, height from the canvas aspect
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).w, 1280);
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).h, 2276);
    c.width = 1000;
    c.height = 1000;
    p.video.width = 855;
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).w, 854);
    CT_CHECK_EQ(ExportJob::OutputSize(p, c).h, 854);
    int num = 0, den = 0;
    c.fps_num = 30;
    ExportJob::OutputFps(p, c, num, den);
    CT_CHECK_EQ(num, 30);
    p.video.fps_num = 24;
    ExportJob::OutputFps(p, c, num, den);
    CT_CHECK_EQ(num, 24);
    p.container = Preset::Container::Gif;
    p.gif.fps = 15;
    ExportJob::OutputFps(p, c, num, den);
    CT_CHECK_EQ(num, 15);
    CT_CHECK_EQ(ExportJob::FrameCount(0, 2'000'000, 60, 1), 120);
    CT_CHECK_EQ(ExportJob::FrameCount(0, 2'000'001, 60, 1), 121);
    CT_CHECK_EQ(ExportJob::FrameCount(0, 1'000'000, 30000, 1001), 30);
    CT_CHECK_EQ(ExportJob::FrameCount(5, 5, 60, 1), 0);
    CT_CHECK_EQ(Scaler::BoxPasses({3840, 2160}, {1280, 720}), 1);
    CT_CHECK_EQ(Scaler::BoxPasses({1920, 1080}, {1280, 720}), 0);
    CT_CHECK_EQ(Scaler::BoxPasses({1024, 512}, {256, 128}), 2);
}

// ---- device -------------------------------------------------------------------------------------------------------

CT_TEST_DEVICE(export_scaler_matches_cpu_box_filter) {
    CT_REQUIRE_DEVICE(dev);
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    render::RenderContext rc;
    CT_REQUIRE(SUCCEEDED(rc.Init(*dev)));
    const SizeI src{1024, 512}, dst{256, 128};
    std::vector<uint8_t> px(size_t(src.w) * src.h * 4);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            uint8_t* p = &px[(size_t(y) * src.w + x) * 4];
            p[0] = uint8_t((x * 7 + y * 3) & 255);
            p[1] = uint8_t(((x / 3) ^ (y / 5)) * 37);
            p[2] = uint8_t((x * y) >> 6);
            p[3] = 255;
        }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(src.w);
    d.Height = UINT(src.h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), UINT(src.w * 4), 0};
    ComPtr<ID3D11Texture2D> stex, dtex;
    ComPtr<ID3D11ShaderResourceView> ssrv;
    ComPtr<ID3D11RenderTargetView> drtv;
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateTexture2D(&d, &init, stex.GetAddressOf())));
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateShaderResourceView(stex.Get(), nullptr, ssrv.GetAddressOf())));
    d.Width = UINT(dst.w);
    d.Height = UINT(dst.h);
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateTexture2D(&d, nullptr, dtex.GetAddressOf())));
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateRenderTargetView(dtex.Get(), nullptr, drtv.GetAddressOf())));
    {
        platform::D3DDevice::Lock lock(*dev);
        CT_REQUIRE(SUCCEEDED(Scaler::Downscale(rc, ssrv.Get(), src, drtv.Get(), dst)));
    }
    std::vector<uint8_t> got;
    SizeI gs;
    CT_REQUIRE(SUCCEEDED(render::Readback::ToBgra8(*dev, dtex.Get(), got, gs)));
    // CPU reference: 4x4 box (two 2x box passes, rounded like the GPU's 8-bit intermediate).
    std::vector<uint8_t> half(size_t(src.w / 2) * (src.h / 2) * 4), ref(size_t(dst.w) * dst.h * 4);
    auto box = [](const std::vector<uint8_t>& in, int w, std::vector<uint8_t>& out, int ow, int oh) {
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
                for (int c = 0; c < 4; ++c) {
                    int s = 0;
                    for (int k = 0; k < 4; ++k) s += in[(size_t(2 * y + k / 2) * w + 2 * x + k % 2) * 4 + c];
                    out[(size_t(y) * ow + x) * 4 + c] = uint8_t((s + 2) / 4);
                }
    };
    box(px, src.w, half, src.w / 2, src.h / 2);
    box(half, src.w / 2, ref, dst.w, dst.h);
    const double psnr = Psnr(got.data(), ref.data(), ref.size());
    std::printf("    Downscale 1024x512 -> 256x128 vs CPU box: %.1f dB\n", psnr);
    CT_CHECK(psnr >= 45.0);
}

CT_TEST_DEVICE(export_mp4_psnr_vs_preview) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    const project::Project p = test::MakeExportProject(env.bundle, 1920, 1080, kPatternUs, 2'000'000);
    // MP4 at quality 90.
    ExportRequest r = Request(p, "mp4-1080p", env.Out("q90.mp4"));
    r.preset.video.rc = RateControl::Quality;
    r.preset.video.quality = 90;
    r.preset.video.width = r.preset.video.height = 0;
    ExportJob job;
    const ExportJob::Progress* pr = nullptr;
    CT_REQUIRE(RunJob(env, std::move(r), &pr, job));
    CT_REQUIRE(pr->finished.load());
    CT_CHECK_HR(pr->hr);
    CT_CHECK_EQ(pr->frames_done.load(), 120);
    std::printf("    mp4 2 s 1080p60 q90: %.2f s, %.2fx real time, %s, encoder %s\n", pr->elapsed_s.load(), pr->speed_realtime.load(),
                FormatBytes(pr->output_bytes).c_str(), pr->encoder.c_str());
    media::MediaInfo info;
    CT_REQUIRE(SUCCEEDED(media::MediaInfo::Load(env.Out("q90.mp4"), info)));
    CT_CHECK_EQ(info.AudioStreams().size(), 1u);
    CT_CHECK(info.FirstVideo() >= 0);
    // Decode frame 30 and compare with the preview converted to NV12 by the same converter.
    media::VideoDecoder dec;
    media::VideoDecoder::Config dc;
    dc.path = env.Out("q90.mp4");
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, dc)));
    const TimeUs t30 = FrameTimeUs(30, 60, 1);
    test::Nv12Image decoded;
    bool got = false;
    for (int i = 0; i < 120 && !got; ++i) {
        media::DecodedFrame f;
        if (dec.ReadFrame(f) != S_OK) break;
        if (f.pts_us + f.duration_us / 2 >= t30) {
            got = test::ReadbackNv12(*dev, f.nv12, decoded);
            CT_CHECK(std::llabs(f.pts_us - t30) < 2000);
        }
        dec.ReleaseFrame(f.pool_index);
    }
    CT_REQUIRE(got);
    std::vector<uint8_t> prev;
    ComPtr<ID3D11Texture2D> prev_tex;
    CT_REQUIRE(Preview(env, p, t30, prev, &prev_tex));
    capture::ColorConverter conv;
    capture::ColorConverter::Config cc;
    cc.src_size = cc.dst_size = SizeI{1920, 1080};
    CT_REQUIRE(SUCCEEDED(conv.Init(*dev, cc)));
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1920;
    d.Height = 1080;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.BindFlags = conv.DstBindFlags();
    ComPtr<ID3D11Texture2D> nv12;
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateTexture2D(&d, nullptr, nv12.GetAddressOf())));
    {
        platform::D3DDevice::Lock lock(*dev);
        CT_REQUIRE(SUCCEEDED(conv.Convert(dev->Context(), prev_tex.Get(), nv12.Get())));
    }
    test::Nv12Image ref;
    CT_REQUIRE(test::ReadbackNv12(*dev, nv12.Get(), ref));
    CT_REQUIRE(ref.width == decoded.width && ref.height == decoded.height);
    const double py = Psnr(ref.y.data(), decoded.y.data(), ref.y.size());
    const double puv = Psnr(ref.uv.data(), decoded.uv.data(), ref.uv.size());
    std::vector<uint8_t> all_a(ref.y), all_b(decoded.y);
    all_a.insert(all_a.end(), ref.uv.begin(), ref.uv.end());
    all_b.insert(all_b.end(), decoded.uv.begin(), decoded.uv.end());
    const double pall = Psnr(all_a.data(), all_b.data(), all_a.size());
    std::printf("    frame 30 vs preview: PSNR Y %.2f dB, UV %.2f dB, YUV %.2f dB\n", py, puv, pall);
    CT_CHECK(pall >= 42.0);
    {
        platform::D3DDevice::Lock lock(*dev);
        conv.Shutdown();
    }
}

CT_TEST_DEVICE(export_png_identical_to_preview) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    const project::Project p = test::MakeExportProject(env.bundle, 1920, 1080, kPatternUs, 2'000'000);
    const ExportJob::Progress* pr = nullptr;
    // PNG at t = 0.75 s equals the preview readback byte for byte.
    ExportRequest rp = Request(p, "png-frame", env.Out("still.png"), 750'000, 0);
    rp.range_end = 0;
    ExportJob job2;
    CT_REQUIRE(RunJob(env, std::move(rp), &pr, job2));
    CT_CHECK_HR(pr->hr);
    std::vector<uint8_t> png_px, prev2;
    SizeI ps;
    CT_REQUIRE(SUCCEEDED(media::ImageIo::LoadRgba(env.Out("still.png"), png_px, ps)));
    CT_REQUIRE(Preview(env, p, 750'000, prev2));
    CT_CHECK_EQ(ps.w, 1920);
    CT_REQUIRE(png_px.size() == prev2.size());
    size_t diff = 0;
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < png_px.size(); ++i)
        if (png_px[i] != prev2[i]) {
            ++diff;
            if (first == SIZE_MAX) first = i;
        }
    if (diff) {
        const size_t px0 = first / 4 * 4;
        std::printf("    first diff at pixel %zu (%zu,%zu): png %d %d %d %d preview %d %d %d %d\n", px0 / 4, (px0 / 4) % 1920,
                    (px0 / 4) / 1920, png_px[px0], png_px[px0 + 1], png_px[px0 + 2], png_px[px0 + 3], prev2[px0], prev2[px0 + 1],
                    prev2[px0 + 2], prev2[px0 + 3]);
        if (const char* dump = std::getenv("CT_EXPORT_DUMP")) {
            test::WritePng((std::string(dump) + "/png_export.png").c_str(), png_px.data(), 1920, 1080);
            test::WritePng((std::string(dump) + "/png_preview.png").c_str(), prev2.data(), 1920, 1080);
        }
    }
    CT_CHECK_EQ(diff, 0u);
}

CT_TEST_DEVICE(export_m4a_duration_and_cancel_leaves_no_file) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    const project::Project p = test::MakeExportProject(env.bundle, 1920, 1080, kPatternUs, 2'000'000);
    ExportJob job;
    const ExportJob::Progress* pr = nullptr;
    CT_REQUIRE(RunJob(env, Request(p, "m4a-audio", env.Out("a.m4a")), &pr, job));
    CT_CHECK_HR(pr->hr);
    media::AudioDecoder ad;
    media::AudioDecoder::Config ac;
    ac.path = env.Out("a.m4a");
    CT_REQUIRE(SUCCEEDED(ad.Init(ac)));
    std::vector<float> buf(4800 * 2);
    uint64_t frames = 0;
    double sum2 = 0;
    for (;;) {
        uint32_t n = 0;
        TimeUs pts = 0;
        if (ad.Read(buf.data(), 4800, n, pts) != S_OK || n == 0) break;
        for (uint32_t i = 0; i < n * 2; ++i) sum2 += double(buf[i]) * buf[i];
        frames += n;
    }
    const double dur = double(frames) / 48000.0;
    std::printf("    m4a: %.4f s decoded, RMS %.3f\n", dur, std::sqrt(sum2 / double(std::max<uint64_t>(frames * 2, 1))));
    CT_CHECK(std::fabs(dur - 2.0) <= 0.02);
    CT_CHECK(sum2 > 0);

    // Cancel half way: no file left, cancelled flag set.
    const project::Project longer = test::MakeExportProject(env.bundle, 1920, 1080, kPatternUs, 6'000'000);
    ExportJob job2;
    CT_REQUIRE(RunJob(env, Request(longer, "mp4-1080p", env.Out("cancel.mp4")), &pr, job2, 0.5));
    CT_CHECK(pr->finished.load());
    CT_CHECK(pr->cancelled.load());
    CT_CHECK(pr->frames_done.load() < pr->frames_total.load());
    CT_CHECK(!fs::Exists(env.Out("cancel.mp4")));
}

CT_TEST_DEVICE(export_gif_720p_decodes) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    const project::Project p = test::MakeExportProject(env.bundle, 1920, 1080, kPatternUs, 2'000'000);
    ExportJob job;
    const ExportJob::Progress* pr = nullptr;
    CT_REQUIRE(RunJob(env, Request(p, "gif-720p", env.Out("a.gif")), &pr, job));
    CT_CHECK_HR(pr->hr);
    CT_CHECK_EQ(pr->frames_total.load(), 40);
    std::printf("    gif 2 s 720p20: %.2f s, %.2fx real time, %s\n", pr->elapsed_s.load(), pr->speed_realtime.load(),
                FormatBytes(pr->output_bytes).c_str());
    std::string bytes;
    CT_REQUIRE(fs::ReadAll(env.Out("a.gif"), bytes));
    test::gif::File f;
    std::string err;
    CT_REQUIRE(test::gif::Parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), f, &err));
    CT_CHECK_EQ(f.width, 1280);
    CT_CHECK_EQ(f.height, 720);
    CT_CHECK(f.trailer);
    CT_CHECK(f.frames.size() >= 2u);
    int total_cs = 0;
    for (const auto& fr : f.frames) total_cs += fr.delay_cs;
    CT_CHECK_EQ(total_cs, 200);   // 40 frames x 5 cs (merged duplicates keep the total)
}
