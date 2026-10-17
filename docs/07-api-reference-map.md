# 07 · API reference map

Which page to open when an API misbehaves. All Microsoft links are on `learn.microsoft.com`; if one has moved,
search its title there. Read the "Remarks" section of a function page before changing flags blindly.

## Direct3D / DXGI

| Topic | Page |
|---|---|
| Desktop Duplication overview (ACCESS_LOST rules, pointer shapes, secure desktop) | https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api |
| `IDXGIOutputDuplication::AcquireNextFrame` / `ReleaseFrame` / `GetFramePointerShape` | https://learn.microsoft.com/windows/win32/api/dxgi1_2/nn-dxgi1_2-idxgioutputduplication |
| `IDXGIOutput5::DuplicateOutput1` (format list, HDR FP16) | https://learn.microsoft.com/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1 |
| `IDXGIOutput6::GetDesc1` (color space, HDR detection) | https://learn.microsoft.com/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgioutput6-getdesc1 |
| Flip model swap chains, waitable object, `DXGI_SWAP_EFFECT_FLIP_DISCARD` | https://learn.microsoft.com/windows/win32/direct3ddxgi/dxgi-flip-model and https://learn.microsoft.com/windows/win32/direct3ddarticles/dxgi-1-3-improvements |
| `D3D11CreateDevice` flags (`BGRA_SUPPORT`, `VIDEO_SUPPORT`, `DEBUG`) | https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice |
| Multithread protection (`ID3D10Multithread`) | https://learn.microsoft.com/windows/win32/api/d3d10misc/nn-d3d10misc-id3d10multithread |
| NV12 views (R8 / R8G8 per plane), `CheckFormatSupport` | https://learn.microsoft.com/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format (see NV12 remarks) |
| `ID3D11VideoProcessor` (fallback color conversion) | https://learn.microsoft.com/windows/win32/api/d3d11/nn-d3d11-id3d11videoprocessor |
| `ID3D11InfoQueue` (debug layer breaks) | https://learn.microsoft.com/windows/win32/api/d3d11sdklayers/nn-d3d11sdklayers-id3d11infoqueue |
| GPU timestamp queries | https://learn.microsoft.com/windows/win32/api/d3d11/ne-d3d11-d3d11_query |
| HDR on Windows (scRGB FP16, nits) | https://learn.microsoft.com/windows/win32/direct3darticles/high-dynamic-range |

## Media Foundation

