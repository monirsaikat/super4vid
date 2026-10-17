#include <ct/test/ct_test.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include <windows.h>
#include <crtdbg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ct::test {

namespace {

TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;

struct RunState {
    const TestCase* current = nullptr;
    int failures_in_test = 0;
    bool skipped = false;
    char skip_reason[256] = {};
    bool update_golden = false;
    bool verbose = false;
};
RunState g_run;

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double QpcToMs(int64_t ticks) {
    static int64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Unit:   return "unit";
    case Kind::Device: return "device";
    case Kind::Golden: return "golden";
    case Kind::Bench:  return "bench";
    }
    return "?";
}

// Runs one test; returns true when it failed.
bool RunOne(const TestCase& tc) {
    g_run.current = &tc;
    g_run.failures_in_test = 0;
    g_run.skipped = false;
    g_run.skip_reason[0] = '\0';

    if ((tc.kind == Kind::Device || tc.kind == Kind::Golden) && IsCi()) {
        std::printf("[SKIP] %s: CT_CI is set (needs hardware)\n", tc.name);
        return false;
    }

    const int64_t start = QpcNow();
    if (tc.kind == Kind::Bench) {
        tc.bench_fn(tc.bench_n);
    } else {
        tc.fn();
    }
    const double ms = QpcToMs(QpcNow() - start);

    if (g_run.failures_in_test > 0) {
        std::printf("[FAIL] %s (%.1f ms)\n", tc.name, ms);
        return true;
    }
    if (g_run.skipped) {
        std::printf("[SKIP] %s: %s\n", tc.name, g_run.skip_reason);
        return false;
    }
    if (tc.kind == Kind::Bench) {
        const double ns_per_op = tc.bench_n > 0 ? ms * 1e6 / static_cast<double>(tc.bench_n) : 0.0;
        const double ops_per_s = ms > 0 ? static_cast<double>(tc.bench_n) * 1000.0 / ms : 0.0;
        std::printf("[BENCH] %s: %lld ops in %.1f ms, %.3f M ops/s, %.2f ns/op\n", tc.name,
                    static_cast<long long>(tc.bench_n), ms, ops_per_s / 1e6, ns_per_op);
        return false;
    }
    std::printf("[PASS] %s (%.1f ms)\n", tc.name, ms);
    return false;
}

void PrintUsage(const char* exe) {
    std::printf("usage: %s [--filter <substr>] [--list] [--bench] [--repeat N] [--update-golden] [--verbose]\n",
                exe ? exe : "test");
}

} // namespace

Registrar::Registrar(TestCase* test_case) {
    test_case->next = nullptr;
    if (g_tail) g_tail->next = test_case; else g_head = test_case;
    g_tail = test_case;
}

void ReportFailure(const char* file, int line, const char* expr, const std::string& detail) {
    ++g_run.failures_in_test;
    if (detail.empty())
        std::printf("[FAIL] %s(%d): %s\n", file, line, expr);
    else
        std::printf("[FAIL] %s(%d): %s  %s\n", file, line, expr, detail.c_str());
    std::fflush(stdout);
}

void ReportSkip(const char* reason) {
    g_run.skipped = true;
    std::snprintf(g_run.skip_reason, sizeof(g_run.skip_reason), "%s", reason ? reason : "");
}

bool UpdateGolden() { return g_run.update_golden; }
bool Verbose() { return g_run.verbose; }
const char* CurrentTestName() { return g_run.current ? g_run.current->name : ""; }

namespace {

std::wstring Widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// COM for WIC on the test thread (either apartment is fine; keep whatever the test set up).
void EnsureCom() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;   // S_OK / S_FALSE / RPC_E_CHANGED_MODE: COM is usable in all three cases
}

Microsoft::WRL::ComPtr<IWICImagingFactory> WicFactory() {
    EnsureCom();
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.GetAddressOf()))))
        return nullptr;
    return f;
}

} // namespace

bool WritePng(const char* path_utf8, const void* rgba, int width, int height) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> f = WicFactory();
    if (!f || !rgba || width <= 0 || height <= 0) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateStream(stream.GetAddressOf()))) return false;
    if (FAILED(stream->InitializeFromFilename(Widen(path_utf8).c_str(), GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.GetAddressOf()))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(frame.GetAddressOf(), nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    const UINT stride = static_cast<UINT>(width) * 4;
    BYTE* data = static_cast<BYTE*>(const_cast<void*>(rgba));
    if (fmt == GUID_WICPixelFormat32bppRGBA) {
        if (FAILED(frame->WritePixels(static_cast<UINT>(height), stride, stride * static_cast<UINT>(height), data))) return false;
    } else {   // the encoder wants another layout: convert through a WIC bitmap
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(f->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppRGBA,
                                             stride, stride * static_cast<UINT>(height), data, bitmap.GetAddressOf())))
            return false;
        if (FAILED(f->CreateFormatConverter(conv.GetAddressOf()))) return false;
        if (FAILED(conv->Initialize(bitmap.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return false;
        if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

bool ReadPng(const char* path_utf8, std::vector<uint8_t>& rgba, int& width, int& height) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> f = WicFactory();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateDecoderFromFilename(Widen(path_utf8).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            dec.GetAddressOf())))
        return false;
    if (FAILED(dec->GetFrame(0, frame.GetAddressOf()))) return false;
    if (FAILED(f->CreateFormatConverter(conv.GetAddressOf()))) return false;
    // Straight (non-premultiplied) RGBA, exactly what WritePng stored.
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    rgba.resize(static_cast<size_t>(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()))) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

void CompareImages(const uint8_t* a, const uint8_t* b, int width, int height, int& max_diff, double& psnr_db) {
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    uint64_t sq = 0;
    int mx = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        mx = std::max(mx, d);
        sq += static_cast<uint64_t>(d * d);
    }
    max_diff = mx;
    if (sq == 0 || n == 0) {
        psnr_db = 1e9;
        return;
    }
    const double mse = static_cast<double>(sq) / static_cast<double>(n);
    psnr_db = 10.0 * std::log10(255.0 * 255.0 / mse);
}

