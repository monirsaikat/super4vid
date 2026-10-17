# 05 · UI design specification

Target: calmer and more modern than Camtasia, closer to Screen Studio / Linear / modern dark tools. Everything is
rendered by Dear ImGui (docking branch) on D3D11 with custom styling and custom-drawn widgets (`ImDrawList`).
No ImGui default look may remain visible in the shipped product.

## 1. Design tokens (`src/ui/include/ct/ui/theme.h`)

### Colors (dark theme, the only theme in v1.0)

| Token | Hex | Use |
|---|---|---|
| `bg0` | `#0F1115` | window background |
| `bg1` | `#161920` | panels, timeline background |
| `bg2` | `#1E222B` | cards, inputs, track lanes (alternate rows `#1B1F27`) |
| `bg3` | `#262B36` | hover |
| `bg4` | `#2E3441` | pressed / active |
| `border` | `#2C323E` | 1 px borders, separators |
| `text` | `#E6E8EE` | primary text |
| `text_dim` | `#9AA3B2` | secondary text, labels |
| `text_faint` | `#5F6878` | disabled, ruler ticks |
| `accent` | `#4F8CFF` | selection, primary buttons, playhead handle |
| `accent_hover` | `#6CA0FF` | |
| `accent_soft` | `#4F8CFF` at 18 % alpha | selection fill, focus ring |
| `record` | `#FF4D4F` | record button, recording indicator, playhead line |
| `success` | `#3DDC84` | meters (low), checkmarks |
| `warn` | `#FFB020` | meters (high), markers |
| `danger` | `#FF4D4F` | errors, destructive |
| clip `video` | `#2B6CB0` fill, `#63B3ED` text | screen/video clips |
| clip `camera` | `#6B46C1` fill, `#B794F4` text | webcam clips |
| clip `audio` | `#276749` fill, `#68D391` text | audio clips, waveform `#9AE6B4` |
| clip `annotation` | `#975A16` fill, `#F6E05E` text | callouts/text |
| clip `camera_kf` | `#4F8CFF` diamonds | zoom keyframes lane |

### Shape, spacing, type

