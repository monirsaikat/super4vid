// EditorWindow: the editor document on top of EditorShell (file ops, preview rendering, media, autosave, prompts).
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/editor_window.h>

#include "cache_media_provider.h"
#include "editor_player.h"

#include <ct/core/async.h>
#include <ct/core/config.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/events/event_log.h>
#include <ct/media/media_info.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/playback/cache_budget.h>
#include <ct/project/auto_zoom.h>
#include <ct/project/autosave.h>
#include <ct/project/edit_ops.h>
#include <ct/project/importer.h>
#include <ct/project/serialize.h>
#include <ct/render/compositor.h>
#include <ct/render/render_context.h>
#include <ct/ui/dialogs.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/first_run.h>
#include <ct/ui/settings_window.h>
#include <ct/ui/fonts.h>
#include <ct/ui/panels/export_dialog.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>
#include <ct/version.h>

#include <imgui.h>

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

using Microsoft::WRL::ComPtr;

namespace ct::ui {

using namespace ct::project;

namespace {

constexpr size_t kMaxRecent = 10;

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
double QpcMs(int64_t d) {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return static_cast<double>(d) * 1000.0 / static_cast<double>(freq);
}

std::string RecentPath() { return platform::Paths::SettingsDir() + "\\recent.json"; }

bool SameView(const PreviewView& a, const PreviewView& b) {
    return a.cursor == b.cursor && a.annotations == b.annotations && a.keystrokes == b.keystrokes && a.camera == b.camera;
}

// Media from a probe (P09 9.2: MediaInfo on the IO pool).
bool MediaFromInfo(const std::string& path, const media::MediaInfo& info, Media* out, std::string* why) {
    Media m;
    m.path = path;
    m.duration_us = info.duration_us;
    const int v = info.FirstVideo();
    if (info.is_image) m.kind = Media::Kind::Image;
    else if (v >= 0) m.kind = Media::Kind::Video;
    else if (!info.AudioStreams().empty()) m.kind = Media::Kind::Audio;
    else {
        *why = "The file has no video or audio stream.";
        return false;
    }
    if (const media::StreamInfo* s = info.Find(v)) {
        const bool rot = s->rotation_deg == 90 || s->rotation_deg == 270;
        m.width = rot ? s->height : s->width;
        m.height = rot ? s->width : s->height;
        m.fps_num = s->fps_num;
        m.fps_den = s->fps_den > 0 ? s->fps_den : 1;
    }
    for (size_t i = 0; i < info.AudioStreams().size(); ++i) m.audio_streams.push_back(i == 0 ? "default" : "audio" + std::to_string(i + 1));
    *out = std::move(m);
    return true;
}

} // namespace

struct EditorWindow::Impl {
    explicit Impl(EditorWindow& owner) : w(owner) {}

    EditorWindow& w;
    HWND hwnd = nullptr;
    platform::D3DDevice* dev = nullptr;
    audio::AudioDevices* devices = nullptr;
    render::RenderContext rc;
    render::Compositor comp;
    bool gpu_ok = false;
    detail::EditorPlayer player;
    std::unique_ptr<detail::PlayerTransport> transport;
    std::unique_ptr<detail::CacheMediaProvider> provider;
    struct {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        SizeI size;
    } rt;
    // Preview re-render decision.
    uint64_t last_rev = ~0ull;
    TimeUs last_t = -1;
    PreviewView last_view;
    bool last_exact = false;
    bool rendered_once = false;
    // Scrub latency.
    TimeUs lat_t = -1;
    int64_t lat_start = 0;
    bool lat_pending = false;
    int64_t frame_qpc = 0;

    std::vector<std::function<void()>> deferred;
    std::vector<std::string> recent;
    bool unsaved_prompt = false;
    bool unsaved_open = false;
    std::function<void()> unsaved_then;
    std::vector<Autosave::Recoverable> recover;
    bool recover_open = false;
    bool saving = false;
    std::function<void()> after_save;
    uint64_t autosave_seen = 0;
    double autosave_at = -1;   // ImGui time of the last completed autosave
    int autosave_interval = 60;
    std::map<std::string, std::shared_ptr<events::EventLog>> events;   // recording path -> log (Regenerate)
    std::set<std::string> events_loading;
    bool regen_after_load = false;
    int busy = 0;   // IO jobs whose result the UI waits for (spinner in the status bar)
    std::shared_ptr<int> alive = std::make_shared<int>(1);
    Settings settings;
    std::unique_ptr<SettingsWindow> settings_window;
    // File > Export (P10). Destroyed before the RenderContext it shares (its destructor cancels and joins the job).
    std::unique_ptr<ExportDialog> export_dialog;

    void OpenExport(const std::string& preset_id) {
        if (!gpu_ok || !export_dialog) {
            Banner(BannerPanel::Preview, "Export needs the preview renderer, which could not start.");
            return;
        }
        if (transport && transport->Playing()) transport->Pause();
        ExportDialog::Host host;
        host.device = dev;
        host.render_context = &rc;
        host.audio_devices = devices;
        host.toasts = &shell().Toasts();
        host.owner_hwnd = hwnd;
        host.request_redraw = [h = hwnd] { PostMessageW(h, WM_NULL, 0, 0); };
        export_dialog->Open(st().project, st().Playhead(), host, preset_id);
    }

