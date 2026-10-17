# P07 · Project model (types, JSON, edit operations, undo, autosave, import, auto-zoom)

**Goal:** a pure, Windows-free (only `ct_core` + `ct_events`) model of a project with validated edit operations,
byte-stable JSON serialization, snapshot undo/redo, autosave with crash recovery, import of `.ctrec` bundles,
and the deterministic Auto-Zoom / Auto-Follow camera generator. Everything here is unit-testable without a GPU.

**Read first:** `docs/04-file-formats.md` sections 1-3, 6; `docs/03` section 1. 
**Depends on:** P01, and the `ct_events` module (`.ctev` reader/writer, created in P05; if P07 runs before P05, create `src/events/` now from `docs/04` section 2 and P05 will reuse it). **Size:** M (about 3,500 lines incl. tests).

## Deliverables

```
src/events/include/ct/events/{ctev.h,event_log.h}  src/events/src/{ctev.cpp,event_log.cpp}          (shared with recorder)
src/project/include/ct/project/{types.h,project.h,serialize.h,edit_ops.h,undo.h,autosave.h,importer.h,auto_zoom.h,camera.h,easing.h}
src/project/src/*.cpp   tests/project/*.cpp   tests/data/projects/*.ctproj   tests/data/autozoom/*.json
```

## Types (`types.h`; names are the contract with P08/P09)