- Radii: controls 6 px, cards/panels 10 px, preview canvas 14 px, clips 6 px, pills 999 px.
- Spacing scale: 4 / 8 / 12 / 16 / 24 / 32. Panel padding 12. Control height 28 (compact) or 32 (default).
- Fonts (system, never bundled): "Segoe UI Variable Text" 14 px body, 12 px labels, "Segoe UI Variable Display" 20 px semibold titles; fallback "Segoe UI". Mono: "Cascadia Mono" 12 px, fallback "Consolas". Load through `ImFontAtlas::AddFontFromFileTTF` from `C:\Windows\Fonts\` (`SegUIVar.ttf`, `segoeui.ttf`, `CascadiaMono.ttf`, `consola.ttf`), choosing the first that exists. Rebuild the atlas on DPI change at `base_px * dpi / 96`.
- Icons: glyphs from "Segoe Fluent Icons" (`SegoeIcons.ttf`, Windows 11) falling back to "Segoe MDL2 Assets" (`segmdl2.ttf`, Windows 10), merged into the atlas with `MergeMode`. Common codepoints (identical in both fonts; verify any new one in Character Map before use):
  `Play E768 · Pause E769 · Stop E71A · Record E7C8 · Settings E713 · Microphone E720 · Camera E722 · Volume E767 · Mute E74F · Cut E8C6 · Undo E7A7 · Redo E7A6 · Save E74E · FolderOpen E838 · OpenFile E8E5 · FullScreen E740 · Close E8BB · Minimize E921 · Maximize E922 · ChevronDown E70D · ChevronRight E76C · Add E710 · Delete E74D · Monitor E7F4 · ZoomIn E8A3 · ZoomOut E71F · Previous E892 · Next E893 · Flag E7C1 · More E712 · Video E714 · Picture E8B9 · Font E8D2 · Edit E70F · Copy E8C8 · Paste E77F · Info E946 · Warning E7BA · Error EA39 · Check E73E · Cancel E711 · Search E721 · Lock E72E · Unlock E785 · Hide ED1A · View E890 · Export EDE1`
- Motion: panels, popovers, toasts fade/slide 160 ms ease-out; record button pulses (1.2 s loop) while recording; no animation on timeline interactions (they must feel instant); playhead updates every rendered frame during playback.
- Focus ring: 2 px `accent` outside the control, radius +2. Every interactive element has a hover state.

### ImGui style mapping (`Theme::Apply(ImGuiStyle&)`)

```
WindowRounding 10  ChildRounding 10  FrameRounding 6  PopupRounding 8  GrabRounding 6  TabRounding 6  ScrollbarRounding 8
WindowPadding (12,12)  FramePadding (10,6)  ItemSpacing (8,6)  ItemInnerSpacing (6,4)  CellPadding (8,4)
IndentSpacing 16  ScrollbarSize 10  GrabMinSize 10  WindowBorderSize 1  FrameBorderSize 0  PopupBorderSize 1  TabBarBorderSize 0
Colors: WindowBg=bg1  ChildBg=bg1  PopupBg=bg2  Border=border  FrameBg=bg2  FrameBgHovered=bg3  FrameBgActive=bg4
        TitleBg/TitleBgActive=bg0  MenuBarBg=bg0  ScrollbarBg=transparent  ScrollbarGrab=bg3  ScrollbarGrabHovered=bg4
        CheckMark=accent  SliderGrab=accent  SliderGrabActive=accent_hover  Button=bg2  ButtonHovered=bg3  ButtonActive=bg4
        Header=accent_soft  HeaderHovered=bg3  HeaderActive=bg4  Separator=border  Tab=bg1  TabHovered=bg3  TabSelected=bg2
        DockingPreview=accent_soft  DockingEmptyBg=bg0  Text=text  TextDisabled=text_faint  NavCursor=accent
```

## 2. Recorder bar (default window when the app starts)

A floating, always-on-top, frameless pill at the bottom-center of the primary monitor (position remembered).
`WS_POPUP | WS_VISIBLE`, `WS_EX_TOOLWINDOW | WS_EX_TOPMOST`, rounded via `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND)`,
dark title bar attribute, and **`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`** so it never appears in recordings.

```
Idle (560 x 64 dip):
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ ⠿ │ [▭ Monitor 1 ▾] [⬚ Region] [▣ Window] │ 60 fps ▾ │ 🎤 ▁▃▅ │ 🔊 ▁▂ │ 📷 off │  ● REC  │ ⚙ │ ✕ │
└──────────────────────────────────────────────────────────────────────────────────────┘
   drag   source segmented control             fps    mic+meter  sys+meter  cam     record   settings close

Recording (260 x 44 dip, slides to the same anchor):
┌──────────────────────────────────────────┐
│ ● 00:12:34 │ ⏸ │ ■ │ ⚑ │ 🎤 ▁▃▅ │ ⠿ │
└──────────────────────────────────────────┘
  pulsing dot + timer  pause stop marker  meter  drag