    void ApplySettings(const Settings& s) {
        autosave_interval = s.editor.autosave_s;
        playback::CacheBudget::SetBytes(static_cast<size_t>(s.editor.frame_cache_mb) << 20);
        if (w.m_shell && !st().project.id.empty()) st().autosave.Init(st().project.id, autosave_interval);
    }

    EditorShell& shell() { return *w.m_shell; }
    EditorState& st() { return w.m_shell->State(); }

    // Posts fn to the UI thread unless the window is gone by then.
    void Post(std::function<void()> fn) {
        std::weak_ptr<int> a = alive;
        Async::PostToUi([a, fn = std::move(fn)] {
            if (a.lock()) fn();
        });
    }
    void Defer(std::function<void()> fn) { deferred.push_back(std::move(fn)); }
    void Banner(BannerPanel p, std::string msg, std::string details = {}) {
        CT_LOG_WARN("editor: %s %s", msg.c_str(), details.c_str());
        shell().Banners().Show(p, std::move(msg), std::move(details));
    }
    void Toast(std::string msg, ToastKind k = ToastKind::Info) { shell().Toasts().Push(std::move(msg), k); }

    // --- document -----------------------------------------------------------------------------------------------
    void Install(Project p, bool mark_dirty) {
        provider->Clear();
        st().autosave.Clear();
        events.clear();
        st().SetProject(std::move(p));
        player.SetProject(nullptr);   // a new document in the same object: drop the old caches and plan
        player.SetProject(&st().project);
        transport->Seek(st().project.ui.playhead_us);
        st().autosave.Init(st().project.id, autosave_interval);
        if (mark_dirty) st().Edit("Recover project", [](Project&) { return true; });
        shell().Banners().Clear(BannerPanel::Media);
        int missing = 0;
        for (const Media& m : st().project.media) missing += m.missing ? 1 : 0;
        if (missing)
            Banner(BannerPanel::Media, Format("%d media file%s could not be found. Right-click a card and choose Relink.", missing, missing == 1 ? "" : "s"));
        last_rev = ~0ull;
        if (!st().project.path.empty()) AddRecent(st().project.path);
        shell().Timeline().Fit(st());
        LoadEventLogs();
    }

    Project NewProject() {
        Project p;
        p.id = Uuid4();
        p.name = str::kUntitled;
        return p;
    }

    // Prompts when dirty; `then` runs after Save / Don't save, never after Cancel.
    void Guard(std::function<void()> then) {
        if (!st().IsDirty()) {
            then();
            return;
        }
        unsaved_prompt = true;
        unsaved_then = std::move(then);
    }

    void NewDoc() {
        Guard([this] { Install(NewProject(), false); });
    }

    void OpenProject(std::string path) {
        Guard([this, path] {
            ++busy;
            Async::Run([this, path, a = std::weak_ptr<int>(alive)] {
                auto p = std::make_shared<Project>();
                Err err;
                const bool ok = project::Load(path, *p, &err);
                std::weak_ptr<int> aa = a;
                Async::PostToUi([this, aa, p, ok, path, msg = err.message] {
                    if (!aa.lock()) return;
                    --busy;
                    if (!ok) {
                        Banner(BannerPanel::Preview, "Could not open " + dialogs::FileStem(path) + ": " + msg, path);
                        RemoveRecent(path);
                        return;
                    }
                    Install(std::move(*p), false);
                    Toast("Opened " + dialogs::FileStem(path), ToastKind::Success);
                });
            });
        });
    }

    bool ProjectEmpty() const {
        const Project& p = w.m_shell->State().project;
        return p.clips.empty() && p.media.empty();
    }

    void ImportRecording(std::string bundle) {
        while (bundle.size() > 3 && (bundle.back() == '\\' || bundle.back() == '/')) bundle.pop_back();
        if (dialogs::FileStem(bundle) == "meta" && dialogs::Extension(bundle) == ".json") bundle = dialogs::ParentDir(bundle);
        const bool fresh = ProjectEmpty() && !st().IsDirty();
        auto work = std::make_shared<Project>(fresh ? NewProject() : st().project);
        if (fresh) work->name = dialogs::FileStem(bundle);
        const uint64_t rev = st().revision;
        ++busy;
        Async::Run([this, bundle, work, rev, fresh, a = std::weak_ptr<int>(alive)] {
            Err err;
            const bool ok = project::ImportRecording(*work, bundle, &err);
            Async::PostToUi([this, a, bundle, work, rev, fresh, ok, msg = err.message] {
                if (!a.lock()) return;
                --busy;
                if (!ok) {
                    Banner(BannerPanel::Media, "Could not import the recording " + dialogs::FileStem(bundle) + ": " + msg, bundle);
                    return;
                }
                if (fresh) {
                    Install(std::move(*work), false);
                } else if (st().revision == rev) {
                    st().Edit("Import recording", [&](Project& p) {
                        const std::string path = p.path;
                        p = std::move(*work);
                        p.path = path;
                        return true;
                    });
                } else {
                    // The project changed meanwhile: import again on the current one (meta and events are cached by now).
                    Err e2;
                    if (!st().EditCopy("Import recording", [&](Project& p) { return project::ImportRecording(p, bundle, &e2); })) {
                        Banner(BannerPanel::Media, "Could not import the recording: " + e2.message, bundle);
                        return;
                    }
                }
                LoadEventLogs();
                Toast("Imported " + dialogs::FileStem(bundle), ToastKind::Success);
            });
        });
    }

