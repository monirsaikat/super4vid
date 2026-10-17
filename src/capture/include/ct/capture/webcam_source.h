#pragma once
// ct/capture/webcam_source.h
// Webcam capture through the Media Foundation source reader (P05 task 5.7): device enumeration with native modes, a
// source reader with the D3D manager and advanced video processing that outputs NV12, a "ct-webcam" thread running a
// blocking ReadSample loop that copies each frame into an own NV12 TexturePool (6 textures) and hands it to a FrameSink
// with its media-time timestamp (variable frame rate: cam.mp4 is VFR, aligned to the screen by media time).
// Thread model: Enumerate/Init/Start/Stop/SetPaused from the owner thread (session / IO pool; Init can take 300-800 ms,
// never call it on the UI thread). The callers' threads must have COM initialized. Stats()/IsRunning() from any thread.
// FrameSink::OnFrame/OnError run on ct-webcam with D3DDevice::Lock held (same contract as ScreenCapturer: every
// OnFrame that returns true is one hand-out the sink returns with Nv12Pool().Release(index)).
// LatestPreviewPlanes from the UI thread; sample the views under D3DDevice::Lock.
// Lifetime: Init -> [Nv12Pool() setup] -> Start -> (SetPaused)* -> Stop -> [Start again] -> Shutdown/destructor.

#include <ct/capture/frame_sink.h>
#include <ct/capture/media_time_source.h>
#include <ct/core/thread.h>
#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/texture_pool.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IMFSample;

namespace ct::capture {

// MediaTimeSource / QpcMediaTimeSource: ct/capture/media_time_source.h (recorder::SessionClock implements it).

// One native media type of a camera (GetNativeMediaType). fourcc = the subtype GUID's Data1 ('NV12', 'YUY2', 'MJPG'...).
struct WebcamMode {
    SizeI size;
    uint32_t fps_num = 0, fps_den = 1;
    uint32_t fourcc = 0;
    uint32_t native_index = 0;   // index for GetNativeMediaType
    double Fps() const { return fps_den ? static_cast<double>(fps_num) / fps_den : 0.0; }
};

struct WebcamInfo {
    std::wstring symbolic_link;
    std::string name;                 // friendly name, UTF-8
    std::vector<SizeI> sizes;         // distinct native frame sizes, largest area first
    std::vector<WebcamMode> modes;    // every native type, in device order
};

// --- Pure helpers (unit tested) -----------------------------------------------------------------------------------

// NV12 0, YUY2 1, MJPG 2, anything else 3 (lower is better).
int WebcamSubtypeRank(uint32_t fourcc);
// Best mode for the request, lexicographically: |area - preferred area|, then |fps - preferred_fps|, then subtype rank;
// ties keep the earlier mode. Modes with odd or empty sizes or no frame rate are skipped. -1 if none qualifies.
int PickWebcamMode(const std::vector<WebcamMode>& modes, SizeI preferred_size, int preferred_fps);

// A frame's media time: the device timestamp (QPC, 100 ns) mapped through the clock when present and plausible
// (within kMaxDeviceSkewUs before now; later than now is clamped to now), else clock.NowMediaUs().
constexpr TimeUs kWebcamMaxDeviceSkewUs = 1'000'000;
TimeUs WebcamFrameTimeUs(const MediaTimeSource& clock, bool has_device_ts, uint64_t device_ts_hns, bool* used_device);

// The source of samples behind WebcamSource: the MF source reader, or a fake in tests / the synthetic generator.
// ReadSample blocks; Flush (from another thread) makes a pending ReadSample return (S_OK, no sample); Shutdown ends
// the device for good (any pending or later ReadSample fails) and is the last-resort unblock in Stop.
struct WebcamReader {
    virtual HRESULT ReadSample(DWORD* stream_flags, LONGLONG* timestamp_hns, IMFSample** sample) = 0;
    virtual HRESULT Flush() = 0;
    virtual void Shutdown() = 0;
    virtual ~WebcamReader() = default;
};

// Generates moving colour bars at `fps` (paced with Sleep) as NV12 samples carrying MFSampleExtension_DeviceTimestamp;
// dxgi_buffers = DXGI surface buffers on `dev` (the camera path), else contiguous memory buffers (the CPU fallback path).
// For webcam_demo --synthetic, tests and a recorder self-test without a camera.
std::unique_ptr<WebcamReader> CreateSyntheticWebcamReader(platform::D3DDevice& dev, SizeI size, int fps, bool dxgi_buffers);

struct WebcamStats {
    std::atomic<uint64_t> samples{0};             // ReadSample calls that returned a sample
    std::atomic<uint64_t> frames{0};              // delivered (OnFrame returned true)
    std::atomic<uint64_t> dropped_pool_empty{0};  // no free pool texture (or the reuse predicate refused all)
    std::atomic<uint64_t> dropped_sink{0};        // OnFrame returned false
    std::atomic<uint64_t> dropped_paused{0};
    std::atomic<uint64_t> dropped_late{0};        // timestamp not after the previous frame's
    std::atomic<uint64_t> dropped_bad_buffer{0};  // no usable buffer / copy failed
    std::atomic<uint64_t> stream_ticks{0};        // MF_SOURCE_READERF_STREAMTICK (gaps)
    std::atomic<uint64_t> device_timestamps{0};   // frames timed by MFSampleExtension_DeviceTimestamp
    std::atomic<uint64_t> fallback_timestamps{0}; // frames timed by NowMediaUs()
    std::atomic<uint64_t> cpu_uploads{0};         // frames that came in a memory buffer (UpdateSubresource path)
    std::atomic<int64_t> first_frame_us{-1}, last_frame_us{-1};
};

class WebcamSource {
public:
    // MFEnumDeviceSources(VIDCAP); with_modes also opens each device (300-800 ms each) to list its native types.
    // No camera = S_OK with an empty list.
    static HRESULT Enumerate(std::vector<WebcamInfo>& out, bool with_modes = true);