```

Behaviour:
- Source control: "Monitor N" opens a popover listing monitors with thumbnails (captured once via duplication); "Region" opens the region overlay (section 3); "Window" opens the overlay in window-snap mode.
- fps popover: 15 / 24 / 30 / 60 / native; quality: Good / High / Best (= quality 60 / 70 / 85); codec H.264 / HEVC (grayed if no HW HEVC).
- Mic / system toggles: click toggles, chevron opens device list. Meter is a 3-segment bar updated at 30 Hz from `Meters` atomics (green < -18 dBFS, amber < -6, red above).
- Camera toggle: chevron lists devices + resolution; when on, a 160x90 live preview popover appears on hover.
- REC: starts the countdown (3-2-1 overlay in the capture region, excluded from capture, 1 s steps, Esc cancels). Hotkey F9 does the same from anywhere.
- Settings gear: opens the settings window (P11). Close hides to tray (tray menu: Record / Pause / Stop / Open editor / Settings / Quit; Pause and Stop are enabled only while recording).
- During recording the bar can be hidden entirely (setting), the tray icon turns red, hotkeys still work.
- After stop: a toast "Saved · 2026-10-17 12-03-44.ctrec" with buttons **Open in editor** · **Show in folder** · **Delete**. Default action after 8 s: none.

## 3. Region selection overlay

Full virtual-screen layered window (`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW`; not `WS_EX_NOACTIVATE`, it needs keyboard focus), drawn with Direct2D through `platform::LayeredWindow` (an `ID2D1DCRenderTarget` over a 32-bit premultiplied DIB pushed with `UpdateLayeredWindow`; redraw only dirty rectangles so 4K stays cheap).

- Dim `rgba(0,0,0,0.45)` everywhere except the selection. Crosshair lines through the cursor before dragging, size label `1920 × 1080` next to the cursor (even numbers enforced; odd drags round down).
- Hover before drag highlights the window under the cursor (`DWMWA_EXTENDED_FRAME_BOUNDS`, skipping cloaked/invisible/tool windows and our own); click without drag selects that window's rect.
- After the first drag: 8 resize handles (10 px squares), move by dragging inside, arrow keys nudge 1 px (Shift 10 px), Enter/Space confirms, Esc cancels, R re-starts, M cycles monitor presets (full monitor 1..N).
- Confirm shows a thin `record`-colored 2 px frame around the region for the whole recording (separate click-through layered window, `WS_EX_TRANSPARENT`, excluded from capture).

## 4. Editor window

Dockable layout (ImGui docking, no multi-viewports), default arrangement and sizes at 1600x900:

```
┌ Menu: File Edit View Timeline Camera Export Help ───────────────────────── ⏺ Record │ Export ▾ ┐
├──────────────┬──────────────────────────────────────────────────┬──────────────────────────────┤
│ Media  (260) │ Preview                                          │ Properties (320)             │
│ ┌──────────┐ │   ┌────────────────────────────────────────┐     │ ▸ Clip   Screen 00:00-03:05  │
│ │thumb  rec│ │   │        canvas, letterboxed, 14px radius │     │   Transform  x y scale rot   │
│ │thumb  mp4│ │   │        on-canvas gizmo for selection   │     │   Crop                       │
│ └──────────┘ │   └────────────────────────────────────────┘     │ ▸ Cursor  size smooth ripple │
│ + Import     │  ⏮ ◀ ▶ ⏭   00:01:12.03 / 00:03:05.00   fit ▾ 🔁  │ ▸ Camera  mode auto/manual   │
│ Effects tab  │                                                  │ ▸ Background padding radius  │
├──────────────┴──────────────────────────────────────────────────┴──────────────────────────────┤
│ Timeline (height 300, resizable)                                                               │
│ [+Track] [Snap] [Ripple] | ✂ Split  ⌫ Delete  ⚑ Marker            zoom ─────●───── [fit]       │
│ ruler  0:00      0:10      0:20      0:30      0:40 … (ticks adapt to zoom)                    │
│ ▣ Camera KF │  ◆        ◆    ◆                    ◆                                            │
│ ▦ Callouts  │      [Click here ]        [Nice!]                                                │
│ ▣ Camera    │ [███████ webcam ████████████████████████████████████]                            │
│ ▣ Screen    │ [▓▓▓ filmstrip ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓]                           │
│ ♪ Mic       │ [~~~~~~~~ waveform ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]                            │
│ ♪ System    │ [~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~]                            │
├────────────────────────────────────────────────────────────────────────────────────────────────┤
│ status: Autosaved 12 s ago · 60 fps · 1920×1080 · cache 180/256 MB            ⚠ 0  ⓘ GPU 3.2 ms │
└────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Timeline widget (`TimelineWidget`, fully custom-drawn, one `ImDrawList`)

