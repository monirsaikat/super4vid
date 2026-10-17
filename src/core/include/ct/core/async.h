#pragma once
// ct/core/async.h
// Process-wide IO thread pool ("ct-io-pool-N", MTA) for anything too slow for the UI thread (file IO, MF object
// creation, device enumeration), and a way to run a callback on the UI thread afterwards.
// Thread model: Run/PostToUi from any thread. Init/SetUiPoster/Shutdown from the main thread.
// Lifetime: Run() before Init() initializes the pool with 2 threads. Shutdown() runs every queued job, then joins
// the workers; Run() after Shutdown() executes the job synchronously on the caller.

#include <functional>

namespace ct {

struct Async {
    static void Init(int io_threads = 2);
    static void Run(std::function<void()> job);   // FIFO; jobs may run concurrently with each other
    // The app installs a poster that sends `fn` to the UI thread, e.g. PostMessageW(hwnd, WM_CT_INVOKE, 0, (LPARAM)fn).
    // The poster takes ownership of the heap-allocated fn: the UI thread calls (*fn)() and then deletes it, and the
    // poster deletes it itself if posting fails.
    static void SetUiPoster(void (*post)(void* user, std::function<void()>* fn), void* user);
    // Without a poster (unit tests, command-line tools) fn runs synchronously on the calling thread.
    static void PostToUi(std::function<void()> fn);
    static void Shutdown();
};

} // namespace ct