| Topic | Page |
|---|---|
| Sink Writer (MP4 writing, attributes, `Finalize`) | https://learn.microsoft.com/windows/win32/medfound/sink-writer |
| Sink Writer attributes (`MF_SINK_WRITER_D3D_MANAGER`, `DISABLE_THROTTLING`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`) | https://learn.microsoft.com/windows/win32/medfound/sink-writer-attributes |
| `IMFSinkWriter::SetInputMediaType` (encoder parameters argument) | https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-setinputmediatype |
| H.264 encoder MFT (supported types, ICodecAPI properties) | https://learn.microsoft.com/windows/win32/medfound/h-264-video-encoder |
| H.265/HEVC encoder MFT | https://learn.microsoft.com/windows/win32/medfound/h-265---hevc-video-encoder |
| AAC encoder (input PCM 16-bit, bitrates 12000/16000/20000/24000 B/s) | https://learn.microsoft.com/windows/win32/medfound/aac-encoder |
| Codec API properties (`CODECAPI_AVEncCommonRateControlMode`, quality, GOP, low latency) | https://learn.microsoft.com/windows/win32/medfound/codec-api-properties |
| Media type attributes (`MF_MT_*`) | https://learn.microsoft.com/windows/win32/medfound/media-type-attributes |
| Video media types, nominal range, YUV matrix | https://learn.microsoft.com/windows/win32/medfound/video-media-types and https://learn.microsoft.com/windows/win32/medfound/extended-color-information |
| `MFCreateDXGISurfaceBuffer` | https://learn.microsoft.com/windows/win32/api/mfapi/nf-mfapi-mfcreatedxgisurfacebuffer |
| `IMFDXGIDeviceManager` / `MFCreateDXGIDeviceManager` | https://learn.microsoft.com/windows/win32/api/mfobjects/nn-mfobjects-imfdxgidevicemanager |
| Direct3D 11 video APIs in MF (device manager, decoder output textures) | https://learn.microsoft.com/windows/win32/medfound/direct3d-11-video-apis |
| Source Reader (reading, seeking, hardware decode) | https://learn.microsoft.com/windows/win32/medfound/source-reader |
| Source Reader attributes (`MF_SOURCE_READER_D3D_MANAGER`, `ENABLE_ADVANCED_VIDEO_PROCESSING`) | https://learn.microsoft.com/windows/win32/medfound/source-reader-attributes |
| `IMFSourceReader::ReadSample` flags | https://learn.microsoft.com/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-readsample |
| `IMFDXGIBuffer::GetResource` / `GetSubresourceIndex` | https://learn.microsoft.com/windows/win32/api/mfobjects/nn-mfobjects-imfdxgibuffer |
| Enumerating encoders `MFTEnumEx` (hardware flag) | https://learn.microsoft.com/windows/win32/api/mfapi/nf-mfapi-mftenumex |
| Webcam enumeration `MFEnumDeviceSources` | https://learn.microsoft.com/windows/win32/medfound/audio-video-capture-in-media-foundation |
| Fragmented MP4 sink `MFCreateFMPEG4MediaSink` (optional PX) | https://learn.microsoft.com/windows/win32/api/mfidl/nf-mfidl-mfcreatefmpeg4mediasink |
| `MF_MPEG4SINK_MOOV_BEFORE_MDAT` (fast start) | https://learn.microsoft.com/windows/win32/medfound/mf-mpeg4sink-moov-before-mdat |
| `mftrace` tool | https://learn.microsoft.com/windows/win32/medfound/using-mftrace |

## Core Audio (WASAPI)

| Topic | Page |
|---|---|
| Loopback recording | https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording |
| Capturing a stream (event-driven pattern) | https://learn.microsoft.com/windows/win32/coreaudio/capturing-a-stream |
| Rendering a stream | https://learn.microsoft.com/windows/win32/coreaudio/rendering-a-stream |
| `IAudioClient::Initialize` flags (`AUTOCONVERTPCM`, `SRC_DEFAULT_QUALITY`, `LOOPBACK`, `EVENTCALLBACK`) | https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize and https://learn.microsoft.com/windows/win32/coreaudio/audclnt-streamflags-xxx-constants |
| `IAudioCaptureClient::GetBuffer` (QPC position, flags) | https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer |
| `IAudioClock::GetPosition` (render clock) | https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition |
| Device events `IMMNotificationClient` | https://learn.microsoft.com/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient |
| Per-process loopback (optional PX) | https://learn.microsoft.com/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params |
| MMCSS (`AvSetMmThreadCharacteristics`) | https://learn.microsoft.com/windows/win32/procthread/multimedia-class-scheduler-service |

## Win32 UI, input, system

| Topic | Page |
|---|---|
| Low-level hooks (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`, timeout behaviour) | https://learn.microsoft.com/windows/win32/winmsg/about-hooks and https://learn.microsoft.com/windows/win32/winmsg/lowlevelmouseproc |
| `RegisterHotKey` | https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-registerhotkey |
| `SetWindowDisplayAffinity` (`WDA_EXCLUDEFROMCAPTURE`) | https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity |
| Cursor info and icons (`GetCursorInfo`, `GetIconInfoExW`, `DrawIconEx`) | https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getcursorinfo |
| Layered windows (`UpdateLayeredWindow`) | https://learn.microsoft.com/windows/win32/winmsg/window-features#layered-windows |
| DWM attributes (dark mode `DWMWA_USE_IMMERSIVE_DARK_MODE`, corners, `DWMWA_EXTENDED_FRAME_BOUNDS`, `DWMWA_CLOAKED`) | https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute |
| High DPI (Per-Monitor V2, `WM_DPICHANGED`) | https://learn.microsoft.com/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows |
| Monitors (`EnumDisplayMonitors`, `GetDpiForMonitor`, `QueryDisplayConfig` for names) | https://learn.microsoft.com/windows/win32/gdi/multiple-display-monitors |
| Waitable timers high resolution | https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw |
| Shell notify icon (tray) | https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw |
| Known folders | https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath |
| File dialogs `IFileOpenDialog` | https://learn.microsoft.com/windows/win32/shell/common-file-dialog |
| Minidumps | https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump |
| TraceLogging | https://learn.microsoft.com/windows/win32/tracelogging/trace-logging-portal |
| `SetThreadDescription` | https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription |
| Power events `WM_POWERBROADCAST` | https://learn.microsoft.com/windows/win32/power/wm-powerbroadcast |

## Direct2D / DirectWrite / WIC

| Topic | Page |
|---|---|
| D2D on a D3D11 surface (`ID2D1Device`, `CreateBitmapFromDxgiSurface`) | https://learn.microsoft.com/windows/win32/direct2d/devices-and-device-contexts |
| DirectWrite text layout | https://learn.microsoft.com/windows/win32/directwrite/direct-write-portal |
| D2D effects (Gaussian blur for shadows and blur annotation) | https://learn.microsoft.com/windows/win32/direct2d/effects-overview |
| WIC encode/decode PNG/JPEG | https://learn.microsoft.com/windows/win32/wic/-wic-lh |

## Other

| Topic | Page |
|---|---|
| Dear ImGui docs and docking | https://github.com/ocornut/imgui/wiki and `imgui.h` comments |
| Inno Setup script reference | https://jrsoftware.org/ishelp/ |
| GIF89a spec (LZW, NETSCAPE loop extension) | https://www.w3.org/Graphics/GIF/spec-gif89a.txt |
| Windows.Graphics.Capture (optional PX) | https://learn.microsoft.com/windows/uwp/audio-video-camera/screen-capture |
| BT.709 matrices | https://learn.microsoft.com/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering |
