#pragma once
// ct/project/autosave.h
// Crash recovery (docs/04 section 6): <autosave dir>/<project-id>.ctproj is written every `interval_s` while the
// project is dirty, and after every 20 edits. A sidecar <project-id>.lock is held open (deny-write) by the owning
// process for as long as the Autosave lives and records the PID and the project's original path. On startup a lock
// that can be opened for writing belongs to a dead process: its autosave is offered for recovery.
// Thread model: Init/NotifyChanged/Tick/Clear on the UI thread. Tick serializes on the caller (fast) and writes the
// file on the ct::Async pool (older pending writes are skipped); it never waits for that IO. FindRecoverable/Discard do blocking IO.
// Lifetime: the destructor closes the lock but keeps the files (the editor calls Clear() on a clean close/save).

#include <ct/project/project.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ct::project {

class Autosave {
public:
    static constexpr int kEditsPerSave = 20;

    Autosave();
    ~Autosave();
    Autosave(const Autosave&) = delete;
    Autosave& operator=(const Autosave&) = delete;

    // Process-wide autosave folder. The app sets it from ct::platform::Paths at startup; the default is
    // %LOCALAPPDATA%\super4vid\autosave. Created on first use.
    static void SetDirectory(std::string_view dir);
    static std::string Directory();

    // interval_s <= 0 disables time-based saves (every 20th edit still saves).
    void Init(const Id& project_id, int interval_s);
    void NotifyChanged();                       // one undoable edit
    void Tick(const Project& p);                // UI timer, 1 Hz
    void Tick(const Project& p, int64_t now_us);   // testable variant (ct::Clock::NowUs time base)
    bool SaveNow(const Project& p);             // synchronous write; also takes the lock
    void Clear();                               // on explicit save / close: deletes autosave + lock, not dirty
    void SetSynchronous(bool sync) { m_sync = sync; }   // tests: write on the calling thread
    // Tests: called on the writer (pool) thread right before each background write, outside the state lock.
    using TestWriteHook = void (*)(void* ctx);
    void SetTestWriteHook(TestWriteHook hook, void* ctx);

    bool Dirty() const { return m_edits > 0; }
    // The UI's 1 Hz timer has work to do: unsaved edits, or a background write in flight or failed (retried by
    // Tick). False lets an idle editor disarm the timer (P11 11.1: no periodic wake-ups while nothing changes).
    bool NeedsTick() const;
    uint64_t Saves() const;    // completed writes
    std::string AutosavePath() const;
    std::string LockPath() const;

    struct Recoverable {
        std::string autosave_path;
        std::string original_path;   // empty when the project was never saved
        std::string name;
        int64_t mtime;               // unix seconds of the autosave file
    };
    // Autosaves whose lock is missing or not held by a live process. Never returns this process's own projects.
    static std::vector<Recoverable> FindRecoverable();
    static bool Discard(const Recoverable& r);   // deletes the autosave and its lock

private:
    struct Shared;
    bool TakeLock(const Project& p);
    void WriteLockInfo(const Project& p);
    void StartWrite(std::string text);

    Id m_id;
    int m_interval_s = 0;
    int m_edits = 0;
    int64_t m_last_save_us = 0;
    bool m_sync = false;
    int m_lock_fd = -1;
    std::string m_lock_original;   // original path last written into the lock
    std::shared_ptr<Shared> m_shared;
};

} // namespace ct::project