- Geometry: header column 160 px; ruler 24 px; track heights: video 72, camera 56, audio 56, annotation 40, camera-keyframe lane 28; gap 2 px; clips inset 1 px, radius 6.
- Zoom: pixels-per-second from 2 to 2000, `Ctrl+Wheel` zooms around the mouse, `Wheel` scrolls horizontally, `Shift+Wheel` scrolls vertically, `Alt+Wheel` changes track heights, `F` fits the project. Ruler ticks pick the first unit from {1 frame, 100 ms, 500 ms, 1 s, 5 s, 10 s, 30 s, 1 min, 5 min} that yields >= 80 px spacing; minor ticks at 1/5.
- Clips: label (clip name or media name, truncated with ellipsis), duration badge when wide enough. Video clips draw a filmstrip (thumbnails 160x90 every N seconds from `MediaCache::Thumbnail`, N chosen so thumbs are ~96 px wide at the current zoom, loaded lazily on the IO pool). Audio clips draw min/max peaks (`Peaks` precomputed at 100/s per channel, averaged to mono, filled polygon). Fades draw as diagonal lines at the clip ends. Speed != 1 shows `1.5×` badge. Muted/hidden clips 40 % opacity.
- Interaction: click selects; `Ctrl` toggles; drag on empty space = rubber band; drag body moves (snaps to clip edges, playhead, markers, 0 within 8 px; `Alt` disables snap); dragging across tracks moves to a compatible track; 8 px trim handles at both ends (cursor changes); `Alt`+drag body slips media; `Ctrl`+drag duplicates; `S` splits all selected (or the clip under the playhead); `Delete` removes (ripple when ripple mode is on); double-click opens properties; right-click context menu (Split, Delete, Ripple delete, Duplicate, Mute, Speed…, Reset transform, Add transition…, Color…).
- Playhead: 1 px `record` line with a 12 px triangle handle on the ruler; drag anywhere on the ruler scrubs; while scrubbing, the preview shows the nearest cached frame immediately and the exact frame when decode completes (< 150 ms).
- Markers: diamonds on the ruler, drag to move, double-click to rename, `M` adds at playhead.
- Camera keyframe lane: diamonds at each keyframe, drag to move in time, click selects (properties show zoom/cx/cy/ease), double-click on empty adds. Auto mode shows generated keyframes in a dimmer color; editing one converts it to manual.
- Performance: the widget culls to the visible range using binary search on sorted clip start times per track; drawing 1,000 clips must stay under 1 ms CPU.

### Preview panel

- Canvas letterboxed in the available area with 24 px margin; `fit`, `50 %`, `100 %`, `200 %`; `Space`+drag pans when zoomed; the rendered texture is the compositor's output (no re-scaling in the UI except the final blit).
- Selected video clip shows a gizmo: 8 handles (scale, `Shift` keeps aspect, default keeps aspect for corners), rotation handle 24 px above, drag body moves; numbers appear while dragging. Annotations: move/resize same way plus a pointer-tip handle for callouts. Camera keyframe editing: a dashed rectangle showing the visible area, drag to pan, corner drag to zoom.
- Transport bar: `⏮` to start, `◀` previous frame, `▶/⏸`, `▶` next frame, `⏭` to end, time display `HH:MM:SS.FF` (frames) clickable to type a time, fit menu, loop toggle, speed indicator when J/K/L is active, safe-area toggle, mute toggle + volume.

### Properties panel

Sections collapse independently; show only what applies to the selection; multi-selection edits common fields.
Every numeric field is an ImGui drag-float with the unit in the label (`x 0.00 · y 0.00 · scale 100 % · rot 0°`),
`Alt`+click resets, and a small ◇ keyframe toggle on keyframeable fields (filled when a keyframe exists at the playhead).

