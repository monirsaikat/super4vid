// ct/core/{thread,sync,async}.h
#include <ct/core/async.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/test/ct_test.h>

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

CT_TEST(thread_name_priority_com) {
    ct::Thread t;
    std::wstring seen_name;
    int seen_priority = -100;
    HRESULT com_hr = E_FAIL;
    ct::Thread::Options opt;
    opt.name = "ct-test-worker";
    opt.priority = THREAD_PRIORITY_ABOVE_NORMAL;
    CT_REQUIRE(t.Start(opt, [&] {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) seen_name = desc;
        LocalFree(desc);
        seen_priority = GetThreadPriority(GetCurrentThread());
        // Already in the MTA: a second MTA init returns S_FALSE, an STA request fails with RPC_E_CHANGED_MODE.
        com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com_hr)) CoUninitialize();
    }));
    CT_CHECK(t.Handle() != nullptr);
    CT_CHECK(t.Id() != 0);
    CT_CHECK(!t.Start(opt, [] {}));   // already started
    CT_CHECK(t.Join());
    CT_CHECK(!t.IsRunning());
    CT_CHECK(seen_name == L"ct-test-worker");
    CT_CHECK_EQ(seen_priority, THREAD_PRIORITY_ABOVE_NORMAL);
    CT_CHECK_EQ(com_hr, RPC_E_CHANGED_MODE);
    // Restartable after Join.
    std::atomic<int> runs{0};
    CT_REQUIRE(t.Start({}, [&] { runs.fetch_add(1); }));
    CT_CHECK(t.Join());
    CT_CHECK_EQ(runs.load(), 1);
}

CT_TEST(thread_mmcss_task) {
    ct::Thread t;
    ct::Thread::Options opt;
    opt.name = "ct-test-audio";
    opt.mmcss_task = L"Pro Audio";
    opt.com_mta = false;
    std::atomic<bool> applied_inside{false};
    CT_REQUIRE(t.Start(opt, [&] {
        // The task is applied before the body runs.
        applied_inside.store(t.MmcssApplied());
    }));
    CT_CHECK(t.Join(5000));
    if (!t.MmcssApplied()) CT_SKIP("MMCSS unavailable (AvSetMmThreadCharacteristicsW failed)");
    CT_CHECK(applied_inside.load());
}

CT_TEST(thread_join_timeout) {
    ct::Event release_worker(true, false);
    ct::Thread t;
    CT_REQUIRE(t.Start({}, [&] { release_worker.Wait(); }));
    CT_CHECK(t.IsRunning());
    CT_CHECK(!t.Join(20));   // still blocked: timeout path
    CT_CHECK(t.IsRunning());
    CT_CHECK(t.Handle() != nullptr);
    release_worker.Set();
    CT_CHECK(t.Join(5000));
    CT_CHECK(!t.IsRunning());
    CT_CHECK(t.Handle() == nullptr);
    ct::Thread never_started;
    CT_CHECK(never_started.Join(0));
}

CT_TEST(sync_event_and_srwlock) {
    ct::Event auto_ev;   // auto-reset
    CT_CHECK(!auto_ev.Wait(0));
    auto_ev.Set();
    CT_CHECK(auto_ev.Wait(0));
    CT_CHECK(!auto_ev.Wait(0));   // consumed
    ct::Event manual(true, true);
    CT_CHECK(manual.Wait(0) && manual.Wait(0));
    manual.Reset();
    CT_CHECK(!manual.Wait(0));

    ct::SrwLock lock;
    int64_t counter = 0;
    constexpr int kPerThread = 100'000;
    ct::Thread workers[4];
    for (ct::Thread& w : workers)
        CT_REQUIRE(w.Start({}, [&] {
            for (int i = 0; i < kPerThread; ++i) {
                ct::ExclusiveGuard g(lock);
                ++counter;
            }
        }));
    for (ct::Thread& w : workers) CT_CHECK(w.Join(10000));
    {
        ct::SharedGuard g(lock);
        CT_CHECK_EQ(counter, int64_t(4) * kPerThread);
    }
    CT_CHECK(lock.TryLockExclusive());
    lock.UnlockExclusive();
}

namespace {

struct FakeUi {
    std::vector<std::function<void()>*> posted;
    static void Post(void* user, std::function<void()>* fn) { static_cast<FakeUi*>(user)->posted.push_back(fn); }
};

} // namespace

CT_TEST(async_pool_runs_jobs_and_posts) {
    ct::Async::Init(2);
    std::atomic<int> done{0};
    std::atomic<bool> names_ok{true};
    ct::Event all_done(true, false);
    constexpr int kJobs = 200;
    for (int i = 0; i < kJobs; ++i) {
        ct::Async::Run([&] {
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
                if (std::wstring(desc).rfind(L"ct-io-pool-", 0) != 0) names_ok.store(false);
            }
            LocalFree(desc);
            if (done.fetch_add(1) + 1 == kJobs) all_done.Set();
        });
    }
    CT_CHECK(all_done.Wait(10000));
    CT_CHECK(names_ok.load());

    // UI posting: through the installed poster, executed (and freed) by the "UI thread".
    FakeUi ui;
    ct::Async::SetUiPoster(&FakeUi::Post, &ui);
    int ui_calls = 0;
    ct::Async::PostToUi([&] { ++ui_calls; });
    CT_CHECK_EQ(ui.posted.size(), size_t(1));
    CT_CHECK_EQ(ui_calls, 0);
    for (auto* fn : ui.posted) {
        (*fn)();
        delete fn;
    }
    CT_CHECK_EQ(ui_calls, 1);
    ct::Async::SetUiPoster(nullptr, nullptr);
    ct::Async::PostToUi([&] { ++ui_calls; });   // no poster: synchronous
    CT_CHECK_EQ(ui_calls, 2);

    // Shutdown drains queued jobs.
    std::atomic<int> late{0};
    for (int i = 0; i < 50; ++i)
        ct::Async::Run([&] {
            Sleep(0);
            late.fetch_add(1);
        });
    ct::Async::Shutdown();
    CT_CHECK_EQ(late.load(), 50);
    int inline_runs = 0;
    ct::Async::Run([&] { ++inline_runs; });   // after Shutdown: inline
    CT_CHECK_EQ(inline_runs, 1);
}
