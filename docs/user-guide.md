# Camtesia user guide

> Describes Camtesia 1.0 as specified; some features are still in development.

Camtesia records your screen, microphone, system audio and webcam, then lets you edit the result on a timeline
and export it. The editor re-renders the cursor, zooms in on your clicks, and frames the recording on a padded,
rounded background.

Contents:
[Install](#1-install) ·
[First recording](#2-your-first-recording) ·
[Where files go](#3-where-files-go) ·
[The .ctrec bundle](#4-the-ctrec-recording-bundle) ·
[Editor](#5-the-editor) ·
[Export](#6-export) ·
[Settings](#7-settings) ·
[Privacy](#8-privacy) ·
[Troubleshooting](#9-troubleshooting) ·
[Uninstall](#10-uninstall)

---

## 1. Install

### Requirements

- Windows 10 version 2004 (build 19041) or later, 64-bit. Windows 11 is the main test target.
- No other software. Camtesia is one executable with no runtimes or redistributables.
- A GPU with a hardware video encoder is recommended. Without one, Camtesia uses the Windows software encoder.

### Installer

1. Run `CamtesiaSetup-x.y.z.exe`.
2. By default it installs for the current user only and needs no administrator rights. The program goes to
   `%LOCALAPPDATA%\Programs\Camtesia`. Choose "Install for all users" in the first dialog to install to
   `Program Files` instead (this asks for administrator rights).
3. Optional tasks:
   - **Create a desktop shortcut** (off by default).
   - **Start Camtesia in the tray when Windows starts** (off by default).
4. Setup adds a Start menu entry and opens `.ctproj` project files with Camtesia when you double-click them.

Unsigned builds trigger a Windows SmartScreen warning. Choose **More info** and then **Run anyway**.

For scripted installs, the standard Inno Setup switches work, for example:

```
CamtesiaSetup-1.0.0.exe /VERYSILENT /CURRENTUSER /TASKS=desktopicon,startup
```

### Portable ZIP

1. Extract `Camtesia-x.y.z-portable.zip` to any folder, for example a USB drive.
2. Run `Camtesia.exe`.

The ZIP contains a file named `portable.txt`. While it sits next to the exe, Camtesia keeps all its data
(settings, logs, crash dumps, autosaves and recordings) in a `data\` folder next to the exe. Nothing is written to
your user profile. The portable version does not register the `.ctproj` file type.

### Verify a download

Each release ships `SHA256SUMS.txt`. In PowerShell:

```
Get-FileHash .\CamtesiaSetup-1.0.0.exe -Algorithm SHA256
```

Compare the result with the line in `SHA256SUMS.txt`.

---

## 2. Your first recording

### The recorder bar

Camtesia starts as a small floating bar at the bottom centre of your main monitor. The bar never appears in your
recordings. Drag it by the grip on the left; it remembers its position.

From left to right the bar has:

| Control | What it does |
|---|---|
| Grip | Drag to move the bar. |
| Source: **Monitor N** / **Region** / **Window** | What to record. See below. |
| fps | Frame rate, quality and codec. |
| Microphone + meter | Click to turn on or off. The arrow opens the device list. |
| System audio + meter | Click to turn on or off. The arrow opens the device list. |
| Camera | Click to turn on or off. The arrow lists cameras and resolutions. Hover for a live preview. |
| **REC** | Start recording (with countdown). |
| Gear | Open Settings. |
| Close | Hide the bar to the system tray. |

The level meters are green below -18 dBFS, amber up to -6 dBFS and red above. Speak at a level that stays mostly
green and amber.

On first start, Camtesia shows one tip: "Press F9 to start recording. Recordings are saved to Videos\Camtesia".

### Choose what to record

- **Monitor N** opens a list of your monitors with thumbnails. Pick one to record the whole monitor.
- **Region** dims the screen so you can drag a rectangle.
- **Window** opens the same overlay in window-snap mode: hover a window and click it to use its rectangle.

In the region overlay:

| Input | Action |
|---|---|
| Drag | Draw the region. A size label (for example `1920 × 1080`) follows the cursor. |
| Click without dragging | Select the window under the cursor. |
| Drag a handle / drag inside | Resize / move the region. |
| Arrow keys | Nudge 1 px (`Shift` = 10 px). |
| `M` | Cycle through full-monitor presets. |
| `R` | Start the selection again. |
| `Enter` or `Space` | Confirm. |
| `Esc` | Cancel. |

Notes:

- Width and height are always even numbers (odd sizes round down). The smallest region is 64 × 64.
- A region that spans two monitors is clamped to the monitor that holds most of it.
- **Window** records the window's rectangle on screen, not the window itself. If you move the window or another
  window covers it, the recording shows what is on screen in that rectangle.
- While recording, a thin red frame marks the region. The frame is not recorded.

### Frame rate, quality and codec

Open the fps menu on the bar.

| Option | Choices | Default |
|---|---|---|
| Frame rate | 15, 24, 30, 60, native (up to 144 Hz if the hardware allows) | 60 if the monitor runs at 60 Hz or more, else 30 |
| Quality | Good, High, Best | High |
| Codec | H.264, HEVC | H.264 |

HEVC is greyed out when your GPU has no hardware HEVC encoder. Recordings are always constant frame rate.
On an HDR monitor the recording is converted to standard dynamic range (SDR).

### Audio and webcam

- Microphone and system audio are recorded as **separate tracks**, so you can adjust them independently later.
  Both are on by default. Audio is 48 kHz stereo AAC.
- If a device is unplugged or the Windows default device changes during a recording, Camtesia fills the gap with
  silence and switches to the new default device. The timeline stays in sync.
- The webcam is off by default. When on, it records to its own file at 1280 × 720 by default. You place it on the
  video as picture-in-picture in the editor.

### Start, pause, mark, stop

| Action | Bar | Hotkey (works from any app) |
|---|---|---|
| Start / stop | **REC** / stop button | `F9` |
| Pause / resume | pause button | `F10` |
| Add a marker | flag button | `F8` |
| Cancel and delete the recording | - | `Ctrl+Shift+F9` |

All hotkeys can be changed in Settings. If another app already uses a hotkey, Camtesia shows a message with the key
name and keeps running without it.

1. Press **REC** or `F9`. A 3-2-1 countdown appears in the middle of the capture area. Press `Esc` to cancel it.
   The countdown is not recorded.
2. Recording starts. The bar shrinks to a timer with pause, stop and marker buttons and a microphone meter, and the
   tray icon turns red. By default the bar hides itself during recording (setting **Hide bar while recording**);
   the hotkeys and tray menu still work.
3. Pause as often as you like. The saved video has no gaps; pauses are listed in the recording's metadata.
4. Press `F9` again to stop. Camtesia shows "Finalizing..." for a moment and then a notification:
   "Saved · 2026-10-17 12-03-44.ctrec" with **Open in editor**, **Show in folder** and **Delete**.

Markers you add during recording (`F8`) appear in the editor so you can find important moments quickly.

### The tray icon

Left-click shows or hides the bar. Right-click opens a menu: Record, Pause, Stop, Open editor, Settings, Quit.
While recording, the tooltip shows the elapsed time. When the bar is hidden, the "Saved" message appears as a
Windows notification.

### What happens automatically during a recording

| Situation | What Camtesia does |
|---|---|
| UAC prompt or `Ctrl+Alt+Del` screen | Repeats the last frame while Windows blocks capture, then continues. |
| Screen resolution changes | Keeps recording at the original size; the new image is letterboxed. |
| PC goes to sleep | Pauses the recording. After wake-up it stays paused; resume it yourself. |
| Free disk space below 1 GB | Shows a warning once. |
| Free disk space below 200 MB | Stops and saves the recording ("Disk almost full"). |
| GPU is reset by the driver | Saves the recording up to that point and shows a message. |

---

## 3. Where files go

| What | Installed version | Portable version |
|---|---|---|
| Recordings | `%USERPROFILE%\Videos\Camtesia\` (change in Settings > Recording) | `<exe folder>\data\recordings\` |
| Settings | `%APPDATA%\Camtesia\settings.json` | `<exe folder>\data\settings\` |
| Editor layout | `%APPDATA%\Camtesia\editor_layout.ini` | `<exe folder>\data\settings\` |
| Your export presets | `%APPDATA%\Camtesia\presets\` | `<exe folder>\data\settings\` |
| Autosaves (crash recovery) | `%LOCALAPPDATA%\Camtesia\autosave\` | `<exe folder>\data\autosave\` |
| Logs | `%LOCALAPPDATA%\Camtesia\logs\` | `<exe folder>\data\logs\` |
| Crash dumps | `%LOCALAPPDATA%\Camtesia\crashes\` | `<exe folder>\data\crashes\` |
| Exports | You choose; the default is `%USERPROFILE%\Videos\Camtesia\` | You choose |

Each recording is named after its start time, for example `2026-10-17 12-03-44.ctrec`. If that name exists,
Camtesia adds ` (2)`.

Projects (`.ctproj`) are saved wherever you choose. A project stores the paths of its media: relative to the project
file when both are on the same drive, otherwise absolute. Move projects and recordings together to keep the links.

---

## 4. The .ctrec recording bundle

A recording is a folder named `Name.ctrec`, not a single file. Inside are standard files you can open in other
programs: `screen.mp4` (the screen video with the microphone and system audio as separate audio tracks),
`cam.mp4` (the webcam, if recorded), `events.ctev` (cursor positions at 120 Hz, clicks, scroll, cursor shapes,
keystrokes, the focused window, markers and pauses) and `meta.json` (devices, sizes, frame rate, encoder and
pauses). The cursor is **not** burned into `screen.mp4`; it is stored as data so the editor can redraw it sharp, at any
size and with smoothing. Keep the folder together: moving or deleting one file breaks the recording in the editor.

---

## 5. The editor

### Opening the editor

- Click **Open in editor** on the "Saved" notification.
- Right-click the tray icon > **Open editor**.
- Double-click a `.ctproj` file.
- Drop a `.ctrec` folder onto the editor window, or use File > Import Recording.

From the editor, `Ctrl+R` opens the recorder bar again.

### Layout

```
┌ File  Edit  View  Timeline  Camera  Export  Help                         Record │ Export ┐
├────────────┬─────────────────────────────────────────────────┬───────────────────────────┤
│ Media      │ Preview                                         │ Properties                │
│            │ transport: start, frame back, play, frame fwd,  │ (changes with selection)  │
│            │ end, time, fit, loop                            │                           │
├────────────┴─────────────────────────────────────────────────┴───────────────────────────┤
│ Timeline                                                                                 │
├──────────────────────────────────────────────────────────────────────────────────────────┤
│ Status: autosave age · fps · canvas size · cache use · warnings · GPU time               │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

All panels dock and resize. View > Reset layout restores the default. `Ctrl+1`..`Ctrl+4` focus Media, Preview,
Properties and Timeline.

### Media panel

- **Import...** (`Ctrl+I`) adds files. Supported: `.ctrec` recordings, MP4, MOV, M4A, MP3, WAV, PNG, JPG.
- Cards show a thumbnail, name, duration and badges: `REC` (a Camtesia recording), `CAM` (has webcam),
  `EVT` (has cursor and keystroke data).
- Drag a card onto the timeline to add it.
- Right-click a card: **Reveal** (in Explorer), **Relink** (point a missing file to its new location),
  **Remove** (asks first if clips use it).
- The **Effects** tab holds annotation presets: Text, Callout, Arrow, Rectangle, Ellipse, Highlight, Blur and
  Keystrokes. Drag one onto the timeline.

### What importing a recording does

When you import a `.ctrec`, Camtesia creates (or reuses) these tracks and places clips on them:

| Track | Content |
|---|---|
| Screen | The screen video, with cursor and keystroke settings |
| Camera | The webcam as picture-in-picture (only if recorded) |
| Mic | Microphone audio (only if recorded) |
| System | System audio (only if recorded) |
| Callouts | Empty annotation track |

In a new, empty project the canvas takes the recording's size and frame rate, and the polished style is applied:
96 px padding, 18 px rounded corners, a dark gradient background and a drop shadow. Auto-Zoom keyframes are generated
from your clicks and typing. Later recordings are added at the end of the timeline.

### Timeline

**Mouse**

| Input | Action |
|---|---|
| Click a clip | Select it. `Ctrl`+click toggles; `Shift`+click selects a range on the same track. |
| Drag empty space | Rubber-band selection. |
| Drag a clip | Move it. It snaps to clip edges, the playhead, markers and 0 within 8 px. Hold `Alt` to disable snapping. |
| Drag a clip to another track | Moves it to a compatible track. |
| Drag a clip edge | Trim. |
| `Alt`+drag a clip | Slip: change which part of the media plays, without moving the clip. |
| `Ctrl`+drag a clip | Duplicate. |
| Double-click a clip | Open its properties. |
| Right-click a clip | Split, Delete, Ripple delete, Duplicate, Mute, Speed..., Reset transform, Add transition..., Color... |
| Drag on the ruler | Scrub. |
| `Wheel` / `Shift+Wheel` | Scroll horizontally / vertically. |
| `Ctrl+Wheel` | Zoom around the mouse. |
| `Alt+Wheel` | Change track heights. |

The track header has mute, lock and hide toggles; drag a header to reorder tracks. Audio clips show waveforms,
video clips show thumbnails, fades show as diagonal lines and a changed speed shows a badge such as `1.5×`.
Clips on the same track can never overlap.

**Ripple mode** (`Ctrl+Shift+R`, off by default): deleting a clip closes the gap after it. **Snapping**
(`N`, on by default) is shown in the timeline toolbar.

**Markers** appear as diamonds on the ruler. Press `M` to add one at the playhead, drag to move, double-click to
rename.

**Clip properties** include name, colour, speed, transform (position, scale, rotation, opacity), crop, corner radius
and shadow. Audio clips have gain, mute, fade in/out, normalize and a noise gate.

**Keyframes.** Fields with a small ◇ can be animated: click ◇ to set a keyframe at the playhead (it fills when a
keyframe exists there). Easing options are linear, ease in, ease out, ease in-out and hold. In any numeric field,
`Alt`+click resets the value and `Ctrl`+click lets you type it.

### Keyboard shortcuts

| Keys | Action |
|---|---|
| `Space` | Play / pause |
| `J` / `K` / `L` | Play backwards / stop / play forwards. Press `L` or `J` again for 2× and 4×. `Shift+L` / `Shift+J` play at 0.5×. |
| `←` / `→` | Previous / next frame (`Shift` = 1 s) |
| `,` / `.` | Move the selected clips one frame left / right (`Shift` = 10 frames) |
| `Home` / `End` | Go to start / end |
| `I` / `O` | Set loop in / out point. `Alt+I` / `Alt+O` clear them. |
| `S` | Split the selected clips at the playhead (or the clip under the playhead) |
| `Delete` / `Backspace` | Delete the selection (ripples if ripple mode is on) |
| `Shift+Delete` | Ripple delete, always |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` / `Ctrl+Y` | Redo |
| `Ctrl+X` / `Ctrl+C` / `Ctrl+V` / `Ctrl+D` | Cut / copy / paste at the playhead / duplicate |
| `Ctrl+A` | Select all clips |
| `M` | Add marker |
| `Z` / `Shift+Z` | Add camera keyframe at the playhead / remove the nearest one |
| `+` / `-` / `F` | Zoom the timeline in / out / fit the whole project |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` | New / open / save / save as |
| `Ctrl+I` | Import media |
| `Ctrl+E` | Export |
| `Ctrl+R` | Open the recorder bar |
| `Ctrl+Shift+R` | Toggle ripple mode |
| `N` | Toggle snapping |
| `Ctrl+1`..`Ctrl+4` | Focus Media / Preview / Properties / Timeline |
| `F11` | Stats overlay (frame time, GPU time, cache, queues) |
| `?` / `F1` | Shortcut cheat sheet |

Undo keeps the last 200 steps (up to 64 MB of history). A whole drag counts as one step.

### Preview

- Zoom with **fit**, 50 %, 100 % or 200 %. Hold `Space` and drag to pan when zoomed in.
- Select a video clip to get handles: drag the body to move, a corner or edge to scale, the handle above to rotate.
  `Shift` keeps the aspect ratio and rotates in 15° steps. `Alt` scales from the opposite corner.
- Annotations move and resize the same way. Callouts have an extra handle for the pointer tip. Double-click a text
  annotation to edit its text in place.
- Toggles for safe areas (90 % / 80 %), a rule-of-thirds grid, and hiding the cursor or annotations. These only
  affect the preview, never the project.
- The transport has start, previous frame, play/pause, next frame, end, a time display (click it and type a time),
  loop on/off, and mute and volume.

Scrubbing shows the nearest cached frame immediately and the exact frame a moment later.

### Auto-Zoom and Auto-Follow

Auto-Zoom moves a virtual camera: it zooms in when you click or start typing, holds while you work in one area, and
zooms back out afterwards. Auto-Follow pans the zoomed view to keep the cursor in frame. Both are on by default.

Select nothing and open the **Camera** section in Properties (or use the Camera menu):

| Setting | Default | Meaning |
|---|---|---|
| Mode | Auto | Auto = generated keyframes; Manual = only your own keyframes |
| Zoom | 2.0× | How far to zoom in |
| Minimum hold | 1.2 s | Shortest time to stay zoomed in |
| Lead | 150 ms | How early the zoom starts before the click |
| Ease | 650 ms | Duration of each zoom in or out |
| Follow | On | Pan with the cursor while zoomed |
| Follow dead zone | 0.15 | How far the cursor may move before the camera follows |
| Triggers | Clicks and typing | Which events cause a zoom |

How it behaves:

- Clicks close together in time and place form one zoom. Distant clicks that follow each other quickly cause a pan
  instead of a zoom out and in.
- Clicks in roughly the first and last 0.8 s of a recording may not produce a zoom, because there is no room for the
  zoom ramp.
- Changing a setting regenerates the keyframes after a short delay. **Regenerate** does it on demand.

Camera keyframes appear as diamonds in the camera lane above the tracks; generated ones are dimmer. Drag a diamond to
move it in time, click to select it, double-click empty lane space to add one. When a key is selected, the preview
shows a dashed rectangle of the visible area: drag it to pan, drag a corner to zoom. **Editing a generated keyframe
turns it into a manual one**, and **Regenerate** only replaces generated keyframes, so your edits are kept.

### Cursor

Select the screen clip and open the **Cursor** section.

| Setting | Default |
|---|---|
| Visible | On |
| Size | 1.5× |
| Smoothing | 0.35 (0 = raw movement) |
| Click effect | Ripple (choices: none, ripple, highlight, both) |
| Click colour | Blue `#4F8CFF` |
| Hide when idle | After 3 s without movement (fades out) |
| Spotlight | Off (darkens everything outside a soft circle around the cursor) |
| Shadow | On |

The cursor uses the real cursor shapes from the recording (arrow, I-beam, hand and so on). Smoothing removes jitter;
clicks still land exactly where you clicked.

### Background, padding and canvas

Select nothing to see the **Background/Canvas** and **Project** sections.

| Setting | Default |
|---|---|
| Background | Gradient `#1B2735` to `#090A0F` at 135°. Also solid colour or image (wallpaper), with optional blur. |
| Padding | 96 px around the screen recording |
| Corner radius | 18 px |
| Shadow | On: blur 48 px, offset 24 px down, opacity 45 % |
| Canvas size | Same as the first recording. Presets: 16:9 (1080p, 4K), 9:16, 1:1, 4:5 |
| Frame rate | Same as the first recording |

For vertical or square videos, change the canvas aspect ratio here. Export sizes keep the canvas aspect ratio.
Individual clips can override corner radius and shadow.

### Annotations

Drag a preset from the **Effects** tab onto an annotation track, then edit it in Properties.

| Type | Use |
|---|---|
| Text | A line or block of text |
| Callout | Text in a rounded box with a pointer |
| Arrow | A line with an arrowhead |
| Rectangle, Ellipse | Outlines or filled shapes |
| Highlight | Draws attention to a region |
| Blur | Blurs a region (or pixelates it) to hide private information |
| Keystrokes | Shows keystroke badges (see below) |

Defaults: font Segoe UI, 48 px, semi-bold, white text on blue. Annotations pop in and fade out over 220 ms (also:
none, fade, pop, slide). Position and opacity can be keyframed. Fonts come from Windows; none are bundled.

### Keystroke badges

When keystrokes were recorded, the screen clip can show them as on-screen badges.

| Setting | Default | Choices |
|---|---|---|
| Show keystrokes | On | |
| Position | Bottom centre | bottom centre, bottom left, bottom right, top centre |
| Style | Pill | pill, keycap |
| Hold | 1200 ms | How long a badge stays |

Badges only show what was recorded. With the default "shortcuts only" mode, that means shortcuts such as `Ctrl+C`,
not ordinary typing. See [Privacy](#8-privacy).

### Webcam picture-in-picture

The camera clip sits in a corner of the canvas. Select it and open the **PIP** section.

| Setting | Default | Choices |
|---|---|---|
| Corner | Bottom right | any of the four corners |
| Size | 0.22 (fraction of the canvas) | |
| Margin | 32 px | |
| Mask | Rounded (24 px radius) | none, rounded, circle |
| Shadow | On | |
| Mirror | Off | |

### Transitions

Right-click a clip > **Add transition...** to blend it with the next clip on the same track.

| Transition | Options |
|---|---|
| Cut | (no transition) |
| Crossfade | duration |
| Dip to colour | duration, colour (default black) |
| Slide | duration, direction: left, right, up, down |

The default duration is 0.5 s.

### Saving, autosave and recovery

- `Ctrl+S` saves the project as a `.ctproj` file. The title bar shows `*` when there are unsaved changes, and closing
  asks whether to save.
- Camtesia autosaves to a separate recovery file every 60 s and after every 20 edits. Your `.ctproj` is only written
  when you save.
- If Camtesia closes unexpectedly, the next start asks "Recover unsaved project?".
- If a media file is missing when you open a project, its clips show in red. The project still opens; use **Relink**
  in the Media panel.

---

## 6. Export

Press `Ctrl+E` or click **Export**.

### The export dialog

- Left: presets (built-in and your own). **Save as preset...** stores the current settings.
- Right: format, codec, size (canvas, 1440p, 1080p, 720p or a custom even size), frame rate (canvas, 60, 30, 24),
  rate control and bitrate or quality, audio bitrate, GIF options, range (whole project or the loop region),
  destination, and the estimated file size.

Click **Export**. A progress window shows frames done, speed (for example `2.1x realtime`) and time remaining.
**Cancel** stops the export and deletes the partial file. You can keep editing while it runs; the export works on a
copy of the project, so your edits do not affect it. When it finishes, a notification offers **Open** and
**Show in folder**.

Exports use the same renderer as the preview, so the result looks exactly like what you see.

### Formats and presets

| Preset | Format | Notes |
|---|---|---|
| MP4 720p (H.264) | MP4 | |
| MP4 1080p (H.264) | MP4 | Default. VBR 10 Mbps (peak 16 Mbps), AAC 192 kbps |
| MP4 1440p (H.264) | MP4 | |
| MP4 4K (H.264) | MP4 | |
| MP4 1080p (HEVC) | MP4 | Needs a hardware HEVC encoder |
| MP4 4K (HEVC) | MP4 | Needs a hardware HEVC encoder |
| GIF 720p | GIF | |
| GIF 480p | GIF | |
| PNG frame | PNG | The frame at the playhead. JPEG is also available. |
| M4A audio | M4A (AAC) | Audio only |

MP4 details:

- Video: H.264 or HEVC with the hardware encoder. Rate control: quality, VBR or CBR.
- Audio: one stereo AAC track that mixes all audio clips, 128 to 320 kbps (default 192 kbps).
- Files are "fast start", so they begin playing in browsers before they finish downloading.
- The frame rate defaults to the canvas frame rate.

GIF details: 20 fps by default, up to 256 colours, dithering on, loops forever. Only changed pixels are stored
between frames, which keeps files small for screen recordings.

---

## 7. Settings

Open Settings from the gear on the recorder bar, the tray menu, or Edit > Preferences in the editor.

| Section | Settings |
|---|---|
| Recording | Frame rate, codec, quality, countdown (0-10 s, default 3), cursor sample rate (default 120 Hz), keystroke mode, output folder, hide bar while recording |
| Audio | Microphone device, system audio on/off, meter test |
| Camera | Device, resolution (default 1280 × 720) |
| Hotkeys | Start/stop, pause, marker, cancel. Conflicts with other apps are detected. |
| Editor | Autosave interval (default 60 s), frame cache size (default 256 MB), snap and ripple defaults, preview quality (full or half) |
| Export | Default preset, default folder |
| Privacy | Crash dumps on/off, **Open data folder** |
| About | Version, build, encoder report, GPU, **Copy diagnostics**, licences |

Changing a hotkey takes effect immediately. The interface is English only in version 1.0.

---

## 8. Privacy

- **Nothing leaves your computer.** Camtesia has no telemetry, no accounts, no cloud features and no automatic
  update checks.
- **Keystroke recording** has three modes (Settings > Recording):

  | Mode | What is recorded |
  |---|---|
  | Off | No keystrokes. The keyboard hook is not installed at all. |
  | Shortcuts only (default) | Only key presses while `Ctrl`, `Alt` or `Win` is held, plus `F1`-`F24` and `Esc`. Ordinary typing, including passwords, is not recorded. |
  | All keys | Every key press, including the characters typed. Use only when you need typing shown on screen. |

  The mode used is stored in the recording, and the editor can only show what was recorded.
- **Window titles.** Each recording's `events.ctev` stores the title and program name of the window that had focus
  during the recording. Keep this in mind before sharing a `.ctrec` folder. Exported videos contain only pixels and
  audio.
- **Crash dumps** are written only to your own disk (`%LOCALAPPDATA%\Camtesia\crashes\`) and are never sent anywhere.
  Turn them off in Settings > Privacy. A dump can contain parts of what was in memory, such as window titles.
- Use the **Blur** annotation to hide private details in the video before exporting.

---

## 9. Troubleshooting

Errors appear as a red banner or notification in plain words. **Details** shows the technical error code and the log
location, with an **Open log folder** button.

| Problem | Cause and fix |
|---|---|
| The video freezes for a few seconds | A UAC prompt, `Ctrl+Alt+Del`, the lock screen or a full-screen game blocked capture ("Windows blocked screen capture"). Windows does not allow recording the secure desktop. Camtesia repeats the last frame and continues automatically. |
| "No video encoder is available. Install the latest graphics driver." | Neither a hardware nor the Windows software encoder could be used. Update the graphics driver. On Windows N editions, install the Media Feature Pack. |
| HEVC is greyed out | Your GPU has no hardware HEVC encoder. Use H.264. |
| "The microphone is in use by another app. Recording continues without it." | Another program holds the microphone exclusively. Close it or turn off its exclusive mode (Windows Sound settings > device > Advanced). |
| A microphone was unplugged | Camtesia records silence and switches to the new default microphone. |
| "Not enough disk space on <drive>. Recording stopped and saved." | Free up space or choose another output folder in Settings > Recording. Camtesia warns at 1 GB free and stops at 200 MB. |
| "The graphics device was reset. The recording was saved up to this point." | The graphics driver restarted. Start a new recording. |
| A hotkey does not work | Another app registered it first. Camtesia showed the key name at startup. Choose another key in Settings > Hotkeys. |
| The recorder bar appears in recordings | Your Windows build does not support hiding windows from capture. Turn on **Hide bar while recording**. |
| "Media file not found: <path>" | The file was moved or deleted. Right-click it in the Media panel > **Relink**. |
| Camtesia crashed or was closed during a recording | Camtesia tries to save the recording when it crashes. If the process is killed (for example from Task Manager), `screen.mp4` cannot be recovered in version 1.0; `events.ctev` keeps everything up to the last second. |
| Camtesia crashed in the editor | Restart it and accept "Recover unsaved project?". |

### Logs and crash dumps

| What | Where |
|---|---|
| Logs | `%LOCALAPPDATA%\Camtesia\logs\camtesia-YYYYMMDD-HHMMSS.log`. A file rotates at 10 MB; the last 5 are kept. |
| Crash dumps | `%LOCALAPPDATA%\Camtesia\crashes\*.dmp` |
| Portable version | `<exe folder>\data\logs\` and `<exe folder>\data\crashes\` |

Help > **Open logs folder** opens the log folder. For more detail, set the environment variable `CT_LOG=trace`
before starting Camtesia. When you report a problem, attach the newest log, the crash dump if there is one, and the
text from Settings > About > **Copy diagnostics**.

---

## 10. Uninstall

### Installed version

Open Windows Settings > Apps > Installed apps > Camtesia > Uninstall (or "Add or remove programs"). The uninstaller
closes Camtesia if it is running and removes:

- the program files,
- the Start menu and desktop shortcuts,
- the `.ctproj` file association,
- the "start with Windows" entry.

It does **not** delete your data. To remove everything, also delete these folders by hand:

- `%USERPROFILE%\Videos\Camtesia\` (your recordings and exports, check before deleting)
- `%APPDATA%\Camtesia\` (settings, presets, editor layout)
- `%LOCALAPPDATA%\Camtesia\` (autosaves, logs, crash dumps)

For a silent uninstall, run `unins000.exe /VERYSILENT` from the install folder.

### Portable version

Quit Camtesia and delete its folder. Your recordings are in its `data\recordings\` subfolder, so move them first if
you want to keep them.