```cpp
namespace ct::project {
using Id = std::string;                                   // "m1", "t3", "c42": prefix + base-36 counter stored in Project::next_id
enum class Ease { Linear, In, Out, InOut, Hold };
float ApplyEase(Ease e, float u);                         // Linear u; In u^3; Out 1-(1-u)^3; InOut u<.5 ? 4u^3 : 1-(-2u+2)^3/2; Hold 0
template <class T> struct Keyframe { TimeUs t_us; T v; Ease ease = Ease::Linear; };
template <class T> struct KeyTrack {
    std::vector<Keyframe<T>> keys;                        // sorted by t_us, unique times
    bool Empty() const;  T At(TimeUs t, const T& fallback) const;   // before first = first.v; after last = last.v; between = lerp(ApplyEase(prev.ease,u))
    void Set(TimeUs t, T v, Ease e);  bool Remove(TimeUs t);  const Keyframe<T>* Nearest(TimeUs t, TimeUs tolerance) const;
};
struct Transform { float x = 0, y = 0, scale = 1, rotation_deg = 0, opacity = 1; };          // x,y normalized canvas offsets
struct Crop { int left = 0, top = 0, right = 0, bottom = 0; };                               // source pixels
struct Shadow { bool enabled = true; float blur_px = 48, offset_y_px = 24, opacity = 0.45f; };
struct Background { enum class Kind { Solid, Gradient, Image } kind = Kind::Gradient; ColorF color_a{0.106f,0.153f,0.208f,1}, color_b{0.035f,0.039f,0.059f,1}; float angle_deg = 135; std::string image; float blur = 0; };
struct Canvas { int width = 1920, height = 1080, fps_num = 60, fps_den = 1; Background background; int padding_px = 96; int corner_radius_px = 18; Shadow shadow; };
struct AudioProps { float gain_db = 0; bool muted = false; TimeUs fade_in_us = 0, fade_out_us = 0; bool normalize = false; std::optional<float> noise_gate_db; };
struct CursorStyle { bool visible = true; float scale = 1.5f; float smoothing = 0.35f; enum class Click { None, Ripple, Highlight, Both } click = Click::Ripple; ColorF click_color{0.31f,0.55f,1,1}; TimeUs hide_when_idle_us = 3'000'000; bool spotlight = false; bool shadow = true; };
struct KeystrokeStyle { bool enabled = true; enum class Pos { BottomCenter, BottomLeft, BottomRight, TopCenter } pos = Pos::BottomCenter; enum class Style { Pill, Keycap } style = Style::Pill; int hold_ms = 1200; };
struct Pip { enum class Corner { TL, TR, BL, BR } corner = Corner::BR; float size = 0.22f; int margin_px = 32; enum class Mask { None, Rounded, Circle } mask = Mask::Rounded; int radius_px = 24; bool shadow = true; bool mirror = false; };
struct Annotation {
    enum class Type { Text, Callout, Arrow, Rect, Ellipse, Highlight, Blur, Keystrokes } type = Type::Text;
    std::string text; std::string font = "Segoe UI"; int size_px = 48; int weight = 600; ColorF color{1,1,1,1}, fill{0.31f,0.55f,1,1}, stroke{0,0,0,1}; int stroke_px = 0; int radius_px = 12;
    RectF rect{0.4f, 0.4f, 0.2f, 0.1f};  PointF pointer{0.5f, 0.6f};  float arrow_head_px = 24;  int blur_px = 24;  bool pixelate = false;
    enum class Anim { None, Fade, Pop, Slide } anim_in = Anim::Pop, anim_out = Anim::Fade; int anim_ms = 220;
};
struct Media { Id id; enum class Kind { Recording, Video, Audio, Image } kind; std::string path; TimeUs duration_us = 0; int width = 0, height = 0, fps_num = 0, fps_den = 1; std::vector<std::string> audio_streams; bool has_events = false, has_camera = false; bool missing = false; };
struct Track { Id id; enum class Kind { Video, Audio, Annotations } kind; std::string name; bool muted = false, locked = false, hidden = false; int height_px = 0; };
struct Clip {
    Id id, track, media;  enum class Source { Screen, Camera, Mic, System, Default } source = Source::Default;
    TimeUs start_us = 0, in_us = 0, out_us = 0;  double speed = 1.0;  std::string name;  ColorF color{0,0,0,0};
    Transform transform; Crop crop; std::optional<int> corner_radius_px; std::optional<Shadow> shadow; AudioProps audio;
    std::map<std::string, KeyTrack<float>> keyframes;      // keys: "transform.x" "transform.y" "transform.scale" "transform.rotation_deg" "transform.opacity" "audio.gain_db"
    CursorStyle cursor; KeystrokeStyle keystrokes; std::optional<Pip> pip; std::optional<Annotation> annotation;
    TimeUs Duration() const { return TimeUs((out_us - in_us) / speed); }  TimeUs End() const { return start_us + Duration(); }
    TimeUs MediaTimeAt(TimeUs timeline_t) const { return in_us + TimeUs((timeline_t - start_us) * speed); }
    float Prop(const char* key, TimeUs t, float static_value) const;   // keyframed value or static
};
struct Transition { Id track, a, b; enum class Kind { Crossfade, Dip, Slide } kind = Kind::Crossfade; TimeUs duration_us = 500'000; ColorF color{0,0,0,1}; enum class Dir { Left, Right, Up, Down } dir = Dir::Left; };
struct CameraKey { TimeUs t_us; float zoom = 1, cx = 0.5f, cy = 0.5f; Ease ease = Ease::InOut; bool generated = false; };
struct AutoZoomParams { float zoom = 2.0f; TimeUs min_hold_us = 1'200'000, lead_us = 150'000; int ease_ms = 650; bool follow = true; float follow_deadzone = 0.15f; bool on_click = true, on_typing = true; };
struct Camera { enum class Mode { Manual, Auto } mode = Mode::Auto; std::vector<CameraKey> keys; AutoZoomParams auto_params; struct View { float zoom, cx, cy; }; View At(TimeUs t) const; /* zoom interpolated in log2 space, centre clamped so the window stays inside [0,1] */ };
struct Marker { TimeUs t_us; std::string label; ColorF color{1,0.69f,0.125f,1}; };
struct UiState { TimeUs playhead_us = 0; double zoom = 1; TimeUs scroll_us = 0; std::vector<Id> selected; std::optional<std::pair<TimeUs,TimeUs>> loop; };
struct Project {
    int version = 1; Id id; std::string name; std::string path /* not serialized */; uint64_t next_id = 1;
    Canvas canvas; std::vector<Media> media; std::vector<Track> tracks; std::vector<Clip> clips; std::vector<Transition> transitions; Camera camera; std::vector<Marker> markers; UiState ui; Json extra /* unknown keys preserved */;
    bool Validate(std::string* why) const;            // docs/04 section 3 invariants
    TimeUs Duration() const;  Id NewId(const char* prefix);
    Media* FindMedia(const Id&);  Track* FindTrack(const Id&);  Clip* FindClip(const Id&);  (const overloads)
    std::vector<Clip*> ClipsOnTrack(const Id&);       // sorted by start_us
    std::vector<const Clip*> ClipsAt(TimeUs t) const; // active clips, bottom track first
    const Clip* PrimaryScreenClip() const;            // first Source::Screen clip (drives ScreenFit in the compositor)
};
}
```

## 7.2 Serialization (`serialize.h`)
`std::string ToJson(const Project&)` / `bool FromJson(std::string_view, Project&, Err*)` / `bool Save(const Project&, std::string_view path, Err*)` (atomic write, stores media paths relative when on the same volume) / `bool Load(std::string_view path, Project&, Err*)` (resolves relative paths against the project directory, marks `missing`) / `bool Migrate(Json&, int from, Err*)`. Key order and formatting exactly as `docs/04` section 3 so that `Save(Load(x)) == x` byte for byte for files written by this version. Floats are written with `%.6g` trimmed; times as integers; colors as `#RRGGBB` or `#RRGGBBAA`; enums as the lowercase strings in `docs/04`. Unknown keys at any object level are kept in `extra` maps and re-emitted after known keys.

