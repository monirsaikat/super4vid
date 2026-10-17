#include <ct/project/autosave.h>

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/time.h>
#include <ct/project/serialize.h>

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cwchar>

namespace ct::project {

namespace {

SrwLock g_dir_lock;
std::string g_dir;   // guarded by g_dir_lock

std::string DefaultDirectory() {
    wchar_t* local = nullptr;
    size_t len = 0;
    std::string dir;
    if (_wdupenv_s(&local, &len, L"LOCALAPPDATA") == 0 && local) {
        dir = PathJoin(WideToUtf8(local), "Camtesia\\autosave");
        std::free(local);
    } else {
        dir = PathJoin(PathParent(fs::TempPath("")), "Camtesia\\autosave");
    }
    return dir;
}

// Opens a lock file for read+write, denying other writers. -1 when another process holds it.
int OpenLock(const std::string& path, bool create) {
    int fd = -1;
    const int flags = _O_RDWR | _O_BINARY | (create ? _O_CREAT : 0);
    if (_wsopen_s(&fd, fs::ToWinPath(path).c_str(), flags, _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0) return -1;
    return fd;
}

std::string ReadFd(int fd) {
    std::string s;
    _lseek(fd, 0, SEEK_SET);
    char buf[1024];
    int n = 0;
    while ((n = _read(fd, buf, sizeof(buf))) > 0) s.append(buf, static_cast<size_t>(n));
    return s;
}

} // namespace

struct Autosave::Shared {
    SrwLock lock;
    uint64_t queued = 0;    // generation of the newest text handed to a writer
    uint64_t written = 0;   // generation on disk
    uint64_t saves = 0;
    std::string path;
};

Autosave::Autosave() : m_shared(std::make_shared<Shared>()) {}

Autosave::~Autosave() {
    if (m_lock_fd >= 0) _close(m_lock_fd);
}

void Autosave::SetDirectory(std::string_view dir) {
    ExclusiveGuard g(g_dir_lock);
    g_dir = std::string(dir);
}

std::string Autosave::Directory() {
    {
        SharedGuard g(g_dir_lock);
        if (!g_dir.empty()) return g_dir;
    }
    ExclusiveGuard g(g_dir_lock);
    if (g_dir.empty()) g_dir = DefaultDirectory();
    return g_dir;
}

std::string Autosave::AutosavePath() const { return PathJoin(Directory(), m_id + ".ctproj"); }
std::string Autosave::LockPath() const { return PathJoin(Directory(), m_id + ".lock"); }

uint64_t Autosave::Saves() const {
    SharedGuard g(m_shared->lock);
    return m_shared->saves;
}

void Autosave::Init(const Id& project_id, int interval_s) {
    if (m_lock_fd >= 0 && project_id != m_id) {
        _close(m_lock_fd);
        m_lock_fd = -1;
    }
    m_id = project_id;
    m_interval_s = interval_s;
    m_edits = 0;
    m_last_save_us = Clock::NowUs();
    m_lock_original.clear();
}

void Autosave::NotifyChanged() { ++m_edits; }

void Autosave::Tick(const Project& p) { Tick(p, Clock::NowUs()); }

void Autosave::Tick(const Project& p, int64_t now_us) {
    if (m_id.empty() || m_edits == 0) return;
    const bool time_due = m_interval_s > 0 && now_us - m_last_save_us >= int64_t(m_interval_s) * kUsPerSecond;
    if (!time_due && m_edits < kEditsPerSave) return;
    if (!TakeLock(p)) return;
    m_edits = 0;
    m_last_save_us = now_us;
    StartWrite(ToJson(p));   // absolute media paths: the autosave lives in another folder
}

bool Autosave::SaveNow(const Project& p) {
    if (m_id.empty() || !TakeLock(p)) return false;
    const std::string text = ToJson(p);
    Shared& s = *m_shared;
    ExclusiveGuard g(s.lock);
    const uint64_t gen = ++s.queued;
    if (!fs::WriteAtomic(AutosavePath(), text)) return false;
    s.written = gen;
    ++s.saves;
    m_edits = 0;
    m_last_save_us = Clock::NowUs();
    return true;
}

void Autosave::StartWrite(std::string text) {
    std::shared_ptr<Shared> shared = m_shared;
    uint64_t gen;
    {
        ExclusiveGuard g(shared->lock);
        gen = ++shared->queued;
        shared->path = AutosavePath();
    }
    auto job = [shared, gen, text = std::move(text)]() {
        ExclusiveGuard g(shared->lock);
        if (gen <= shared->written || gen != shared->queued) return;   // a newer snapshot is coming
        if (fs::WriteAtomic(shared->path, text)) {
            shared->written = gen;
            ++shared->saves;
        }
    };
    if (m_sync) job();
    else Async::Run(std::move(job));
}

bool Autosave::TakeLock(const Project& p) {
    if (m_lock_fd < 0) {
        fs::CreateDirectories(Directory());
        m_lock_fd = OpenLock(LockPath(), true);
        if (m_lock_fd < 0) return false;   // another live instance edits the same project
        m_lock_original = "\x01";          // force the first write
    }
    if (m_lock_original != p.path) WriteLockInfo(p);
    return true;
}

void Autosave::WriteLockInfo(const Project& p) {
    Json info = Json::Object();
    info.Set("pid", static_cast<int64_t>(_getpid()));
    info.Set("original_path", p.path);
    info.Set("name", p.name);
    const std::string text = info.Dump(0);
    _chsize_s(m_lock_fd, 0);
    _lseek(m_lock_fd, 0, SEEK_SET);
    _write(m_lock_fd, text.data(), static_cast<unsigned>(text.size()));
    _commit(m_lock_fd);
    m_lock_original = p.path;
}

void Autosave::Clear() {
    if (m_id.empty()) return;
    {
        // Wait for an in-flight write and make sure no queued one lands after the delete.
        ExclusiveGuard g(m_shared->lock);
        m_shared->written = m_shared->queued;
        fs::Delete(AutosavePath());
    }
    if (m_lock_fd >= 0) {
        _close(m_lock_fd);
        m_lock_fd = -1;
    }
    fs::Delete(LockPath());
    m_edits = 0;
    m_lock_original.clear();
}

std::vector<Autosave::Recoverable> Autosave::FindRecoverable() {
    std::vector<Recoverable> out;
    const std::string dir = Directory();
    std::vector<fs::Entry> entries;
    if (!fs::List(dir, entries)) return out;
    for (const fs::Entry& e : entries) {
        if (e.is_dir || !EndsWith(ToLowerAscii(e.name), ".ctproj")) continue;
        const std::string stem(PathStem(e.name));
        const std::string lock = PathJoin(dir, stem + ".lock");
        Recoverable r;
        r.autosave_path = PathJoin(dir, e.name);
        r.mtime = e.mtime_unix;
        if (fs::Exists(lock)) {
            const int fd = OpenLock(lock, false);
            if (fd < 0) continue;   // held by a live process (possibly this one)
            Json info;
            if (Json::Parse(ReadFd(fd), info)) {
                r.original_path = info.Get<std::string>("original_path", "");
                r.name = info.Get<std::string>("name", "");
            }
            _close(fd);
        }
        if (r.name.empty()) {
            std::string text;
            Json j;
            if (fs::ReadAll(r.autosave_path, text) && Json::Parse(text, j)) r.name = j.Get<std::string>("name", "");
        }
        out.push_back(std::move(r));
    }
    return out;
}

bool Autosave::Discard(const Recoverable& r) {
    const std::string lock = PathJoin(PathParent(r.autosave_path), std::string(PathStem(r.autosave_path)) + ".lock");
    const bool a = fs::Delete(r.autosave_path);
    const bool b = fs::Delete(lock);
    return a && b;
}

} // namespace ct::project
