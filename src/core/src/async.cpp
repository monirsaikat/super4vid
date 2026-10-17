#define CT_LOG_CATEGORY "async"
#include <ct/core/async.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/thread.h>

#include <windows.h>

#include <memory>
#include <utility>
#include <vector>

namespace ct {

namespace {

using Job = std::function<void()>;
using UiPoster = void (*)(void* user, std::function<void()>* fn);

struct AsyncState {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    std::vector<Job> queue;   // FIFO: [head, size)
    size_t head = 0;
    std::vector<std::unique_ptr<Thread>> workers;
    bool started = false;
    bool stopping = false;
    bool shut_down = false;   // Shutdown() ran; Run() executes inline from now on
    SRWLOCK poster_lock = SRWLOCK_INIT;
    UiPoster poster = nullptr;
    void* poster_user = nullptr;

    // Safety net for a missing Async::Shutdown(): static destructors run while the workers are still alive, and a
    // plain ~Thread would wait forever for a worker blocked on the condition variable.
    ~AsyncState() {
        if (workers.empty()) return;
        AcquireSRWLockExclusive(&lock);
        stopping = true;
        ReleaseSRWLockExclusive(&lock);
        WakeAllConditionVariable(&cv);
        for (auto& t : workers)
            if (!t->Join(2000)) (void)t.release();   // a hung job: leak the Thread rather than hang the exit
    }
};

AsyncState& S() {
    static AsyncState state;
    return state;
}

void WorkerLoop() {
    AsyncState& s = S();
    for (;;) {
        AcquireSRWLockExclusive(&s.lock);
        while (s.head == s.queue.size() && !s.stopping) SleepConditionVariableSRW(&s.cv, &s.lock, INFINITE, 0);
        if (s.head == s.queue.size()) {   // stopping and drained
            ReleaseSRWLockExclusive(&s.lock);
            return;
        }
        Job job = std::move(s.queue[s.head++]);
        if (s.head == s.queue.size()) {
            s.queue.clear();
            s.head = 0;
        } else if (s.head > 64 && s.head * 2 > s.queue.size()) {
            s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<ptrdiff_t>(s.head));
            s.head = 0;
        }
        ReleaseSRWLockExclusive(&s.lock);
        if (job) job();
    }
}

// Caller holds s.lock exclusively.
void StartWorkersLocked(AsyncState& s, int io_threads) {
    if (s.started) return;
    if (io_threads < 1) io_threads = 1;
    s.started = true;
    s.stopping = false;
    s.shut_down = false;
    for (int i = 0; i < io_threads; ++i) {
        auto t = std::make_unique<Thread>();
        const std::string name = Format("ct-io-pool-%d", i);
        Thread::Options opt;
        opt.name = name.c_str();
        if (t->Start(opt, &WorkerLoop))
            s.workers.push_back(std::move(t));
        else
            CT_LOG_ERROR("could not start %s", name.c_str());
    }
}

} // namespace

void Async::Init(int io_threads) {
    AsyncState& s = S();
    AcquireSRWLockExclusive(&s.lock);
    StartWorkersLocked(s, io_threads);
    ReleaseSRWLockExclusive(&s.lock);
}

void Async::Run(std::function<void()> job) {
    AsyncState& s = S();
    AcquireSRWLockExclusive(&s.lock);
    if (s.shut_down || s.stopping) {
        ReleaseSRWLockExclusive(&s.lock);
        if (job) job();
        return;
    }
    if (!s.started) StartWorkersLocked(s, 2);
    if (s.workers.empty()) {   // thread creation failed: never lose the job
        ReleaseSRWLockExclusive(&s.lock);
        if (job) job();
        return;
    }
    s.queue.push_back(std::move(job));
    ReleaseSRWLockExclusive(&s.lock);
    WakeConditionVariable(&s.cv);
}

void Async::SetUiPoster(void (*post)(void* user, std::function<void()>* fn), void* user) {
    AsyncState& s = S();
    AcquireSRWLockExclusive(&s.poster_lock);
    s.poster = post;
    s.poster_user = user;
    ReleaseSRWLockExclusive(&s.poster_lock);
}

void Async::PostToUi(std::function<void()> fn) {
    AsyncState& s = S();
    AcquireSRWLockShared(&s.poster_lock);
    const UiPoster poster = s.poster;
    void* const user = s.poster_user;
    ReleaseSRWLockShared(&s.poster_lock);
    if (poster)
        poster(user, new std::function<void()>(std::move(fn)));
    else if (fn)
        fn();
}

void Async::Shutdown() {
    AsyncState& s = S();
    AcquireSRWLockExclusive(&s.lock);
    if (!s.started) {
        s.shut_down = true;
        ReleaseSRWLockExclusive(&s.lock);
        return;
    }
    s.stopping = true;
    std::vector<std::unique_ptr<Thread>> workers = std::move(s.workers);
    s.workers.clear();
    ReleaseSRWLockExclusive(&s.lock);
    WakeAllConditionVariable(&s.cv);
    for (auto& t : workers) t->Join(INFINITE);   // workers drain the queue first
    AcquireSRWLockExclusive(&s.lock);
    s.queue.clear();
    s.head = 0;
    s.started = false;
    s.stopping = false;
    s.shut_down = true;
    ReleaseSRWLockExclusive(&s.lock);
}

} // namespace ct