    void ImportMedia(std::vector<std::string> paths) {
        for (std::string& path : paths) {
            ++busy;
            Async::Run([this, path, a = std::weak_ptr<int>(alive)] {
                media::MediaInfo info;
                const HRESULT hr = media::MediaInfo::Load(path, info);
                Async::PostToUi([this, a, path, info, hr] {
                    if (!a.lock()) return;
                    --busy;
                    Media m;
                    std::string why;
                    if (FAILED(hr)) why = "The file could not be read (" + HrToString(hr) + ").";
                    if (FAILED(hr) || !MediaFromInfo(path, info, &m, &why)) {
                        Banner(BannerPanel::Media, "Could not import " + dialogs::FileStem(path) + ". " + why, path + "\n" + HrToString(hr));
                        return;
                    }
                    Id id;
                    if (ProjectEmpty() && !st().IsDirty() && m.kind != Media::Kind::Image) {
                        // Opening a media file into an empty editor: a project around it (canvas from the video, the
                        // clip on the timeline, its sound on an audio track).
                        const bool ok = st().EditCopy("Import media", [&](Project& p) {
                            if (!ops::AddMedia(p, m, &id)) return false;
                            p.name = dialogs::FileStem(path);
                            if (m.width > 0 && m.height > 0) {
                                Canvas cv = p.canvas;
                                cv.width = std::clamp((m.width + 1) & ~1, kMinCanvas, kMaxCanvas);
                                cv.height = std::clamp((m.height + 1) & ~1, kMinCanvas, kMaxCanvas);
                                if (m.fps_num > 0) {
                                    cv.fps_num = m.fps_num;
                                    cv.fps_den = m.fps_den;
                                }
                                ops::SetCanvas(p, cv, nullptr);
                            }
                            const bool audio_only = m.kind == Media::Kind::Audio;
                            Id tid;
                            if (!ops::AddTrack(p, audio_only ? Track::Kind::Audio : Track::Kind::Video, audio_only ? "Audio" : "Video", &tid, -1)) return false;
                            Clip c;
                            if (!MakeClipForTrack(p, *p.FindMedia(id), *p.FindTrack(tid), 0, &c)) return false;
                            Id cid;
                            if (!ops::AddClip(p, c, &cid, nullptr)) return false;
                            if (const Clip* v = p.FindClip(cid)) cmd::AddLinkedAudioClip(p, *v);
                            return true;
                        });
                        if (ok) {
                            shell().Timeline().Fit(st());
                            Toast("Opened " + dialogs::FileStem(path), ToastKind::Success);
                            return;
                        }
                    }
                    if (st().Edit("Import media", [&](Project& p) { return ops::AddMedia(p, m, &id); }))
                        Toast("Imported " + dialogs::FileStem(path), ToastKind::Success);
                });
            });
        }
    }

    void Relink(const Id& media) {
        const Media* m = st().project.FindMedia(media);
        if (!m) return;
        const bool rec = m->kind == Media::Kind::Recording;
        std::string pick;
        if (rec) pick = dialogs::PickFolder(hwnd, "Relink recording (.ctrec folder)", dialogs::ParentDir(m->path));
        else {
            auto v = dialogs::OpenFiles(hwnd, dialogs::Filter::Media, false, "Relink media", dialogs::ParentDir(m->path));
            if (!v.empty()) pick = v[0];
        }
        ImGui::GetIO().ClearInputKeys();
        if (pick.empty()) return;
        const Id id = media;
        ++busy;
        Async::Run([this, id, pick, rec, a = std::weak_ptr<int>(alive)] {
            media::MediaInfo info;
            RecordingMeta meta;
            Err err;
            HRESULT hr = S_OK;
            bool ok = true;
            if (rec) ok = ReadRecordingMeta(pick, &meta, &err);
            else hr = media::MediaInfo::Load(pick, info);
            Async::PostToUi([this, a, id, pick, rec, info, meta, ok, hr, msg = err.message] {
                if (!a.lock()) return;
                --busy;
                if (!ok || FAILED(hr)) {
                    Banner(BannerPanel::Media, "Relink failed: " + (rec ? msg : HrToString(hr)), pick);
                    return;
                }
                st().Edit("Relink media", [&](Project& p) {
                    Media* mm = p.FindMedia(id);
                    if (!mm) return false;
                    mm->path = pick;
                    mm->missing = false;
                    if (!rec && info.duration_us > 0) mm->duration_us = std::max(mm->duration_us, info.duration_us);
                    return true;
                });
                int missing = 0;
                for (const Media& m : st().project.media) missing += m.missing ? 1 : 0;
                if (!missing) shell().Banners().Clear(BannerPanel::Media);
                last_rev = ~0ull;
                LoadEventLogs();
            });
        });
    }

