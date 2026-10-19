# P09 · Editor UI (ImGui shell, panels, timeline widget, preview, properties, transport)

**Goal:** the complete editor window on top of P06-P08: dockable panels, custom-drawn timeline that stays at 60 fps
with 1,000 clips, preview with on-canvas gizmos, properties with keyframe toggles, transport with J/K/L, media
import, undo/redo, autosave and recovery, toasts and error UX. Everything styled per `docs/05`.

**Read first:** `docs/05-ui-design.md` (all), `docs/03` sections 2, 3, 6; `docs/04` section 3; P07 ops list; P08 `Compositor` API.
**Depends on:** P08 (and P05's `ui::widgets`/`Theme`/`Toast`). **Size:** XL (about 9,000 lines).

## Deliverables

```
src/render/include/ct/render/timeline_player.h  src/render/src/timeline_player.cpp        (project-aware playback: resolves FrameInputs and mixes audio; also used by export)
src/ui/include/ct/ui/{app.h,editor_window.h,editor_state.h,fonts.h,icons.h,shortcuts.h,dialogs.h,drag_drop.h,stats_overlay.h,strings.h(extended)}
src/ui/include/ct/ui/panels/{media_panel.h,preview_panel.h,properties_panel.h,timeline_widget.h,transport.h,status_bar.h,export_dialog.h(P10),settings_window.h(P11)}
src/ui/src/**/*.cpp   tests/ui/*.cpp   tests/data/projects/1000_clips.ctproj
```

## Core structures

```cpp
namespace ct::render {
class TimelinePlayer {                      // one per open project; owns MediaCaches, AudioEngine source, PlaybackClock
public:
    HRESULT Init(RenderContext&, audio::AudioDevices&);
    void SetProject(const project::Project*);                           // pointer stays valid; caches opened lazily per media id
    playback::MediaCache* Cache(const project::Id& media);
    bool Resolve(TimeUs t, FrameInputs& out, bool& all_exact);         // non-blocking frame lookup for every active video clip + transitions
    bool ResolveExact(TimeUs t, FrameInputs& out, uint32_t timeout_ms); // export path: waits for prefetch
    // audio: implements playback::AudioEngine::Source by mixing active audio-bearing clips (gain dB, fades, mute, speed via resampler, normalize gain from peaks)
    playback::PlaybackClock& Clock();  playback::AudioEngine& Audio();
    void Play(); void Pause(); void Seek(TimeUs); void SetRate(double); void StepFrames(int);
    void HintScrub(TimeUs);                                              // paused prefetch around t for all media
    HRESULT RenderAudio(TimeUs from, TimeUs to, float* interleaved_out); // offline mixing (export)
};
}

namespace ct::ui {
enum class Tool { Select, Razor, Hand };
struct Selection { std::vector<project::Id> clips; std::optional<TimeUs> camera_key; std::optional<TimeUs> marker; std::optional<std::pair<project::Id, std::string>> keyframe /* clip, prop */; TimeUs keyframe_t = 0; void Clear(); bool Empty() const; };
struct EditorState {
    project::Project project;  project::UndoStack undo;  project::Autosave autosave;  bool dirty = false;
    render::TimelinePlayer player;  render::Compositor compositor;  render::RenderContext* rc = nullptr;
    Selection selection;  Tool tool = Tool::Select;  bool snap = true;  bool ripple = false;
    double pps = 100;  TimeUs scroll_us = 0;  float track_scale = 1;     // timeline view
    struct { Microsoft::WRL::ComPtr<ID3D11Texture2D> tex; Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv; Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; SizeI size; } preview;
    bool needs_render = true;                                            // preview re-render flag (set on any model/playhead change)
    // every mutation goes through this: snapshots for undo, marks dirty, requests redraw
    template <class Fn> bool Edit(const char* label, Fn&& fn);          // undo.Push(before,label); ok = fn(project); if (!ok) restore; dirty |= ok; needs_render = true; autosave.NotifyChanged()
    void BeginDrag(const char* label);  void EndDrag();                 // undo coalescing wrappers
};
class EditorWindow {
public:
    bool Create(App&, platform::D3DDevice&, render::RenderContext&, audio::AudioDevices&);
    void OpenProject(std::string_view path);  void ImportRecording(std::string_view bundle);  bool CloseRequested();   // prompts if dirty
    void Frame();                                                        // builds the whole UI for one frame; called from the event loop when dirty
    bool WantsContinuousRedraw() const;                                  // playing || dragging || animations || pending async results
};
}
```

## 9.1 App shell
- Window title `"<project name>* - super4vid"`. Menu bar (ImGui `BeginMainMenuBar` styled flat): File (New, Open, Open Recent, Import Media, Import Recording, Save, Save As, Export, Close, Exit), Edit (Undo/Redo labels from the stack, Cut/Copy/Paste/Duplicate/Delete, Select All, Preferences), View (panels toggles, Fit timeline, Zoom in/out, Stats overlay), Timeline (Split, Ripple delete, Add marker, Add track, Snap, Ripple mode), Camera (Auto/Manual, Regenerate, Add key, Clear keys), Export (presets), Help (Shortcuts, Open logs folder, About).
- Docking: `DockSpaceOverViewport` with a default layout built once via `DockBuilder` (Media left 260, Properties right 320, Timeline bottom 300, Preview centre). Layout persisted by ImGui ini in `%APPDATA%\super4vid\editor_layout.ini`; "Reset layout" in View.
- Theme/fonts/icons: `Theme::Apply`; `fonts.h` loads the font set at the current DPI (`docs/05` section 1) and rebuilds on `WM_DPICHANGED` (`ImGui_ImplDX11_InvalidateDeviceObjects` + `CreateDeviceObjects`); `icons.h` defines `constexpr const char* ICON_PLAY = u8""` etc. (UTF-8 literals; with `IMGUI_USE_WCHAR32` the glyph ranges `0xE700-0xF800` are merged from the icon font).
- Shortcuts: `shortcuts.h` table `{ "Space", Action::TogglePlay, "Play / pause" }` parsed into `ImGuiKeyChord`; dispatched once per frame with `ImGui::IsKeyChordPressed` when `!io.WantTextInput` (except Ctrl+S/Z/Y which always work). The cheat sheet (`?`) renders the table.
- Redraw policy: `App` renders a frame when input arrived, when `EditorWindow::WantsContinuousRedraw()`, or when a worker posted `RequestRedraw()` (thumbnails/peaks/decoded exact frame). Idle = 0 redraws.
- Async UI plumbing: `Async::PostToUi` lambdas run at the start of `Frame()`.

## 9.2 Media panel
Cards grid (160x90 thumbs from `MediaCache::Thumbnail(duration/4)`), name (ellipsis), duration, badges. Import via `IFileOpenDialog` (multi-select, filters: Media files; All files) on the UI thread; probing (`MediaInfo::Load`) on the IO pool; `.ctrec` folders via the folder picker (`FOS_PICKFOLDERS`) or dropping the folder onto the window (`WM_DROPFILES` with `DragAcceptFiles`). Drag source payload `"CT_MEDIA"` = media id; the timeline accepts it (section 9.4). Right click: Reveal in Explorer, Relink (file dialog; updates `Media::path`, clears `missing`), Remove (confirm when used by clips). "Effects" tab: annotation preset cards (payload `"CT_ANNOTATION"` = type).

## 9.3 Preview panel
- Render: when `needs_render` or playing: `player.Resolve(playhead)` -> `compositor.RenderFrame(project, playhead, inputs, preview.rtv)`; `preview` texture is canvas-sized (re-created on canvas change); displayed with `ImGui::Image(srv, fitted_size)` (letterbox, 24 px margin, `fit/50/100/200 %`, Space+drag pans when zoomed). If `all_exact == false`, request another frame (the exact frame arrives shortly).
- Canvas coordinate mapping: `canvas_px = (mouse - image_origin) / image_scale`; gizmo for the selected video clip uses `compositor.LayerRectOnCanvas()` and `VideoToCanvas()`; handles 8 px squares at corners/edges + rotation handle; drag updates `Transform` through `state.Edit` inside `BeginDrag/EndDrag` ("Move clip", "Scale clip", "Rotate clip"); `Shift` constrains aspect/15 degree steps; `Alt` scales about the opposite corner. Annotations: move/resize rect, pointer handle for callouts; double-click a text annotation to edit inline (ImGui `InputTextMultiline` overlay at the rect). Camera key editing: when a camera key is selected, draw the dashed zoom window; drag pans, corner drag changes zoom.
- Overlays toggles: safe areas (title/action 90/80 %), grid thirds, show/hide cursor and annotations for inspection (these call `RenderOptions`, never change the project).
- Transport bar below the image: buttons with icons, time display (`FormatTimecode`, click to edit, Enter seeks), fit menu, loop toggle (I/O range shown on the ruler), rate indicator, mute + volume slider (`AudioEngine::SetVolume`).

## 9.4 Timeline widget (`TimelineWidget`)
Pure function of state + input; draws with one `ImDrawList`; all geometry in `docs/05` section 4.

- **Coordinates:** `x(t) = origin_x + (t - scroll_us) * pps / 1e6`; `t(x)` inverse; visible range `[scroll_us, scroll_us + width/pps*1e6]`. Zoom with `Ctrl+Wheel` keeps `t` under the mouse fixed: `scroll_us += (t_mouse - scroll_us) * (1 - old_pps/new_pps)`. `F` fits `[0, Duration]`.
- **Layout:** header column (track names, icons, mute/lock/hide toggles, drag to reorder), ruler, lanes in `tracks` order (annotations/camera lanes on top, video, audio bottom), vertical scrollbar when lanes exceed the height, horizontal scrollbar for time.
- **Culling:** per track, clips sorted by `start_us` (cached vector of indices invalidated on edits); binary search the first clip with `End() >= visible.start`; iterate while `start_us <= visible.end`. Markers/keys likewise.
- **Drawing per clip:** rounded rect fill (clip color or kind color), 1 px border (`accent` when selected, 2 px), label clipped with `PushClipRect`, duration badge if width > 120 px, filmstrip for video clips (`MediaCache::Thumbnail(media_t)` every `thumb_step` seconds chosen so thumbs are ~96 px wide; `AddImage` with the SRV; placeholder shimmer while loading), waveform for audio clips (peaks at `100/s`; per pixel column take min/max over the covered peak range; emit two triangles per column via `PrimReserve`, color `#9AE6B4` at 70 % alpha; gain and fades visualized by scaling the envelope), fades as diagonal lines, speed badge, muted/hidden alpha 0.4, transitions as a hatched overlap region with an icon.
- **Interaction state machine** (`timeline_widget.cpp`):
  ```
  Idle --mouse down on clip--> Pressed(clip, grab_offset, zone: Body|TrimIn|TrimOut)
  Pressed --move > 3 px--> Dragging(Move|TrimIn|TrimOut|Slip(Alt)|Duplicate(Ctrl)) : BeginDrag(label); per frame compute candidate -> ops::Snap (if snap && !Alt) -> apply op via state.Edit (ops reject overlaps: keep last valid)
  Dragging --mouse up--> EndDrag -> Idle
  Idle --mouse down on empty--> RubberBand(rect) --up--> select clips intersecting
  Idle --mouse down on ruler--> Scrubbing: playhead = t(x) each frame, player.HintScrub, pause playback --up--> Idle
  Idle --drag payload CT_MEDIA/CT_ANNOTATION over lane--> DropPreview(ghost rect at snapped t, compatible track highlight) --drop--> ops::AddClip (new track if none compatible)
  ```
  Clicking selects (`Ctrl` toggles, `Shift` range on the same track). `Delete` -> `ops::Delete(selection, ripple)`. `S` -> split selected clips at the playhead (or the clip under the playhead on the "focused" track). Right click: context menu per `docs/05`. Keyboard nudge: `,`/`.` move selection by one frame, `Shift` = 10 frames.
- **Playhead:** line across all lanes + triangle on the ruler; during playback, auto-scroll when the playhead leaves the visible range (page by 80 %).
- **Markers and camera keys:** diamonds (8 px) with drag/double-click behaviours from `docs/05`; generated camera keys drawn dimmer.
- **Performance:** at most one `ImDrawList` command per texture change; text via `AddText` with clipping; no `std::string` allocations per clip (use stack buffers). Target < 1 ms CPU for 1,000 clips (bench in `ct_ui_tests --bench` using a headless ImGui context with a 1600x300 widget).

## 9.5 Properties panel
Generic helpers in `widgets.h`: `bool PropFloat(const char* label, float& v, float min, float max, const char* fmt, const char* unit, KeyframeHook* kf = nullptr)` (drag float, `Alt`+click resets, `Ctrl`+click types, optional ◇ toggle calling `ops::SetKeyframe/RemoveKeyframe` at the playhead and showing filled when a key exists within half a frame), `PropColor`, `PropEnum`, `PropToggle`, `PropTime`. Sections per `docs/05` section 4; each section header is collapsible and remembers its state. Multi-selection: edits apply to all selected clips (one undo entry). Nothing selected: Canvas/Background/Project sections. Camera section: mode toggle, auto parameters with live regeneration (debounced 300 ms), key list with select/delete, "Regenerate" button.

## 9.6 Transport, status bar, toasts, errors
- J/K/L: `J` reverse (−1, −2, −4), `K` pause, `L` forward (1, 2, 4); `Shift+L/J` 0.5x; `Space` toggles at 1x; arrow keys step frames (pausing first). Loop region (I/O) honored by the player (`Clock` wraps).
- Status bar: autosave age, canvas size/fps, cache usage, warning count (click opens the log), GPU ms (from `Compositor::LastTiming`).
- Toasts (`ui::Toast`) for import/export/autosave recovery; error banners per `docs/05`; the "Recover unsaved project?" prompt at startup from `Autosave::FindRecoverable`.
- `F11` stats overlay: frame ms, GPU ms per pass, cache MB, decode queue depths, redraws/s.

## Tests
- Unit (`ct_ui_tests`, headless ImGui context with `io.DisplaySize` set and no backend): `x(t)/t(x)` round trips; ruler tick selection table; timeline drag state machine driven by synthetic `ImGuiIO` events (press on clip, move 50 px, release -> `ops::MoveClip` applied and one undo entry); snapping integration; shortcut table parses and has no duplicate chords; `Selection` behaviours.
- Bench: timeline draw with `1000_clips.ctproj` < 1 ms CPU.
- Manual checklist (record in `PROGRESS.md`): every shortcut in `docs/05` section 5; drag/drop from media and effects; gizmo transforms; inline text edit; DPI change while open; close with unsaved changes; recovery after killing the process; `io.ConfigDebugHighlightIdConflicts = true` in Debug shows no conflicts while exercising every panel.

## Acceptance (9.7)
Editor budgets from `MASTER_PLAN.md` section 8 (60 fps drag with 1,000 clips; 0 redraws idle; scrub < 50 ms cached / < 150 ms cold; 1080p60 preview with 3 layers + cursor + 2 annotations without drops). Numbers in `PROGRESS.md`.

## Pitfalls
- Every ImGui widget inside loops needs a unique ID: `PushID(clip.id)`; never use the label alone.
- Do not call `ops::*` directly from widgets; always through `state.Edit` so undo/dirty/render flags stay consistent.
- Scrubbing must never block on decoding; show the nearest cached frame and request a redraw when the exact frame lands.
- `IFileOpenDialog` requires an STA thread (the UI thread is STA) and must not be opened while an ImGui popup holds mouse capture; release capture first.
- ImGui `ImTextureID` is a pointer to the SRV; keep SRVs alive until the frame is rendered (caches must not evict mid-frame: snapshot the SRV list per frame or pin entries used this frame).
- Font atlas rebuild must happen between frames (not inside `NewFrame/Render`).
- Keep `EditorState` mutations on the UI thread only; workers post results.