bool CheckGolden(const char* file, int line, const void* rgba, int width, int height) {
    return CheckGoldenTol(file, line, rgba, width, height, kGoldenMaxDiff, kGoldenMinPsnr);
}

bool CheckGoldenTol(const char* file, int line, const void* rgba, int width, int height, int max_diff, double min_psnr_db) {
    if (!rgba || width <= 0 || height <= 0) {
        ReportFailure(file, line, "CT_GOLDEN_IMAGE", "empty image");
        return false;
    }
    const std::string base = std::string("data/golden/") + CurrentTestName();
    const std::string golden = base + ".png";
    if (g_run.update_golden) {
        CreateDirectoryA("data", nullptr);
        CreateDirectoryA("data/golden", nullptr);
        if (!WritePng(golden.c_str(), rgba, width, height)) {
            ReportFailure(file, line, "CT_GOLDEN_IMAGE", "could not write " + golden);
            return false;
        }
        std::printf("  golden written: %s (%dx%d)\n", golden.c_str(), width, height);
        return true;
    }
    std::vector<uint8_t> ref;
    int rw = 0, rh = 0;
    if (!ReadPng(golden.c_str(), ref, rw, rh)) {
        ReportFailure(file, line, "CT_GOLDEN_IMAGE",
                      "missing or unreadable " + golden + " (run from tests/ with --update-golden to create it)");
        WritePng((base + ".actual.png").c_str(), rgba, width, height);
        return false;
    }
    if (rw != width || rh != height) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "size %dx%d, golden %dx%d", width, height, rw, rh);
        ReportFailure(file, line, "CT_GOLDEN_IMAGE", buf);
        WritePng((base + ".actual.png").c_str(), rgba, width, height);
        return false;
    }
    const uint8_t* px = static_cast<const uint8_t*>(rgba);
    int diff = 0;
    double psnr = 0;
    CompareImages(px, ref.data(), width, height, diff, psnr);
    if (g_run.verbose) std::printf("  golden %s: max diff %d, PSNR %.2f dB\n", golden.c_str(), diff, psnr);
    if (diff <= max_diff && psnr >= min_psnr_db) {
        DeleteFileA((base + ".actual.png").c_str());
        DeleteFileA((base + ".diff.png").c_str());
        return true;
    }
    std::vector<uint8_t> d(ref.size());
    for (size_t i = 0; i < ref.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            const int v = std::abs(static_cast<int>(px[i + c]) - static_cast<int>(ref[i + c])) * 8;
            d[i + c] = static_cast<uint8_t>(std::min(v, 255));
        }
        d[i + 3] = 255;
    }
    WritePng((base + ".actual.png").c_str(), rgba, width, height);
    WritePng((base + ".diff.png").c_str(), d.data(), width, height);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "max diff %d (limit %d), PSNR %.2f dB (limit %.1f); wrote %s.actual.png / .diff.png", diff,
                  max_diff, psnr, min_psnr_db, base.c_str());
    ReportFailure(file, line, "CT_GOLDEN_IMAGE", buf);
    return false;
}

std::string FormatPair(const std::string& left, const std::string& right) {
    return "left=" + left + " right=" + right;
}

std::string HrDetail(long hr) {
    return "hr=" + ::ct::HrToString(hr);
}

bool HrFailed(long hr) { return hr < 0; }

bool IsCi() {
    char buf[8];
    return GetEnvironmentVariableA("CT_CI", buf, sizeof(buf)) > 0;
}

int RunAll(int argc, char** argv) {
    // Never block an unattended run on a CRT assert dialog or the Windows error-reporting UI.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* filter = nullptr;
    bool list = false;
    bool bench = false;
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(a, "--list") == 0) list = true;
        else if (std::strcmp(a, "--bench") == 0) bench = true;
        else if (std::strcmp(a, "--repeat") == 0 && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(a, "--update-golden") == 0) g_run.update_golden = true;
        else if (std::strcmp(a, "--verbose") == 0) g_run.verbose = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) { PrintUsage(argv[0]); return 0; }
        else { std::printf("unknown argument: %s\n", a); PrintUsage(argv[0]); return 1; }
    }
    ::ct::Log::SetLevel(g_run.verbose ? LogLevel::Trace : LogLevel::Warn);

    if (list) {
        for (const TestCase* tc = g_head; tc; tc = tc->next)
            if (!filter || std::strstr(tc->name, filter)) std::printf("%-7s %s\n", KindName(tc->kind), tc->name);
        return 0;
    }

    int failures = 0, passed = 0, ran = 0;
    const int64_t start = QpcNow();
    for (int r = 0; r < repeat; ++r) {
        for (const TestCase* tc = g_head; tc; tc = tc->next) {
            if (filter && !std::strstr(tc->name, filter)) continue;
            if ((tc->kind == Kind::Bench) != bench) continue;   // --bench runs benches only
            ++ran;
            if (RunOne(*tc)) ++failures; else ++passed;
        }
    }
    std::printf("%d test(s), %d passed or skipped, %d failed, %.1f ms\n", ran, passed, failures,
                QpcToMs(QpcNow() - start));
    return failures;
}

} // namespace ct::test