## 7.3 Edit operations (`edit_ops.h`) — every op validates, mutates, and returns `bool` with `Err`
```cpp
namespace ct::project::ops {
bool AddMedia(Project&, Media, Id* out);                 bool RemoveMedia(Project&, const Id&, bool remove_clips, Err*);
bool AddTrack(Project&, Track::Kind, std::string name, Id* out, int at_index = -1);  bool RemoveTrack(Project&, const Id&, Err*);  bool MoveTrack(Project&, const Id&, int new_index);
bool AddClip(Project&, Clip, Id* out, Err*);             // rejects overlap on the track
bool MoveClip(Project&, const Id&, TimeUs new_start, const Id& new_track, Err*);   // rejects overlap and incompatible track kinds
bool TrimIn(Project&, const Id&, TimeUs new_start_us, Err*);    // keeps End() fixed; respects in_us >= 0
bool TrimOut(Project&, const Id&, TimeUs new_end_us, Err*);     // respects out_us <= media duration (images: unbounded)
bool Slip(Project&, const Id&, TimeUs delta_us, Err*);          // shifts in/out together
bool Split(Project&, const Id&, TimeUs at_us, Id* left, Id* right, Err*);   // keyframes are split and re-based; transitions touching the split point are removed
bool Delete(Project&, const std::vector<Id>&, bool ripple, Err*);            // ripple: later clips on the same tracks shift left by the removed span
bool Duplicate(Project&, const Id&, Id* out, Err*);                          // placed right after the source if free, else fails
bool SetSpeed(Project&, const Id&, double speed, Err*);                      // keeps start_us; adjusts Duration; fails on overlap
bool SetTransform(Project&, const Id&, const Transform&);  bool SetCrop(...);  bool SetAudio(...);  bool SetCursorStyle(...);  bool SetKeystrokeStyle(...);  bool SetPip(...);  bool SetAnnotation(...);
bool SetKeyframe(Project&, const Id& clip, const std::string& prop, TimeUs t, float v, Ease);  bool RemoveKeyframe(Project&, const Id& clip, const std::string& prop, TimeUs t);
bool AddTransition(Project&, Transition, Err*);           // a and b adjacent on the same track (|a.End() - b.start| <= 1 frame), duration <= min(a,b)/2
bool RemoveTransition(Project&, const Id& a, const Id& b);
bool SetCameraKey(Project&, CameraKey);  bool RemoveCameraKey(Project&, TimeUs t);  bool SetCameraMode(Project&, Camera::Mode);
bool AddMarker(Project&, Marker);  bool RemoveMarker(Project&, TimeUs t);  bool MoveMarker(Project&, TimeUs from, TimeUs to);
bool SetCanvas(Project&, const Canvas&, Err*);
TimeUs Snap(const Project&, TimeUs t, TimeUs tolerance_us, const std::vector<Id>& ignore);   // nearest of: clip edges, markers, playhead(passed in ignore list via special id "playhead:<t>"), 0
}
```

## 7.4 Undo, autosave, recovery
```cpp
class UndoStack {
public:
    void Push(const Project& before, const char* label);  bool Undo(Project& current);  bool Redo(Project& current);
    bool CanUndo() const; bool CanRedo() const; const char* UndoLabel() const; const char* RedoLabel() const;
    void BeginCoalesce(const char* key);  void EndCoalesce();        // a drag = one undo entry: only the first Push during a coalesce group is recorded
    size_t Entries() const; size_t Bytes() const;  void Clear();
private: /* std::deque<Entry{std::string json; std::string label;}> , cap 200 entries / 64 MB */
};
class Autosave {
public:
    void Init(const Id& project_id, int interval_s);  void NotifyChanged();  void Tick(const Project&);   // UI timer 1 Hz; saves when dirty and interval elapsed, or every 20 edits
    void Clear();   // on explicit save / close
    struct Recoverable { std::string autosave_path; std::string original_path; std::string name; int64_t mtime; };
    static std::vector<Recoverable> FindRecoverable();   // scans autosave dir; lock files with dead PIDs
};
```

## 7.5 Import (`importer.h`)
`bool ImportRecording(Project&, std::string_view bundle_path, Err*, ImportOptions{ bool apply_default_style = true; bool auto_zoom = true; })`:
1. Parse `meta.json`; add `Media{Recording}` with duration, size, fps, `audio_streams` from `audio` (`["mic","system"]` in stream order), `has_events`, `has_camera`.
2. Ensure tracks exist (create in this order if missing): `Screen` (Video), `Camera` (Video, only if `has_camera`), `Mic` (Audio, if mic), `System` (Audio, if system), `Callouts` (Annotations).
3. Clips at the end of the current timeline (or 0 for an empty project): screen clip `Source::Screen` with default `CursorStyle`/`KeystrokeStyle`; camera clip `Source::Camera` with default `Pip`; audio clips `Source::Mic` / `Source::System` (media stream chosen by `audio_streams` index).
4. If the project is new (no clips before): canvas = recording size and fps; with `apply_default_style` the "polished" defaults (padding 96, radius 18, gradient background, shadow); else leave the canvas.
5. If `auto_zoom` and `has_events`: load the `events::EventLog`, run `AutoZoom::Generate`, set `camera.mode = Auto`, append keys offset by the clip start.