Sections: Clip (name, time, speed, color), Transform, Crop, Corner radius + Shadow, Audio (gain, mute, fades, normalize, noise gate), Cursor, Keystrokes, Camera (mode, auto parameters, "Regenerate", list of keyframes), PIP (for camera clips), Annotation (type-specific), Transition, Background/Canvas (when nothing is selected), Project (name, canvas size presets 16:9 1080p/4K, 9:16, 1:1, 4:5, fps).

### Media panel

Grid of cards (thumbnail 160x90, name, duration, badges `REC` `CAM` `EVT`), `Import…` button (`IFileOpenDialog`, multi-select), drag a card onto the timeline (drop preview ghost), right-click: Reveal, Remove (prompts if used), Relink.
"Effects" tab lists annotation presets (Text, Callout, Arrow, Rectangle, Ellipse, Highlight, Blur, Keystrokes) as draggable cards.

### Dialogs and feedback

- Export dialog (P10): preset list on the left, settings on the right, estimated size, destination, `Export` button; progress as a modal with frame/time/ETA and `Cancel`; completion toast with **Open**, **Show in folder**.
- Toasts bottom-right, stack, 6 s, hover pauses.
- Errors: a non-modal red banner at the top of the panel concerned, with the plain-language message and an **Details** disclosure showing the `HRESULT` string and log path.
- Unsaved changes: standard Yes/No/Cancel modal on close.

## 5. Keyboard shortcuts (global in the editor; recorder hotkeys are in `MASTER_PLAN.md` section 9)

| Keys | Action |
|---|---|
| `Space` | Play / pause |
| `J` / `K` / `L` | Reverse / stop / forward; repeated `L` 2x, 4x; repeated `J` same backwards |
| `Shift+J` / `Shift+L` | Reverse / forward at 0.5x |
| `,` / `.` | Nudge selected clips one frame left / right (`Shift` = 10 frames) |
| `←` / `→` | Previous / next frame (`Shift` = 1 s) |
| `Home` / `End` | Start / end |
| `I` / `O` | Set loop in / out; `Alt+I`/`Alt+O` clear |
| `S` | Split at playhead |
| `Delete` / `Backspace` | Delete selection (ripple if enabled); `Shift+Delete` ripple delete always |
| `Ctrl+Z` / `Ctrl+Shift+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+X/C/V/D` | Cut / copy / paste (at playhead) / duplicate |
| `Ctrl+A` | Select all on the timeline |
| `M` | Add marker |
| `Z` / `Shift+Z` | Add camera keyframe at playhead / remove nearest |
| `+` / `-` / `F` | Zoom in / out / fit timeline |
| `Ctrl+S` / `Ctrl+Shift+S` / `Ctrl+O` / `Ctrl+N` | Save / save as / open / new |
| `Ctrl+I` | Import media |
| `Ctrl+E` | Export |
| `Ctrl+R` | Open recorder bar |
| `Ctrl+Shift+R` | Toggle ripple mode |
| `N` | Toggle snapping |
| `Ctrl`+click on a number field | Type a value (`Alt`+click resets it) |
| `Ctrl+1..4` | Focus Media / Preview / Properties / Timeline |
| `F11` | Toggle stats overlay (frame time, GPU time, cache, queue depths) |
| `?` / `F1` | Shortcut cheat sheet |

## 6. HiDPI and multi-monitor

- Per-monitor V2 awareness is declared in the manifest; `WM_DPICHANGED` resizes the window with the suggested rect and rebuilds fonts at the new scale. All layout constants are in dip and multiplied by `dpi_scale` at draw time.
- The swapchain is resized on `WM_SIZE` (skip when minimized). Rendering pauses while minimized.
- The recorder bar remembers its position per monitor configuration hash; if its monitor disappears it re-centers on the primary.

## 7. Accessibility baseline (v1.0)

- Every control has a tooltip with its name and shortcut. Keyboard navigation works for the transport, timeline selection (Tab cycles clips), and all dialogs. Minimum text contrast 4.5:1 against its background for all tokens above (checked). No information conveyed by color alone (clip kinds also have icons in the header).