    // --- saving ---------------------------------------------------------------------------------------------------
    void SaveTo(std::string path, std::function<void()> then) {
        if (saving) return;
        if (dialogs::Extension(path) != ".ctproj") path += ".ctproj";
        saving = true;
        auto copy = std::make_shared<Project>(st().project);
        if (copy->name.empty() || copy->name == str::kUntitled) copy->name = dialogs::FileStem(path);
        const uint64_t rev = st().revision;
        Async::Run([this, path, copy, rev, then, a = std::weak_ptr<int>(alive)] {
            Err err;
            const bool ok = project::Save(*copy, path, &err);
            Async::PostToUi([this, a, path, rev, then, ok, name = copy->name, msg = err.message] {
                if (!a.lock()) return;
                saving = false;
                if (!ok) {
                    Banner(BannerPanel::Preview, "Could not save the project: " + msg, path);
                    return;
                }
                st().project.path = path;
                st().project.name = name;
                if (st().revision == rev) {
                    st().undo.MarkSaved();
                    st().autosave.Clear();
                    st().autosave.Init(st().project.id, autosave_interval);
                }
                AddRecent(path);
                Toast("Saved " + dialogs::FileStem(path), ToastKind::Success);
                if (then) then();
            });
        });
    }
    void Save(std::function<void()> then) {
        if (st().project.path.empty()) SaveAs(std::move(then));
        else SaveTo(st().project.path, std::move(then));
    }
    void SaveAs(std::function<void()> then) {
        Defer([this, then] {
            const std::string& cur = st().project.path;
            const std::string name = cur.empty() ? (st().project.name.empty() ? std::string(str::kUntitled) : st().project.name) : dialogs::FileStem(cur);
            const std::string dir = cur.empty() ? platform::Paths::DefaultRecordingsDir() : dialogs::ParentDir(cur);
            const std::string path = dialogs::SaveProject(hwnd, name, dir);
            ImGui::GetIO().ClearInputKeys();
            if (!path.empty()) SaveTo(path, then);
        });
    }