## 7.6 Auto-Zoom and Auto-Follow (`auto_zoom.h`) — deterministic, pure
```cpp
struct InterestEvent { TimeUs t_us; float x, y; /* normalized to the screen video frame */ enum class Kind { Click, Typing } kind; };
std::vector<InterestEvent> ExtractInterest(const events::EventLog&, SizeI video_size, const AutoZoomParams&);   // mouse downs (left/right) + typing burst starts (position = cursor at that time)
std::vector<CameraKey> Generate(const std::vector<InterestEvent>&, TimeUs duration_us, const AutoZoomParams&, const events::EventLog* for_follow);
```
Algorithm:
1. Sort events. Walk them into **segments**: start a segment at event `e` with `t_in = e.t - lead_us`, `centre = p(e)`; absorb following events while `next.t - last.t <= 1.5 * min_hold_us` and `|p(next) - centre| <= 0.25`, updating `centre` as the running mean; `t_out = last.t + min_hold_us`.
2. Merge consecutive segments with gap `< 2 * ease_ms`; drop segments shorter than `min_hold_us`; clamp to `[0, duration]`.
3. For each segment emit keys: `(t_in - ease_ms, zoom 1, centre_prev_or_0.5, InOut)`, `(t_in, Z, c, Hold)`, `(t_out, Z, c, InOut)`, `(t_out + ease_ms, 1, 0.5/0.5, Hold)`; centre clamped to `[1/(2Z), 1 - 1/(2Z)]`. Adjacent segments share the transition (no zoom-out between them if the gap is < `2 * ease_ms`; instead pan from c1 to c2 over `ease_ms`).
4. Follow (`params.follow`): inside a segment, sample the cursor every 100 ms; when `|cursor - centre| > follow_deadzone * (1/Z)` emit a key moving `centre` toward the cursor by the overshoot amount with `Ease::Out`, at most one key per 400 ms, keeping the clamp.
5. All generated keys have `generated = true`. The UI converts a key to manual when the user edits it; `Regenerate` replaces only generated keys.

`Camera::At(t)`: find the surrounding keys; zoom interpolated in `log2` space; centre linearly; easing from the previous key; `Hold` keeps the previous value until the next key. Must be continuous in time (test asserts no jump > 0.02 zoom or 0.01 centre per ms outside key times).

## Tests
- Serialization: every sample in `docs/04` round-trips byte-identically; unknown keys preserved; migration from `version 0` fixture; relative/absolute paths; missing media flagged, project still loads.
- Ops: each op's happy path and each documented failure; `Split` conserves total duration and keyframe values at the split; `Delete(ripple)` shifts exactly; `SetSpeed` duration math; `Snap` picks the nearest candidate within tolerance.
- Fuzz: 10,000 random ops on a seeded project; after each op `Validate()` is true; undo of each op restores the previous JSON exactly.
- Undo: coalescing yields one entry per drag; cap enforcement.
- Importer: `tests/data/sample.ctrec` -> tracks/clips as specified; canvas from recording.
- Auto-zoom: fixtures `tests/data/autozoom/*.json` (`{events:[...], params:{...}, expected_keys:[...]}`) written once from the reference implementation and reviewed by eye in the editor; `Camera::At` continuity property test; a 1-click fixture produces exactly 4 keys.
- Easing: endpoints 0/1, monotonic.

## Acceptance (7.7)
All `ct_project_tests` pass in Debug/Release with `--repeat 3`; fuzz 10,000 iterations in < 10 s Release; JSON round-trip byte-identical for all fixtures.

## Pitfalls
- `std::map<std::string, KeyTrack<float>>` keys must match the strings used by the UI and compositor exactly; keep the list in `types.h` as `constexpr const char* kKeyframeProps[]`.
- Floats in JSON: `%.6g` can turn `0.1` into `0.1` fine but `1e-07` into exponent form; the parser must accept both; the writer must be stable (write, parse, write -> identical).
- `Clip::Duration()` with speed uses truncation; `Split` must compute the right part's `in_us` from `MediaTimeAt(at)` so the sum of durations never exceeds the original by more than 1 µs.
- Never store pointers into `std::vector<Clip>` across edits; use ids.
- The model must not know about ImGui, D3D, or Media Foundation. Thumbnails, peaks and decoding live in `playback`.