    struct Config {
        std::wstring symbolic_link;          // empty = the first camera
        SizeI preferred_size{1280, 720};
        int preferred_fps = 30;
        uint32_t pool_size = 6;
    };

    WebcamSource() = default;
    ~WebcamSource();
    WebcamSource(const WebcamSource&) = delete;
    WebcamSource& operator=(const WebcamSource&) = delete;

    // MFCreateDeviceSource + source reader (D3D manager, advanced video processing, hardware transforms), picks the
    // native type with PickWebcamMode, sets it, then asks for NV12 at the same size; creates the pool and preview.
    // MF_E_NOT_FOUND / HRESULT_FROM_WIN32(ERROR_NOT_FOUND) when no camera matches.
    HRESULT Init(platform::D3DDevice&, const Config&);
    // Same, over an existing reader that outputs NV12 at `size` (tests, synthetic source).
    HRESULT InitWithReader(platform::D3DDevice&, std::unique_ptr<WebcamReader> reader, SizeI size, uint32_t fps_num,
                           uint32_t fps_den, uint32_t pool_size = 6);
    void Shutdown();   // Stop, release reader/device, pool, preview

    platform::TexturePool& Nv12Pool() { return m_pool; }   // the cam Mp4Writer attaches its reuse predicate before Start
    // Spawns ct-webcam (THREAD_PRIORITY_ABOVE_NORMAL). clock and sink must outlive Stop().
    bool Start(const MediaTimeSource& clock, FrameSink& sink);
    // Sets the stop flag, Flushes the reader (repeated every 100 ms) until the thread exits; after 3 s shuts the reader
    // down; CT_FATAL after 5 s.
    void Stop();
    // Frames read while paused are dropped (the session's media time does not advance while paused).
    void SetPaused(bool paused) { m_paused.store(paused, std::memory_order_release); }
    bool IsRunning() const { return m_thread.IsRunning(); }

    SizeI ActualSize() const { return m_size; }
    int ActualFps() const;   // rounded
    uint32_t ActualFpsNum() const { return m_fps_num; }
    uint32_t ActualFpsDen() const { return m_fps_den; }
    const WebcamMode& NativeMode() const { return m_native; }   // the camera type chosen by Init (empty for InitWithReader)
    const std::string& DeviceName() const { return m_name; }
    const WebcamStats& Stats() const { return m_stats; }

    // Preview (replaces the spec's LatestPreviewSrv, see DECISIONS.md): the latest delivered frame is copied into a
    // preview NV12 texture; y = R8_UNORM view of the luma plane, uv = R8G8_UNORM view of the chroma plane (half size).
    // The UI converts to RGB (platform::Nv12Blit from P06). Returns false before the first frame or without NV12 SRV
    // support. The pointers stay valid until Shutdown; no AddRef.
    bool LatestPreviewPlanes(ID3D11ShaderResourceView** y, ID3D11ShaderResourceView** uv, SizeI* size) const;
    uint64_t PreviewSerial() const { return m_preview_serial.load(std::memory_order_acquire); }   // +1 per frame

private:
    HRESULT CreateResources();
    void Run();
    void HandleSample(IMFSample* sample);
    bool CopyIntoPool(IMFSample* sample, ID3D11Texture2D* dst);

    platform::D3DDevice* m_dev = nullptr;
    std::unique_ptr<WebcamReader> m_reader;
    platform::TexturePool m_pool;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_preview;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_preview_y, m_preview_uv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_upload;   // staging NV12 for memory-buffer samples
    Thread m_thread;
    const MediaTimeSource* m_clock = nullptr;
    FrameSink* m_sink = nullptr;
    WebcamStats m_stats;
    WebcamMode m_native{};
    std::string m_name;
    SizeI m_size{};
    uint32_t m_fps_num = 30, m_fps_den = 1;
    TimeUs m_last_t_us = -1;
    std::atomic<uint64_t> m_preview_serial{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
};

} // namespace ct::capture