    // --- recent files -----------------------------------------------------------------------------------------------
    void AddRecent(const std::string& path) {
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::string& s) { return _stricmp(s.c_str(), path.c_str()) == 0; }), recent.end());
        recent.insert(recent.begin(), path);
        if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
        StoreRecent();
    }
    void RemoveRecent(const std::string& path) {
        recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
        StoreRecent();
    }
    void StoreRecent() {
        shell().recent_files = recent;
        Json j = Json::Array();
        for (const std::string& s : recent) j.Push(Json(s));
        std::string text = j.Dump(2);
        Async::Run([text = std::move(text)] {
            fs::CreateDirectories(platform::Paths::SettingsDir());
            fs::WriteAtomic(RecentPath(), text);
        });
    }
    void LoadRecent() {
        Async::Run([this, a = std::weak_ptr<int>(alive)] {
            std::string text;
            std::vector<std::string> list;
            Json j;
            if (fs::ReadAll(RecentPath(), text) && Json::Parse(text, j) && j.IsArray())
                for (size_t i = 0; i < j.Size() && list.size() < kMaxRecent; ++i)
                    if (j[i].IsString()) list.push_back(j[i].AsString());
            Async::PostToUi([this, a, list] {
                if (!a.lock()) return;
                // Entries added meanwhile (a file opened from the command line) stay first.
                for (const std::string& s : list)
                    if (std::find(recent.begin(), recent.end(), s) == recent.end() && recent.size() < kMaxRecent) recent.push_back(s);
                shell().recent_files = recent;
            });
        });
    }

    // --- Auto-Zoom ------------------------------------------------------------------------------------------------
    void LoadEventLogs() {
        for (const Media& m : st().project.media) {
            if (m.kind != Media::Kind::Recording || !m.has_events || m.missing) continue;
            if (events.count(m.path) || events_loading.count(m.path)) continue;
            events_loading.insert(m.path);
            const std::string bundle = m.path;
            Async::Run([this, bundle, a = std::weak_ptr<int>(alive)] {
                RecordingMeta meta;
                Err err;
                auto log = std::make_shared<events::EventLog>();
                bool ok = ReadRecordingMeta(bundle, &meta, &err) && meta.has_events && log->Load(meta.events_file, &err);
                Async::PostToUi([this, a, bundle, log, ok] {
                    if (!a.lock()) return;
                    events_loading.erase(bundle);
                    events[bundle] = ok ? log : nullptr;
                    if (regen_after_load && events_loading.empty()) {
                        regen_after_load = false;
                        Regenerate();
                    }
                });
            });
        }
    }

    bool Regenerate() {
        if (!events_loading.empty()) {
            regen_after_load = true;
            return false;
        }
        const Project& p = st().project;
        std::vector<CameraKey> keys;
        int clips = 0;
        for (const Clip& c : p.clips) {
            if (c.source != Clip::Source::Screen) continue;
            const Media* m = p.FindMedia(c.media);
            if (!m || m->kind != Media::Kind::Recording) continue;
            auto it = events.find(m->path);
            if (it == events.end() || !it->second) continue;
            const SizeI vs{m->width > 0 ? m->width : p.canvas.width, m->height > 0 ? m->height : p.canvas.height};
            std::vector<CameraKey> k = AutoZoom::ForClip(c, *it->second, vs, p.camera.auto_params);
            keys.insert(keys.end(), k.begin(), k.end());
            ++clips;
        }
        if (!clips) {
            Toast("No recording with click or typing events on the timeline", ToastKind::Warning);
            return false;
        }
        std::sort(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.t_us < b.t_us; });
        keys.erase(std::unique(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.t_us == b.t_us; }), keys.end());
        const bool ok = st().Edit("Regenerate Auto-Zoom", [&](Project& pp) {
            pp.camera.ReplaceGenerated(keys);
            return true;
        });
        st().PruneSelection();
        return ok;
    }

    // --- preview rendering ------------------------------------------------------------------------------------------
    bool EnsureTarget(SizeI size) {
        if (rt.tex && rt.size.w == size.w && rt.size.h == size.h) return true;
        rt = {};
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = static_cast<UINT>(size.w);
        d.Height = static_cast<UINT>(size.h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device* device = dev->Device();
        HRESULT hr = device->CreateTexture2D(&d, nullptr, rt.tex.GetAddressOf());
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(rt.tex.Get(), nullptr, rt.rtv.GetAddressOf());
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(rt.tex.Get(), nullptr, rt.srv.GetAddressOf());
        if (FAILED(hr)) {
            Banner(BannerPanel::Preview, "The preview could not allocate its canvas texture.", HrToString(hr));
            rt = {};
            return false;
        }
        rt.size = size;
        rendered_once = false;
        return true;
    }

    PreviewFrame RenderPreview(EditorState& s, const PreviewView& v) {
        PreviewFrame f;
        if (!gpu_ok) return f;
        const SizeI size{s.project.canvas.width, s.project.canvas.height};
        if (size.IsEmpty() || !EnsureTarget(size)) return f;
        f.tex = reinterpret_cast<ImTextureID>(rt.srv.Get());
        const bool playing = transport->Playing();
        const TimeUs t = s.Playhead();
        // Scrub latency: from the frame the playhead moved (paused) to the frame that rendered it exactly.
        if (!playing && t != lat_t) {
            lat_t = t;
            lat_start = frame_qpc;
            lat_pending = true;
        }
        const bool need = !rendered_once || s.needs_render || playing || !last_exact || !SameView(v, last_view) ||
                          s.revision != last_rev || t != last_t;
        if (!need) {
            f.exact = true;
            return f;
        }
        render::FrameInputs in;
        bool exact = true;
        player.Resolve(t, in, exact);
        render::RenderOptions o;
        o.cursor = v.cursor;
        o.annotations = v.annotations;
        o.keystrokes = v.keystrokes;
        o.gpu_timing = true;
        std::vector<CameraKey> saved;
        if (!v.camera) saved.swap(s.project.camera.keys);   // whole canvas while a camera key is edited
        HRESULT hr;
        {
            platform::D3DDevice::Lock lock(*dev);
            hr = comp.RenderFrame(s.project, t, in, rt.rtv.Get(), o);
        }
        if (!v.camera) saved.swap(s.project.camera.keys);
        if (FAILED(hr)) {
            Banner(BannerPanel::Preview, "The preview could not render this frame.", HrToString(hr));
            f.tex = ImTextureID_Invalid;
            return f;
        }
        rendered_once = true;
        s.needs_render = false;
        last_exact = exact;
        last_view = v;
        last_rev = s.revision;
        last_t = t;
        ++w.m_stats.preview_renders;
        w.m_stats.last_render_cpu_ms = comp.LastTiming().total_ms;
        if (playing) {
            ++w.m_stats.playing_frames;
            if (!exact) ++w.m_stats.frames_inexact_playing;
        }
        if (exact && lat_pending && !playing) {
            lat_pending = false;
            w.m_stats.last_scrub_ms = QpcMs(QpcNow() - lat_start);
            if (w.on_scrub_latency) w.on_scrub_latency(w.m_stats.last_scrub_ms);
        }
        f.exact = exact;
        return f;
    }

    // --- prompts ----------------------------------------------------------------------------------------------------
    void DrawUnsavedPrompt() {
        const char* id = "Unsaved changes##prompt";
        if (unsaved_prompt && !ImGui::IsPopupOpen(id)) {
            ImGui::OpenPopup(id);
            unsaved_open = true;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(Dp(440, 0));
        if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
        const std::string name = st().project.name.empty() ? std::string(str::kUntitled) : st().project.name;
        {
            FontScope f(FontRole::Title);
            ImGui::Text("Save changes to %s?", name.c_str());
        }
        ImGui::Dummy(Dp(0, 2));
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextWrapped("Your changes will be lost if you don't save them.");
        ImGui::PopStyleColor();
        ImGui::Dummy(Dp(0, 10));
        auto finish = [&](int choice) {   // 0 save, 1 discard, 2 cancel
            unsaved_prompt = false;
            unsaved_open = false;
            ImGui::CloseCurrentPopup();
            std::function<void()> then = std::move(unsaved_then);
            unsaved_then = nullptr;
            if (choice == 0) Save(then);
            else if (choice == 1) {
                st().autosave.Clear();
                if (then) then();
            }
        };
        const float bw = Dp(110), bh = Dp(32);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 3 - Dp(8) * 2 - ImGui::GetStyle().WindowPadding.x);
        if (Button("Don't save", ButtonKind::Normal, ImVec2(bw, bh))) finish(1);
        ImGui::SameLine(0, Dp(8));
        if (Button("Cancel", ButtonKind::Normal, ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) finish(2);
        ImGui::SameLine(0, Dp(8));
        if (Button("Save", ButtonKind::Primary, ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) finish(0);
        ImGui::EndPopup();
    }

    void DrawRecoverPrompt() {
        const char* id = "Recover unsaved project?##recover";
        if (!recover.empty() && !recover_open && !unsaved_prompt) {
            ImGui::OpenPopup(id);
            recover_open = true;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(Dp(480, 0));
        if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (recover_open && !ImGui::IsPopupOpen(id)) recover_open = false;
            return;
        }
        const Autosave::Recoverable r = recover.front();
        {
            FontScope f(FontRole::Title);
            ImGui::TextUnformatted("Recover unsaved project?");
        }
        ImGui::Dummy(Dp(0, 2));
        char when[64] = "";
        const time_t tt = static_cast<time_t>(r.mtime);
        tm lt{};
        if (localtime_s(&lt, &tt) == 0) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt);
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
        ImGui::TextWrapped("Camtesia closed unexpectedly while \"%s\" had unsaved changes (autosaved %s).",
                           r.name.empty() ? str::kUntitled : r.name.c_str(), when);
        if (!r.original_path.empty()) ImGui::TextWrapped("%s", r.original_path.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(Dp(0, 10));
        int choice = -1;
        const float bw = Dp(110), bh = Dp(32);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 3 - Dp(8) * 2 - ImGui::GetStyle().WindowPadding.x);
        if (Button("Discard", ButtonKind::Danger, ImVec2(bw, bh))) choice = 1;
        ImGui::SameLine(0, Dp(8));
        if (Button("Not now", ButtonKind::Normal, ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) choice = 2;
        ImGui::SameLine(0, Dp(8));
        if (Button("Recover", ButtonKind::Primary, ImVec2(bw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) choice = 0;   // P11 11.6: Enter = default
        if (choice >= 0) {
            recover.erase(recover.begin());
            recover_open = false;
            ImGui::CloseCurrentPopup();
            if (choice == 0) {
                Guard([this, r] {
                    Async::Run([this, r, a = std::weak_ptr<int>(alive)] {
                        auto p = std::make_shared<Project>();
                        Err err;
                        const bool ok = project::Load(r.autosave_path, *p, &err);
                        if (ok) Autosave::Discard(r);
                        Async::PostToUi([this, a, p, ok, r, msg = err.message] {
                            if (!a.lock()) return;
                            if (!ok) {
                                Banner(BannerPanel::Preview, "The autosave could not be opened: " + msg, r.autosave_path);
                                return;
                            }
                            p->path = r.original_path;
                            Install(std::move(*p), true);
                            Toast("Recovered " + (r.name.empty() ? std::string(str::kUntitled) : r.name), ToastKind::Success);
                        });
                    });
                });
            } else if (choice == 1) {
                Async::Run([r] { Autosave::Discard(r); });
            }
        }
        ImGui::EndPopup();
    }

    // --- actions ------------------------------------------------------------------------------------------------------
    bool OnAction(Action a) {
        switch (a) {
        case Action::New: NewDoc(); return true;
        case Action::Open:
            Defer([this] {
                const std::string dir = st().project.path.empty() ? platform::Paths::DefaultRecordingsDir() : dialogs::ParentDir(st().project.path);
                auto v = dialogs::OpenFiles(hwnd, dialogs::Filter::ProjectOrRecording, false, "Open project", dir);
                ImGui::GetIO().ClearInputKeys();
                if (!v.empty()) w.OpenPath(v[0]);
            });
            return true;
        case Action::Import:
            Defer([this] {
                auto v = dialogs::OpenFiles(hwnd, dialogs::Filter::Media, true, "Import media");
                ImGui::GetIO().ClearInputKeys();
                if (!v.empty()) w.OpenPaths(v);
            });
            return true;
        case Action::Save: Save(nullptr); return true;
        case Action::SaveAs: SaveAs(nullptr); return true;
        case Action::Export: OpenExport({}); return true;
        case Action::OpenRecorder: Toast("The recorder bar opens from the recorder mode (P05)"); return true;
        default: return false;
        }
    }
};

// --- EditorWindow ---------------------------------------------------------------------------------------------------

EditorWindow::EditorWindow() : m_shell(std::make_unique<EditorShell>()) { m_impl = std::make_unique<Impl>(*this); }

EditorWindow::~EditorWindow() { Destroy(); }

bool EditorWindow::Create(void* hwnd, platform::D3DDevice& dev, audio::AudioDevices* devices) {
    Impl& I = *m_impl;
    I.hwnd = static_cast<HWND>(hwnd);
    I.dev = &dev;
    I.devices = devices;
    m_shell->SetHostWindow(hwnd);
    m_shell->persist_layout = true;
    EditorState& s = m_shell->State();

    // Settings that matter here (editor.autosave_s, frame cache budget, snap/ripple): one small read at startup, before
    // the first frame (like --settings-demo); Edit > Preferences opens P11a's SettingsWindow on the same file.
    I.settings = Settings::Defaults();
    I.settings.Load(SettingsFilePath(), nullptr);
    I.ApplySettings(I.settings);
    s.snap = I.settings.editor.snap;
    s.ripple = I.settings.editor.ripple;
    Autosave::SetDirectory(platform::Paths::AutosaveDir());
    I.settings_window = std::make_unique<SettingsWindow>(SettingsFilePath());
    I.settings_window->SetGpuName(dev.AdapterName());
    I.settings_window->request_redraw = [h = I.hwnd] { PostMessageW(h, WM_NULL, 0, 0); };
    I.settings_window->on_changed = [this](const Settings& st) {
        m_impl->settings = st;
        m_impl->ApplySettings(st);
    };
    m_shell->on_preferences = [this] { m_impl->settings_window->Open(m_impl->settings, SettingsSection::Editor); };
    I.export_dialog = std::make_unique<ExportDialog>();
    m_shell->on_export_preset = [this](const std::string& id) { m_impl->OpenExport(id); };
    m_shell->on_export_restore = [this] {
        if (m_impl->export_dialog) m_impl->export_dialog->Restore();
    };

    HRESULT hr = I.rc.Init(dev);
    if (SUCCEEDED(hr)) hr = I.comp.Init(I.rc);
    I.gpu_ok = SUCCEEDED(hr);
    if (!I.gpu_ok) I.Banner(BannerPanel::Preview, "The preview renderer could not start; editing still works.", HrToString(hr));
    if (I.gpu_ok) {
        render::TimelinePlayer::Options po;
        po.audio_output = I.devices != nullptr;
        const HRESULT phr = I.player.Init(I.rc, I.devices, po);
        if (FAILED(phr)) CT_LOG_WARN("editor: TimelinePlayer::Init %s", HrToString(phr).c_str());
    }
    I.transport = std::make_unique<detail::PlayerTransport>(s, I.player);
    s.playback = I.transport.get();
    I.provider = std::make_unique<detail::CacheMediaProvider>(&s.project, [this](const Id& id, bool* failed) {
        playback::MediaCache* c = m_impl->player.Cache(id);
        if (!c) *failed = !m_impl->player.MediaPending();   // nothing opening any more: it has no (openable) file
        return c;
    });
    m_shell->SetMediaProvider(I.provider.get());

    // Shell hooks.
    m_shell->on_action = [this](Action a) { return m_impl->OnAction(a); };
    m_shell->on_import_recording = [this] {
        m_impl->Defer([this] {
            const std::string pick = dialogs::PickFolder(m_impl->hwnd, "Import recording (.ctrec folder)", platform::Paths::DefaultRecordingsDir());
            ImGui::GetIO().ClearInputKeys();
            if (!pick.empty()) m_impl->ImportRecording(pick);
        });
    };
    m_shell->on_open_recent = [this](const std::string& p) { OpenPath(p); };
    m_shell->on_close_project = [this] { m_impl->NewDoc(); };
    m_shell->on_open_logs = [] { dialogs::OpenWithShell(platform::Paths::LogsDir()); };
    m_shell->on_about = [this] {
        m_impl->Toast(Format("Camtesia %s (%s)", CT_VERSION_STRING, CT_GIT_HASH));
    };
    m_shell->Media().on_import = [this] { m_impl->OnAction(Action::Import); };
    m_shell->Media().on_reveal = [this](const Id& id) {
        if (const Media* m = m_shell->State().project.FindMedia(id)) dialogs::RevealInExplorer(m->path);
    };
    m_shell->Media().on_relink = [this](const Id& id) { m_impl->Defer([this, id] { m_impl->Relink(id); }); };
    m_shell->Properties().on_regenerate = [this](EditorState&) { return m_impl->Regenerate(); };
    PreviewPanel& pv = m_shell->Preview();
    pv.render = [this](EditorState& st, const PreviewView& v) { return m_impl->RenderPreview(st, v); };
    pv.video_size = [this](const Clip& c) -> SizeI {
        if (c.source == Clip::Source::Camera) return {};
        playback::MediaCache* mc = m_impl->player.Cache(c.media);
        if (!mc) return {};
        const media::StreamInfo* v = mc->Info().Find(mc->Info().FirstVideo());
        if (!v) return {};
        const bool rot = v->rotation_deg == 90 || v->rotation_deg == 270;
        return rot ? SizeI{v->height, v->width} : SizeI{v->width, v->height};
    };
    m_shell->draw_stats_extra = [this] {
        const auto& t = m_impl->comp.LastTiming();
        ImGui::Text("render  %6.2f ms cpu (bg %.2f lay %.2f cur %.2f ann %.2f)", t.total_ms, t.background_ms, t.layers_ms, t.cursor_ms, t.annotations_ms);
        ImGui::Text("gpu     bg %.2f  layers %.2f  overlay %.2f ms", t.gpu_background_ms, t.gpu_layers_ms, t.gpu_overlay_ms);
        ImGui::Text("cache   %.0f / %.0f MB, %d files, %d opening", m_stats.cache_mb,
                    static_cast<double>(playback::CacheBudget::Bytes()) / (1 << 20), m_stats.open_caches, m_stats.pending_opens);
        if (m_stats.last_scrub_ms >= 0) ImGui::Text("scrub   %6.1f ms (last)", m_stats.last_scrub_ms);
        ImGui::Text("play    %llu frames, %llu inexact", static_cast<unsigned long long>(m_stats.playing_frames),
                    static_cast<unsigned long long>(m_stats.frames_inexact_playing));
        ImGui::Text("redraws %u /s", m_redraws_per_s);
    };

    I.Install(I.NewProject(), false);
    I.LoadRecent();
    // Crash recovery scan (blocking IO on the pool).
    Async::Run([this, a = std::weak_ptr<int>(I.alive)] {
        std::vector<Autosave::Recoverable> list = Autosave::FindRecoverable();
        Async::PostToUi([this, a, list] {
            if (!a.lock()) return;
            m_impl->recover = list;
        });
    });
    return true;
}

void EditorWindow::Destroy() {
    if (!m_impl) return;
    Impl& I = *m_impl;
    *I.alive = 0;
    I.alive.reset();
    // Autosave: a clean close removes its files (the unsaved prompt already ran); keep them when still dirty.
    if (!m_shell->State().IsDirty()) m_shell->State().autosave.Clear();
    m_shell->State().playback = &m_shell->State().simple_playback;
    I.export_dialog.reset();   // cancels and joins a running export before the device objects it uses go
    I.player.Shutdown();
    if (I.provider) I.provider->Clear();
    I.rt = {};
    I.comp.Shutdown();
    I.rc.Shutdown();
    m_impl.reset();
}

void EditorWindow::OpenPath(std::string_view path_in) {
    std::string path(path_in);
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    switch (dialogs::Classify(path)) {
    case dialogs::PathKind::Project: m_impl->OpenProject(path); break;
    case dialogs::PathKind::Recording:
        if (m_impl->ProjectEmpty() && !m_shell->State().IsDirty()) m_impl->ImportRecording(path);
        else m_impl->ImportRecording(path);
        break;
    case dialogs::PathKind::Media: m_impl->ImportMedia({path}); break;
    default:
        m_impl->Banner(BannerPanel::Media, "Camtesia cannot open " + dialogs::FileStem(path) + ": not a project, recording or media file.", path);
        break;
    }
}

void EditorWindow::OpenPaths(const std::vector<std::string>& paths) {
    std::vector<std::string> media;
    for (const std::string& p : paths) {
        if (dialogs::Classify(p) == dialogs::PathKind::Media) media.push_back(p);
        else OpenPath(p);
    }
    if (!media.empty()) m_impl->ImportMedia(media);
}

void EditorWindow::OpenExportDialog(const std::string& preset_id) { m_impl->OpenExport(preset_id); }

void EditorWindow::BeforeFrame() {
    Impl& I = *m_impl;
    I.frame_qpc = QpcNow();
    I.provider->BeginFrame();
    if (!I.deferred.empty()) {
        // Shell dialogs run their own modal loop: never inside an ImGui frame or with the mouse captured.
        ReleaseCapture();
        std::vector<std::function<void()>> run;
        run.swap(I.deferred);
        for (auto& fn : run) fn();
        ImGui::GetIO().ClearInputMouse();
    }
}

void EditorWindow::Frame() {
    Impl& I = *m_impl;
    EditorState& s = m_shell->State();
    // Status bar numbers.
    if (s.autosave.Saves() != I.autosave_seen) {
        I.autosave_seen = s.autosave.Saves();
        I.autosave_at = ImGui::GetTime();
    }
    m_shell->status.autosave_age_s = I.autosave_at >= 0 ? ImGui::GetTime() - I.autosave_at : -1;
    m_stats.cache_mb = static_cast<double>(playback::CacheBudget::Used()) / (1 << 20);
    m_stats.open_caches = static_cast<int>(s.project.media.size());
    m_stats.pending_opens = I.player.MediaPending() ? 1 : 0;
    m_shell->status.cache_mb = m_stats.cache_mb;
    m_shell->status.cache_budget_mb = static_cast<double>(playback::CacheBudget::Bytes()) / (1 << 20);
    m_shell->status.gpu_ms = I.comp.LastTiming().gpu_total_ms;
    m_shell->status.export_fraction = I.export_dialog && I.export_dialog->Busy() ? I.export_dialog->Fraction() : -1.0;
    if (!s.last_error.empty()) {
        m_shell->Toasts().Push(s.last_error, ToastKind::Warning, {}, 4.0f);
        s.last_error.clear();
    }

    m_shell->Frame();
    I.DrawUnsavedPrompt();
    I.DrawRecoverPrompt();
    if (I.settings_window) I.settings_window->Draw();
    if (I.export_dialog) I.export_dialog->Draw();
}

bool EditorWindow::WantsContinuousRedraw() const {
    const Impl& I = *m_impl;
    return m_shell->WantsContinuousRedraw() || I.busy > 0 || I.saving || I.player.MediaPending() || I.unsaved_prompt ||
           !I.deferred.empty() || (I.settings_window && I.settings_window->WantsRedraw()) ||
           (I.export_dialog && I.export_dialog->WantsRedraw());
}

void EditorWindow::OnTimer() {
    EditorState& s = m_shell->State();
    if (!s.project.id.empty()) s.autosave.Tick(s.project);
}

bool EditorWindow::CloseRequested() {
    Impl& I = *m_impl;
    if (!m_shell->State().IsDirty()) return true;
    I.Guard([this] {
        if (on_close_confirmed) on_close_confirmed();
    });
    return false;
}

} // namespace ct::ui
