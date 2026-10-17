# P02 · Screen capture (D3D service, Desktop Duplication, GPU color conversion, cursor tracking, pacing)

**Goal:** capture any monitor or region at a constant frame rate into NV12 GPU textures with zero CPU pixel
copies, survive every access-lost scenario, and track the cursor (position + shapes) as data.

**Read first:** `docs/03-architecture.md` sections 3, 4, 5, 8, 10; `docs/02` sections 6, 10; `docs/07` (Direct3D/DXGI rows).
**Depends on:** P01. **Size:** L (about 3,500 lines incl. shaders, tests, bench tool).

## Deliverables

```
src/platform/include/ct/platform/{d3d_device.h (extended),texture_pool.h,gpu_timer.h}
src/capture/include/ct/capture/{display.h,desktop_duplicator.h,color_convert.h,cursor_tracker.h,screen_capturer.h,frame_sink.h}
src/capture/src/*.cpp   src/capture/shaders/{fullscreen.hlsl,bgra_to_nv12.hlsl,rgba16f_to_nv12.hlsl}
tests/capture/*.cpp     tools/capture_bench/main.cpp (target capture_bench)
```

## 2.1 `D3DDevice` extensions and `TexturePool`

Add to `D3DDevice` (keep P00's API):
```cpp
IDXGIAdapter4* Adapter() const;  LUID AdapterLuid() const;
HRESULT InitMfManager();                      // MFCreateDXGIDeviceManager(&m_mf_token, &m_mf_mgr); m_mf_mgr->ResetDevice(device, token). Called lazily by ct_media; safe to call twice.
IMFDXGIDeviceManager* MfManager() const;  UINT MfToken() const;
bool SupportsNv12RenderTarget() const;        // CheckFormatSupport(DXGI_FORMAT_NV12) has D3D11_FORMAT_SUPPORT_RENDER_TARGET | TEXTURE2D | SHADER_SAMPLE
bool SupportsFp16RenderTarget() const;
ID3D11VideoDevice* VideoDevice() const;  ID3D11VideoContext* VideoContext() const;    // QueryInterface, may be null on WARP
```
Also create `D3DDevice` on the adapter that owns the **primary monitor** (enumerate adapters, pick the one whose `EnumOutputs` contains the output whose `DXGI_OUTPUT_DESC.AttachedToDesktop` and rect contains (0,0)), falling back to high-performance preference, then WARP.

```cpp
// texture_pool.h — fixed set of identical textures; Acquire on one thread, Release on another (SPSC of indices).
namespace ct::platform {
class TexturePool {
public:
    HRESULT Init(D3DDevice&, const D3D11_TEXTURE2D_DESC& desc, uint32_t count, const char* debug_name);   // sets WKPDID_D3DDebugObjectName
    ID3D11Texture2D* Acquire(uint32_t& index);            // nullptr if none free; honours the reuse predicate
    void Release(uint32_t index);
    void SetReusePredicate(bool (*can_reuse)(void* user, uint32_t index), void* user);   // P04 installs the MF buffer refcount check
    ID3D11Texture2D* At(uint32_t index) const;  uint32_t Count() const;  uint32_t InUse() const;  const D3D11_TEXTURE2D_DESC& Desc() const;
    void* UserData(uint32_t index);  void SetUserData(uint32_t index, void*);   // P04 stores the IMFMediaBuffer per texture
};
class GpuTimer { public: HRESULT Init(D3DDevice&); void Begin(ID3D11DeviceContext*); void End(ID3D11DeviceContext*); bool Resolve(ID3D11DeviceContext*, double& ms); /* disjoint + 2 timestamps, double-buffered, non-blocking */ };
}
```

## 2.2 Display model (`display.h`)
```cpp
namespace ct::capture {
enum class Rotation { Identity, R90, R180, R270 };
struct DisplayInfo {
    int index = 0;  std::wstring device_name;  std::string friendly_name;   // "\\.\DISPLAY1", "DELL U2723QE"
    RectI rect, work_rect;  UINT dpi = 96;  bool primary = false;  UINT refresh_hz = 60;
    bool hdr = false;  float sdr_white_nits = 80.f;  Rotation rotation = Rotation::Identity;
    LUID adapter_luid{};  UINT output_index = 0;  HMONITOR hmonitor = nullptr;
};
std::vector<DisplayInfo> EnumerateDisplays(IDXGIFactory6* factory);
RectI VirtualScreenRect();                       // SM_XVIRTUALSCREEN .. SM_CYVIRTUALSCREEN
int DisplayIndexAt(const std::vector<DisplayInfo>&, PointI p);
RectI ClampRegionToDisplay(const DisplayInfo&, RectI region);   // also forces even width/height (round down), min 64x64
}
```
Implementation: `EnumDisplayMonitors` -> `GetMonitorInfoW(MONITORINFOEXW)` -> `GetDpiForMonitor(MDT_EFFECTIVE_DPI)`; match to DXGI by iterating adapters/outputs and comparing `DXGI_OUTPUT_DESC1.DeviceName`; `Rotation` from `DXGI_OUTPUT_DESC1.Rotation`; HDR when `ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`; friendly name and SDR white via `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` + `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME` and `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL` (`SDRWhiteLevel / 1000.0 * 80` nits), matched by `GDI device name` from `DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME`; refresh from `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS).dmDisplayFrequency`.

## 2.3 `DesktopDuplicator`
```cpp
namespace ct::capture {
class DesktopDuplicator {
public:
    struct Config { DisplayInfo display; RectI region; /* virtual-screen px, already clamped & even */ bool prefer_fp16_when_hdr = true; };
    HRESULT Init(platform::D3DDevice&, const Config&);
    // Acquire a new desktop image into dst (same size as region, format Format()). Caller holds D3DDevice::Lock during the call.
    //   S_OK   = dst updated, present_qpc set
    //   S_FALSE= no new image within timeout (or pointer-only update); dst untouched
    //   DXGI_ERROR_ACCESS_LOST = caller must call Reinit() (keep using the previous frame meanwhile)
    //   other failures = fatal for this session
    HRESULT AcquireInto(ID3D11Texture2D* dst, uint32_t timeout_ms, int64_t& present_qpc);
    HRESULT Reinit();                      // releases and re-creates the duplication; E_ACCESSDENIED while a secure desktop is up -> keep retrying every 250 ms
    DXGI_FORMAT Format() const;            // B8G8R8A8_UNORM or R16G16B16A16_FLOAT
    SizeI Size() const;  Rotation DisplayRotation() const;  bool IsCrossAdapter() const;
    void Shutdown();
};
}
```
Init sequence:
1. Find the `IDXGIOutput6` for `display.adapter_luid/output_index`. If that adapter is not the main device's adapter: create a **second** `ID3D11Device` on that adapter, duplicate there, and copy frames through a staging texture + `UpdateSubresource` on the main device (slow path, log a warning, `IsCrossAdapter() == true`). Document in `DECISIONS.md` if this path is exercised by a tester; the fast cross-adapter path is PX.
2. `output5->DuplicateOutput1(device, 0, n, formats, &dup)` with `formats = { R16G16B16A16_FLOAT (only if display.hdr && prefer_fp16), B8G8R8A8_UNORM }`. On `E_NOTIMPL`/missing interface fall back to `output1->DuplicateOutput(device, &dup)`.
3. `dup->GetDesc(&desc)`: `desc.ModeDesc.Format` is `Format()`; `desc.DesktopImageInSystemMemory` must be FALSE (if TRUE, still works via `MapDesktopSurface`, but we do not support it: return `E_NOTIMPL` and log).
4. Compute the `D3D11_BOX` for `region` in display-local coordinates, accounting for rotation: the duplicated texture is in **native panel orientation**; for `R90/R270` swap width/height and rotate the box; the converter rotates back (section 2.4).

Acquire sequence:
```
hr = dup->AcquireNextFrame(timeout_ms, &info, &resource);
if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
if (hr == DXGI_ERROR_ACCESS_LOST) return hr;           // do not ReleaseFrame
CT_RETURN_IF_FAILED(hr);
if (info.LastPresentTime.QuadPart == 0) { dup->ReleaseFrame(); return S_FALSE; }   // only the pointer moved
resource.As(&tex); ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, tex.Get(), 0, &box);
present_qpc = info.LastPresentTime.QuadPart; dup->ReleaseFrame(); return S_OK;
```
Never hold the acquired frame across the tick; never call `AcquireNextFrame` twice without `ReleaseFrame`. `info.AccumulatedFrames > 1` increments `Stats::C("capture.coalesced")`.

## 2.4 `ColorConverter` (BGRA/FP16 -> NV12 on the GPU)
```cpp
namespace ct::capture {
class ColorConverter {
public:
    struct Config { SizeI src_size; DXGI_FORMAT src_format; Rotation rotation; bool hdr_tonemap; float sdr_white_nits; SizeI dst_size; /* == src_size after rotation, even */ };
    HRESULT Init(platform::D3DDevice&, const Config&);
    HRESULT Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst_nv12);   // caller holds the device lock
    bool UsingVideoProcessorFallback() const;
};
}
```
Primary path (requires `SupportsNv12RenderTarget()`): two RTVs on `dst_nv12`: luma `D3D11_RENDER_TARGET_VIEW_DESC{ Format = DXGI_FORMAT_R8_UNORM }` and chroma `{ Format = DXGI_FORMAT_R8G8_UNORM }` (the driver maps them to plane 0 / plane 1). Fullscreen triangle vertex shader (`SV_VertexID` 0..2, no vertex buffer). Luma pass: viewport W x H; chroma pass: viewport W/2 x H/2 sampling the source with a linear sampler at the 2x2 block centre (this averages the four pixels).

`bgra_to_nv12.hlsl` (BT.709 limited range; keep these constants):
```hlsl
Texture2D<float4> src : register(t0);  SamplerState lin : register(s0);
cbuffer CB : register(b0) { float2 inv_src_size; float2 pad; float4x4 rot_uv; /* rotation as uv transform */ float hdr_scale; float tonemap; float2 pad2; }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 Fetch(float2 uv) {
    float4 p = src.SampleLevel(lin, mul(float4(uv, 0, 1), rot_uv).xy, 0);
    float3 c = p.rgb;
    if (tonemap > 0.5) {                 // scRGB FP16 input: 1.0 = 80 nits linear
        c = c * hdr_scale;               // hdr_scale = 80 / sdr_white_nits -> SDR white becomes 1.0
        c = c / (1.0 + max(c - 1.0, 0.0));   // soft roll-off above SDR white
        c = LinearToSrgb(saturate(c));
    }
    return saturate(c);
}
float  ps_luma  (float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { float3 c = Fetch(uv);
    return  0.1826 * c.r + 0.6142 * c.g + 0.0620 * c.b + 16.0 / 255.0; }
float2 ps_chroma(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { float3 c = Fetch(uv);   // uv already at 2x2 centre
    return float2(-0.1006 * c.r - 0.3386 * c.g + 0.4392 * c.b + 128.0 / 255.0,
                   0.4392 * c.r - 0.3989 * c.g - 0.0403 * c.b + 128.0 / 255.0); }
```
Rotation: `rot_uv` maps output uv to source uv (`R90`: `(u,v) -> (v, 1-u)`, `R180`: `(1-u, 1-v)`, `R270`: `(1-v, u)`).

Fallback path (WARP or no NV12 RT): `ID3D11VideoDevice::CreateVideoProcessorEnumerator(desc{ InputFrameFormat PROGRESSIVE, Input/OutputFrameRate 60/1, InputWidth/Height, OutputWidth/Height, Usage NORMAL })`, `CreateVideoProcessor(enum, 0)`, input view on `src` (`D3D11_VPIV_DIMENSION_TEXTURE2D`), output view on `dst_nv12` (`D3D11_VPOV_DIMENSION_TEXTURE2D`), `VideoProcessorSetStreamColorSpace` (RGB full, sRGB) and `VideoProcessorSetOutputColorSpace` (YCbCr, BT.709, limited), `VideoProcessorSetStreamRotation` for rotation, `VideoProcessorBlt(vp, out_view, 0, 1, &stream{ Enable TRUE, pInputSurface })`. No HDR tone-map in the fallback (log once).

## 2.5 `CursorTracker`
```cpp
namespace ct::capture {
struct CursorSample { TimeUs t_us; int32_t x, y; uint32_t shape_id; bool visible; };
struct CursorShape  { uint32_t id; uint16_t w, h; int16_t hot_x, hot_y; std::vector<uint32_t> bgra_premul; };
class CursorTracker {
public:
    void Reset();
    // Called at 120 Hz on the hooks thread (P05) or the capture thread (bench). region = capture region in virtual-screen px.
    void Poll(TimeUs t_us, const RectI& region, SpscQueue<CursorSample, 4096>& samples, SpscQueue<CursorShape, 64>& shapes);
    static bool CaptureShape(HCURSOR hcursor, CursorShape& out);   // public for tests
};
}
```
Poll: `CURSORINFO ci{ sizeof ci }; GetCursorInfo(&ci)`; `visible = (ci.flags & CURSOR_SHOWING) != 0`; position = `ci.ptScreenPos - region.origin` (may be outside the region; keep it, the editor clips). If `ci.hCursor != m_last_hcursor`: `CaptureShape`, hash pixels + hotspot with `Fnv1a32` -> `shape_id`; if the id is new to this session push a `CursorShape`. Push a `CursorSample` only if anything changed since the last pushed sample.

`CaptureShape`: `GetIconInfoExW` -> `hbmColor` (may be null for monochrome), `hbmMask`, hotspot; size from `GetObjectW(hbmMask, BITMAP)` (for monochrome cursors height is doubled: AND mask over XOR mask). Create two 32-bpp top-down DIB sections (`BITMAPINFOHEADER biHeight = -h`), fill one black and one white, `DrawIconEx(hdc, 0, 0, hcursor, w, h, 0, nullptr, DI_NORMAL)` into each. Per pixel: `alpha = 255 - (white.r - black.r)` (clamped), `color_premul = black.rgb`; where `black > white` (inverting pixels of XOR cursors like the I-beam) set `alpha = 255, color = white` (the editor draws a shadow so it stays visible). Delete all GDI objects (`hbmColor`, `hbmMask`, DIBs, DCs). Animated cursors: frame 0 only (documented limitation).

## 2.6 `ScreenCapturer` (pacing + orchestration)
```cpp
namespace ct::capture {
struct FrameSink {   // implemented by ct_media::Mp4Writer in P04, by capture_bench here
    virtual bool OnFrame(ID3D11Texture2D* nv12, uint32_t pool_index, TimeUs t_us) = 0;   // false = sink rejected (full); capturer releases the texture and counts a drop
    virtual ~FrameSink() = default;
};
struct CaptureStats { std::atomic<uint64_t> ticks{0}, new_frames{0}, repeats{0}, dropped_pool_empty{0}, dropped_sink{0}, late_ticks{0}, access_lost{0}, reinits{0}; Stats::Timing acquire, convert; };
class ScreenCapturer {
public:
    struct Config { DisplayInfo display; RectI region; int fps_num = 60, fps_den = 1; bool hdr_tonemap = true; uint32_t nv12_pool_size = 12; };
    HRESULT Init(platform::D3DDevice&, const Config&);          // creates duplicator, converter, BGRA pool(3), NV12 pool (bind RT|SR, DXGI_FORMAT_NV12), timer
    platform::TexturePool& Nv12Pool();                           // P04 attaches reuse predicate/user data before Start
    bool Start(int64_t epoch_qpc, FrameSink& sink);              // spawns "ct-capture" (TIME_CRITICAL, MMCSS "Capture")
    void Pause();  void Resume();                                 // pause stops ticking; resume shifts the tick base so media time has no gap
    void Stop();                                                  // joins the thread
    const CaptureStats& Stats() const;  SizeI OutputSize() const;
};
}
```
Thread loop (the pacer):
```
n = 0; base_qpc = epoch_qpc; have_frame = false;
while (!stop) {
    tick_qpc = base_qpc + UsToQpc(FrameTimeUs(n, fps))      // tick-aligned
    timer.WaitUntilQpc(tick_qpc - 1 ms, stop_event)
    { Lock lk(dev);
      hr = dup.AcquireInto(bgra_cur, timeout = clamp((tick_qpc + 2 ms - now) in ms, 0, 4));
      if (hr == S_OK) { have_frame = true; stats.new_frames++; }
      else if (hr == DXGI_ERROR_ACCESS_LOST) { stats.access_lost++; schedule Reinit() every 250 ms (non-blocking retry counter); }
      else if (FAILED(hr)) { fatal: log, sink.OnError(hr) (add to FrameSink), break; }
      else stats.repeats++;
      if (have_frame) {
          nv12 = pool.Acquire(idx); if (!nv12) { stats.dropped_pool_empty++; } else {
              stats.convert.measure( converter.Convert(ctx, bgra_cur, nv12) );
              if (!sink.OnFrame(nv12, idx, FrameTimeUs(n, fps) + pause_offset_adjust)) { pool.Release(idx); stats.dropped_sink++; }
          }
      }
    }
    n++; stats.ticks++;
    now = QpcNow(); if (now > tick_qpc + 1.5 * frame_dur) { late = floor((now - base_qpc)/frame_dur) - n; n += late; stats.late_ticks += late; }   // catch up by skipping ticks (file keeps CFR; the skipped ticks are reported as drops)
}
```
Before the first `S_OK` the loop does not emit frames (`have_frame == false`) and, once the first frame arrives, the video stream's first timestamp is that tick's time (not 0); the writer accepts a non-zero first timestamp. Pause/resume: on resume, `base_qpc += (resume_qpc - pause_qpc)` so `FrameTimeUs(n)` continues without a gap.

## Tool: `capture_bench`
`capture_bench --monitor 0 [--region x,y,w,h] --fps 60 --seconds 30 [--no-convert] [--dump-frame 100 out.png]`. Implements a `FrameSink` that immediately releases textures (optionally counts luma checksum of every 60th frame via a staging readback to prove content changes), prints `Stats::Dump` every second and a final summary: ticks, new frames, repeats, drops, acquire/convert avg/max ms, process CPU %, RSS. `--dump-frame` converts NV12 back to RGB on the CPU and writes a PNG through WIC (`platform::ImageIo`, minimal in this phase; full version in P06).

## Tests
- Unit: `ClampRegionToDisplay` evenness/min; rotation uv matrices (4 cases, compare to CPU reference); BT.709 conversion CPU reference `RgbToNv12Ref()` used by the GPU test.
- Device (`CT_TEST_DEVICE`): device creation with/without debug; NV12 RT support report; `EnumerateDisplays` returns >= 1 display with sane DPI; duplicator `Init` + first `S_OK` within 2 s while the test paints a window (`CreateWindow` + `FillRect` red); GPU convert of a synthetic BGRA texture (gradient uploaded via `UpdateSubresource`) read back via staging: Y/U/V within +-2 of the reference for 10,000 sampled pixels; cursor shape capture of `LoadCursor(IDC_ARROW)` yields w,h in [16, 256], hotspot inside, alpha mix of 0 and 255; `ScreenCapturer` 3 s at 60 fps: `ticks in [178, 182]`, `dropped_* == 0`.

## Acceptance (2.7)
`capture_bench --monitor 0 --fps 60 --seconds 30` on the reference machine with a video playing fullscreen on that monitor: `dropped_pool_empty == 0`, `dropped_sink == 0`, `late_ticks == 0`, `acquire avg < 0.5 ms`, `convert avg < 0.3 ms` at 1080p, process CPU < 3 %, GPU < 8 % (Task Manager), RSS < 120 MB. Same at 4K: CPU < 5 %, GPU < 15 %. Lock the workstation and unlock during the run: `access_lost >= 1`, `reinits >= 1`, capture resumes within 1 s, no crash. Numbers recorded in `PROGRESS.md`.

## Pitfalls
- Desktop duplication is per-process-per-output: only one `IDXGIOutputDuplication` per output per process; recreate instead of creating a second.
- `AcquireNextFrame(0)` busy-loops the CPU; always wait on the timer first and use a small positive timeout.
- A `D3D11_BOX` outside the source texture makes `CopySubresourceRegion` fail silently (debug layer error): clamp the region on every `Reinit()` because the display size can change.
- NV12 textures must have even dimensions and `D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE`; `D3D11_RESOURCE_MISC_SHARED` is **not** needed for the MF encoder on the same device.
- RTV on an NV12 plane needs `Texture2D.MipSlice = 0`; `OMSetRenderTargets` one plane at a time with its own viewport.
- `GetIconInfoExW` returns bitmaps you must delete; forgetting leaks GDI handles at 120 Hz and kills the process at 10,000 handles.
- Secure desktop: `DuplicateOutput` fails with `E_ACCESSDENIED` until the UAC prompt closes; do not log every retry.
