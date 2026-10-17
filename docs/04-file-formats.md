# 04 · File formats

All formats are versioned, little-endian, and forward-tolerant (unknown JSON keys are preserved on load/save,
unknown binary record types are skipped by size).

## 1. Recording bundle `Name.ctrec/`

A folder (not a single file) so that every stream is a standard file a user can open elsewhere.

```
2026-10-17 12-03-44.ctrec/
├─ meta.json        required
├─ screen.mp4       required  H.264/HEVC video + 0..2 AAC audio streams (mic, system)
├─ cam.mp4          optional  webcam video (same epoch)
└─ events.ctev      required  cursor / mouse / keyboard / focus / marker events
```

### meta.json

```json
{
  "format": "ctrec", "version": 1,
  "app_version": "0.1.0", "created_utc": "2026-10-17T10:03:44Z",
  "epoch": { "qpc": 123456789012, "qpc_frequency": 10000000 },
  "duration_us": 185000000,
  "screen": {
    "file": "screen.mp4", "width": 1920, "height": 1080, "fps_num": 60, "fps_den": 1, "codec": "h264",
    "region": { "x": 0, "y": 0, "w": 1920, "h": 1080 },
    "monitor": { "device": "\\\\.\\DISPLAY1", "name": "DELL U2723QE", "origin_x": 0, "origin_y": 0, "dpi": 144, "hdr": false },
    "scale": 1.0
  },
  "camera": { "file": "cam.mp4", "width": 1280, "height": 720, "fps_num": 30, "fps_den": 1, "device": "Logitech BRIO" },
  "audio": {
    "mic":    { "stream_index": 1, "device": "Microphone (Yeti)", "sample_rate": 48000, "channels": 2 },
    "system": { "stream_index": 2, "device": "Speakers (Realtek)", "sample_rate": 48000, "channels": 2 }
  },
  "events": { "file": "events.ctev", "cursor_hz": 120, "keystrokes": "shortcuts" },
  "system": { "os": "10.0.26100", "gpu": "AMD Radeon(TM) Graphics", "encoder": "AMDh264Encoder (hardware)" },
  "pauses": [ { "at_us": 60000000, "duration_us": 12000000 } ]
}
```

- `camera`, `audio.mic`, `audio.system` are `null` when not recorded.
- `region` is in physical pixels of the virtual screen. `scale` is `width / region.w` (always 1.0 in v1; reserved for downscaled capture).
- `stream_index` refers to the MP4 stream order inside `screen.mp4` as the sink writer created them (video is 0).
- `pauses` lists wall-clock pauses in media time for the UI; media files contain no gaps.

## 2. Events file `events.ctev`

Binary, append-only, flushed every second so a crash loses at most one second.
All integers little-endian. Times are media time in microseconds (pause-adjusted), relative to the epoch.
Coordinates are physical pixels **relative to the capture region's top-left**, so `(x, y)` maps 1:1 onto video pixels.

```
File    := Header Record*
Header  := magic u8[4] = "CTEV"
           version u16 = 1
           header_size u16 = 44          (4+2+2+8+8+16+4; readers accept any value >= 40, multiple of 4, and skip to it)
           epoch_qpc i64
           qpc_frequency i64
           region_x i32, region_y i32, region_w i32, region_h i32
           flags u32               bit0 = keystrokes recorded, bit1 = all keys (else shortcuts only)
Record  := size u32                total bytes including this field (multiple of 4, padded with zeros)
           type u8, pad u8[3]
           t_us i64
           payload u8[size - 16]
```

| type | name | payload |
|---|---|---|
| 1 | CURSOR_MOVE | `i32 x, i32 y, u32 shape_id, u8 visible, u8 pad[3]` |
| 2 | MOUSE_BUTTON | `u8 button (0 L, 1 R, 2 M, 3 X1, 4 X2), u8 down, u8 pad[2], i32 x, i32 y` |
| 3 | MOUSE_WHEEL | `i16 delta_v, i16 delta_h, i32 x, i32 y` |
| 4 | KEY | `u16 vk, u16 scancode, u8 down, u8 modifiers (bit0 Ctrl, 1 Shift, 2 Alt, 3 Win), u8 extended, u8 pad, u32 utf32_char (0 if none), char label[32] (UTF-8, zero-terminated, e.g. "Ctrl", "A", "Enter")` |
| 5 | CURSOR_SHAPE | `u32 shape_id, u16 w, u16 h, i16 hot_x, i16 hot_y, u8 format (0 = BGRA premultiplied), u8 compression (0 raw, 1 RLE), u16 pad, u32 data_size, u8 data[data_size]` |
| 6 | WINDOW_FOCUS | `u64 hwnd, i32 x, i32 y, i32 w, i32 h, char title[256] (UTF-8, zero-terminated), char exe[64]` |
| 7 | MARKER | `u32 kind (0 user hotkey), char label[64]` |
| 8 | PAUSE | `u8 paused (1 = pause began, 0 = resumed), u8 pad[3]` |
| 9 | TYPING | `u8 started (1 = burst started, 0 = ended), u8 pad[3]` (derived at record time: key events closer than 1 s) |

