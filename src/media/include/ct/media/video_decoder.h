#pragma once
// ct/media/video_decoder.h
// Decodes one video stream with a Source Reader (hardware/DXVA through the shared D3D11 device when allowed) into a
// small pool of NV12 textures that can be sampled (BIND_SHADER_RESOURCE, aperture-cropped, even size). Decoder output
// surfaces are BIND_DECODER array slices without SRV support, hence the copy (GPU copy for DXGI buffers,
// UpdateSubresource for software decoders' system-memory buffers).
// Thread model: one thread at a time; the owner serializes Init/Seek/ReadFrame/ReleaseFrame. The D3D device lock is
// taken only around the copy, never across ReadSample (MF's decoder threads need the device).
// Lifetime: Init -> ... -> Shutdown (or the destructor). Pool textures stay valid until Shutdown or a size change.

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/texture_pool.h>

#include <wrl/client.h>

#include <string>
#include <vector>

struct IMFSourceReader;
struct IMFMediaType;

namespace ct::media {

struct DecodedFrame {
    ID3D11Texture2D* nv12 = nullptr;   // pool texture, valid until ReleaseFrame(pool_index)
    uint32_t pool_index = 0;
    TimeUs pts_us = 0;
    TimeUs duration_us = 0;
    SizeI size;
};

class VideoDecoder {
public:
    struct Config {
        std::string path;
        int stream_index = -1;       // -1 = first video stream
        uint32_t pool_size = 8;
        bool allow_hardware = true;  // false: DXVA off and no D3D manager (software decode, system-memory buffers)
    };

    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    HRESULT Init(platform::D3DDevice&, const Config&);
    void Shutdown();

    // SetCurrentPosition; the next ReadFrame returns the preceding keyframe (or later) and the caller decodes forward.
    // Frames more than 2 s before t that some sources still deliver after a seek are dropped.
    HRESULT Seek(TimeUs t_us);
    // S_OK frame; S_FALSE end of stream; E_PENDING when every pool texture is still held (nothing was consumed).
    HRESULT ReadFrame(DecodedFrame& out);
    // As ReadFrame, but frames that end at or before skip_before_us are decoded and dropped without the copy
    // (fast-forward after a seek).
    HRESULT ReadFrame(DecodedFrame& out, TimeUs skip_before_us);
    void ReleaseFrame(uint32_t pool_index);

    // Views of a pool texture (created once per slot): Y plane R8_UNORM, UV plane R8G8_UNORM.
    ID3D11ShaderResourceView* YSrv(uint32_t pool_index) const;
    ID3D11ShaderResourceView* UvSrv(uint32_t pool_index) const;

    SizeI Size() const { return m_size; }   // pool texture size (aperture, even)
    int FpsNum() const { return m_fps_num; }
    int FpsDen() const { return m_fps_den; }
    int RotationDeg() const { return m_rotation; }
    TimeUs Duration() const { return m_duration_us; }
    TimeUs FrameDurationUs() const;          // nominal, from the frame rate
    bool IsHardware() const { return m_hardware; }   // last frame came as a D3D11 surface from a DXVA decoder
    bool AtEnd() const { return m_eos; }
    uint64_t FramesCopied() const { return m_frames_copied; }
    // Keyframe times learned while decoding (the first frame after Init and after every Seek: the reader lands on a
    // keyframe), sorted. Callers use them to choose between decoding forward and seeking.
    const std::vector<TimeUs>& KnownKeyframes() const { return m_keyframes; }
    uint64_t FramesSkipped() const { return m_frames_skipped; }

    // Copy box for a decoded frame: the display aperture clamped to the frame, origin and size rounded down to even
    // (NV12 copies need even boxes). An empty or absent aperture means the whole frame.
    static RectI ComputeApertureBox(SizeI frame, RectI aperture);

private:
    HRESULT ApplyCurrentType();
    HRESULT CreatePool(SizeI size);
    HRESULT CopyToPool(struct IMFSample* sample, uint32_t index);
    void NoteKeyframe(TimeUs pts);

    platform::D3DDevice* m_dev = nullptr;
    Config m_cfg;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
    unsigned long m_stream = 0;
    platform::TexturePool m_pool;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_y_srv, m_uv_srv;
    std::vector<uint8_t> m_scratch;   // software path: contiguous aperture NV12
    SizeI m_frame{};                  // MF_MT_FRAME_SIZE of the current output type
    RectI m_box{};                    // copy box
    SizeI m_size{};
    long m_stride = 0;
    int m_fps_num = 30, m_fps_den = 1;
    int m_rotation = 0;
    TimeUs m_duration_us = 0;
    TimeUs m_seek_target = -1;        // >= 0 right after Seek, until a frame at/after target - 2 s arrived
    bool m_first_after_seek = true;   // the next decoded frame is the keyframe the reader landed on
    std::vector<TimeUs> m_keyframes;
    bool m_hardware = false;
    bool m_eos = false;
    uint64_t m_frames_copied = 0;
    uint64_t m_frames_skipped = 0;
};

} // namespace ct::media
