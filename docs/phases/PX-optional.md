# PX · Optional extensions (after v1.0)

Each item is independent, has its own acceptance criteria, and must not change public headers of P00-P12
without a `DECISIONS.md` entry. Add a `PX-n` section to `PROGRESS.md` when one is started.

## PX-1 Window capture with Windows.Graphics.Capture (WGC)
Why: true per-window capture (occluded windows, no overlapping windows), Windows 10 1903+.
- Implement in a separate static library `ct_wgc` whose single translation unit is compiled with `/EHsc` and C++/WinRT headers (`winrt/Windows.Graphics.Capture.h`, `winrt/Windows.Graphics.DirectX.Direct3D11.h`, `windows.graphics.capture.interop.h`); expose a C++ interface with no WinRT types (`HRESULT`/pointers only) so the rest of the code stays exception-free.
- Flow: `IGraphicsCaptureItemInterop::CreateForWindow(hwnd)` (or `CreateForMonitor`), `CreateDirect3D11DeviceFromDXGIDevice`, `Direct3D11CaptureFramePool::CreateFreeThreaded(device, B8G8R8A8UIntNormalized, 2, size)`, `CreateCaptureSession(item)`, `session.IsCursorCaptureEnabled(false)` (we record the cursor as data), `session.IsBorderRequired(false)` (Windows 11 / 10 21H1+, needs `GraphicsCaptureAccess::RequestAccessAsync(Borderless)` on some builds), `FrameArrived` -> `IDirect3DDxgiInterfaceAccess::GetInterface(IID_ID3D11Texture2D)` -> copy into the BGRA pool; recreate the pool on `ContentSize` change (letterbox to the original recording size).
- `DesktopDuplicator` and the WGC source both implement a `FrameSource` interface consumed by `ScreenCapturer`.
- Acceptance: record a window partially covered by another; the recording shows only the target; CPU/GPU within P02 budgets.

## PX-2 Crash-safe recording with fragmented MP4
Why: today a kill during recording loses the whole `screen.mp4` (no `moov`).
- Option A (first try): `MFCreateFMPEG4MediaSink(byte_stream, video_type, audio_type, &sink)` + `MFCreateSinkWriterFromMediaSink`; the sink writes `moof/mdat` fragments. Verify the output opens in the MF source reader, VLC and browsers; if the MP4 sink requires a single audio type, write mic and system to separate files in this mode.
- Option B: a custom fMP4 muxer (`ct::media::Fmp4Muxer`) fed by running the encoder MFTs directly (`IMFTransform` with `MFT_MESSAGE_SET_D3D_MANAGER`, async MFT event loop for hardware encoders, `MF_MT_MPEG_SEQUENCE_HEADER` for SPS/PPS -> `avcC`), writing `ftyp`, `moov` (`mvex`, empty `stbl`), then one `moof`+`mdat` per second with `tfdt` timestamps, flushed with `FlushFileBuffers`. Finalization optionally rewrites to a regular MP4 for compatibility.
- Acceptance: kill the process at a random time during a 10 min recording 10 times; every file plays up to the last second in VLC and in the editor.

## PX-3 Per-process audio capture / exclude own audio
`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, IID_IAudioClient, &params{ AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK, { target_pid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE | EXCLUDE... } })` (Windows 10 2004+). UI: "System audio: all / only app… / exclude Camtesia". Acceptance: recording a browser tab's audio excludes a music player; editor preview audio is excluded while recording.

## PX-4 Custom title bar for the editor
`WM_NCCALCSIZE` returning 0 for the client area, `WM_NCHITTEST` zones (caption drag area, resize borders, `HTMINBUTTON/HTMAXBUTTON/HTCLOSE` for ImGui-drawn buttons so Windows 11 Snap Layouts appear on hover), `DwmExtendFrameIntoClientArea` with a 1 px top margin, `WM_GETMINMAXINFO` to respect the work area when maximized. Acceptance: snap layouts, Aero shake, Win+arrows, DPI changes all work; no flicker on resize.

## PX-5 Auto-update (opt-in)
Check `https://<owner-host>/camtesia/latest.json` (`{version, url, sha256, notes}`) via WinHTTP at most once per day; download to `%TEMP%`, verify SHA-256 and Authenticode (`WinVerifyTrust`) when signed; run the installer `/SILENT /CLOSEAPPLICATIONS` on user confirmation. Acceptance: update from version N to N+1 on a test machine preserves settings and recordings.

## PX-6 Cross-adapter capture fast path
Monitor attached to a second GPU: duplicate on that adapter's device and transfer frames through a shared texture (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE | KEYED_MUTEX`), or copy via staging on the secondary adapter and `UpdateSubresource` on the primary at reduced fps as a fallback. Acceptance: laptop with iGPU + dGPU records the external monitor at 60 fps with 0 drops.

## PX-7 Light theme and high-contrast
Second token set in `theme.h`, follows the Windows app mode (`UISettings`/registry `AppsUseLightTheme`), respects `SPI_GETHIGHCONTRAST`. Acceptance: contrast >= 4.5:1 for all tokens in both themes.

## PX-8 Localization
Move `strings.h` constants into a table keyed by id, load `assets/lang/<code>.json` at start, fall back to English. Acceptance: German and Japanese UI with no clipped layouts in the bar and dialogs (ImGui font merge for CJK ranges from `YuGothic`/`Meiryo`).

## PX-9 Cursor-following auto reframe for vertical export
For 9:16 exports from a 16:9 canvas: generate camera keys that keep the cursor/zoom target centered horizontally, smoothed with the same Auto-Follow logic. Acceptance: a 16:9 tutorial exported 1080x1920 keeps the pointer visible at all times.