Rules:
- CURSOR_MOVE is written at most 120 times per second and only when `(x, y, shape_id, visible)` changed.
- A CURSOR_SHAPE record always precedes the first CURSOR_MOVE that references its `shape_id`. `shape_id` is the FNV-1a 32-bit hash of the premultiplied BGRA pixels + hotspot. Shapes are written once per file.
- RLE for shapes: runs of identical 32-bit pixels as `u8 run_length (1..255), u32 pixel`. Choose RLE when it is smaller.
- Writers pad every record to 4 bytes. Readers skip unknown `type` values by `size`.
- Reader API (`ct::events::EventLog`, module `ct_events`) loads the whole file into typed arrays sorted by time and provides `CursorAt(t_us)` with linear interpolation between neighbours and shape lookups.

## 3. Project file `.ctproj` (JSON, UTF-8, pretty-printed with 2 spaces, keys in the order below)

```json
{
  "format": "ctproj", "version": 1, "app_version": "0.1.0", "id": "7c1d4e6a-...uuid...",
  "name": "My recording",
  "canvas": { "width": 1920, "height": 1080, "fps_num": 60, "fps_den": 1,
              "background": { "kind": "gradient", "color_a": "#1B2735", "color_b": "#090A0F", "angle_deg": 135, "image": null, "blur": 0 },
              "padding_px": 96, "corner_radius_px": 18,
              "shadow": { "enabled": true, "blur_px": 48, "offset_y_px": 24, "opacity": 0.45 } },
  "media": [
    { "id": "m1", "kind": "recording", "path": "rel/or/abs/2026-10-17 12-03-44.ctrec", "duration_us": 185000000,
      "video": { "width": 1920, "height": 1080, "fps_num": 60, "fps_den": 1 }, "audio_streams": [ "mic", "system" ], "has_events": true, "has_camera": true },
    { "id": "m2", "kind": "video|audio|image", "path": "C:/x.mp4", "duration_us": 10000000, "video": { }, "audio_streams": [ "default" ] }
  ],
  "tracks": [
    { "id": "t1", "kind": "video", "name": "Screen", "muted": false, "locked": false, "hidden": false, "height_px": 72 },
    { "id": "t2", "kind": "video", "name": "Camera" },
    { "id": "t3", "kind": "audio", "name": "Mic" },
    { "id": "t4", "kind": "annotations", "name": "Callouts" }
  ],
  "clips": [
    { "id": "c1", "track": "t1", "media": "m1", "source": "screen",
      "start_us": 0, "in_us": 0, "out_us": 185000000, "speed": 1.0,
      "transform": { "x": 0.0, "y": 0.0, "scale": 1.0, "rotation_deg": 0.0, "opacity": 1.0, "anchor": "center" },
      "crop": { "left": 0, "top": 0, "right": 0, "bottom": 0 },
      "corner_radius_px": null, "shadow": null,
      "audio": { "gain_db": 0.0, "muted": false, "fade_in_us": 0, "fade_out_us": 0, "normalize": false, "noise_gate_db": null },
      "keyframes": { "transform.opacity": [ { "t_us": 0, "v": 1.0, "ease": "linear" } ] },
      "cursor": { "visible": true, "scale": 1.5, "smoothing": 0.35, "click_effect": "ripple", "click_color": "#4F8CFF", "hide_when_idle_us": 3000000, "spotlight": false },
      "keystrokes": { "enabled": true, "position": "bottom-center", "style": "pill", "hold_ms": 1200 } },
    { "id": "c2", "track": "t2", "media": "m1", "source": "camera", "start_us": 0, "in_us": 0, "out_us": 185000000,
      "pip": { "corner": "bottom-right", "size": 0.22, "margin_px": 32, "mask": "circle|rounded|none", "radius_px": 24, "shadow": true, "mirror": false } },
    { "id": "c3", "track": "t4", "kind": "annotation", "start_us": 5000000, "out_us": 9000000,
      "annotation": { "type": "callout", "text": "Click here", "font": "Segoe UI", "size_px": 40, "weight": 600,
                      "color": "#FFFFFF", "fill": "#4F8CFF", "stroke": "#000000", "stroke_px": 0, "radius_px": 12,
                      "rect": { "x": 0.6, "y": 0.2, "w": 0.25, "h": 0.1 }, "pointer": { "x": 0.55, "y": 0.35 },
                      "anim_in": "pop", "anim_out": "fade", "anim_ms": 220 } }
  ],
  "transitions": [ { "track": "t1", "between": [ "c1", "c9" ], "kind": "crossfade|dip|slide", "duration_us": 500000, "color": "#000000", "direction": "left" } ],
  "camera": {
    "mode": "manual|auto",
    "keyframes": [ { "t_us": 0, "zoom": 1.0, "cx": 0.5, "cy": 0.5, "ease": "inout" } ],
    "auto": { "zoom": 2.0, "min_hold_us": 1200000, "lead_us": 150000, "ease_ms": 650, "follow": true, "follow_deadzone": 0.15, "trigger_on": [ "click", "typing" ] }
  },
  "markers": [ { "t_us": 12000000, "label": "intro ends", "color": "#FFB020" } ],
  "ui": { "playhead_us": 0, "zoom": 1.0, "scroll_us": 0, "selected": [ ], "loop": null }
}
```

Conventions:
- Normalized coordinates (`0..1` of the canvas) for transforms, annotations and camera center; pixels only where suffixed `_px`.
- `keyframes` is a map from property path to an array sorted by `t_us`; a property without keyframes uses its static value. Easings: `linear`, `in`, `out`, `inout`, `hold`.
- Clips on `video` tracks with `source: "screen"` carry `cursor` and `keystrokes` settings; the compositor reads events from the media's `.ctrec`.
- Timeline invariants (checked by `Project::Validate()`): clips on one track never overlap; `0 <= in_us < out_us <= duration_us * speed`; ids unique; referenced ids exist; keyframe arrays sorted; canvas dimensions even and between 16 and 8192.
- Paths: stored relative to the project file when inside the same drive, else absolute. On load, try relative, then absolute, then prompt (missing media is a red clip, project still opens).
- Version migration: `project::Migrate(json, from_version)`; never refuse to open an older file.

## 4. Settings `%APPDATA%\Camtesia\settings.json`

```json
{
  "version": 1,
  "recording": { "fps": 60, "codec": "h264", "quality": 70, "mic_device": null, "system_audio": true, "mic": true,
                 "webcam_device": null, "webcam_resolution": "1280x720", "countdown_s": 3, "keystrokes": "shortcuts",
                 "cursor_hz": 120, "output_dir": null, "hide_bar_while_recording": true },
  "hotkeys": { "toggle": "F9", "pause": "F10", "marker": "F8", "cancel": "Ctrl+Shift+F9" },
  "editor": { "theme": "dark", "autosave_s": 60, "frame_cache_mb": 256, "snap": true, "ripple": false, "preview_quality": "full|half" },
  "export": { "last_preset": "mp4-1080p", "last_dir": null },
  "privacy": { "crash_dumps": true },
  "window": { "editor": { "x": 100, "y": 100, "w": 1600, "h": 900, "maximized": false }, "bar": { "x": 0, "y": 0 } }
}
```

## 5. Export presets `assets/presets/*.json`

```json
{ "id": "mp4-1080p", "name": "MP4 1080p (H.264)", "container": "mp4", "video": { "codec": "h264", "width": 1920, "height": 1080, "fps": "source", "rate_control": "vbr", "bitrate_kbps": 10000, "max_bitrate_kbps": 16000, "quality": 75, "gop_s": 2, "bframes": true, "profile": "high" }, "audio": { "codec": "aac", "bitrate_kbps": 192, "sample_rate": 48000, "channels": 2 }, "fast_start": true }
```

Built-in ids: `mp4-720p`, `mp4-1080p`, `mp4-1440p`, `mp4-4k`, `mp4-hevc-1080p`, `mp4-hevc-4k`, `gif-720p`, `gif-480p`, `png-frame`, `m4a-audio`. User presets save to `%APPDATA%\Camtesia\presets\`.

## 6. Crash recovery file

`%LOCALAPPDATA%\Camtesia\autosave\<project-id>.ctproj` is written every `autosave_s` and on every 20th undo-able
edit. A sidecar `<project-id>.lock` holds the owning PID; a lock whose PID is dead on startup triggers the
"Recover unsaved project?" prompt.
