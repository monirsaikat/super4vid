// ct/core/{spsc_queue,object_pool,ring_buffer,arena}.h
#include <ct/core/arena.h>
#include <ct/core/object_pool.h>
#include <ct/core/ring_buffer.h>
#include <ct/core/spsc_queue.h>
#include <ct/test/ct_test.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

// Runs fn on a raw Win32 thread (the containers do not depend on ct::Thread).
template <class Fn>
HANDLE StartRawThread(Fn* fn) {
    return CreateThread(
        nullptr, 0, [](void* p) -> DWORD {
            (*static_cast<Fn*>(p))();
            return 0;
        },
        fn, 0, nullptr);
}

// Copy-only payload: Pop cannot move the resource out, so the queue must reset the slot itself.
struct CopyOnly {
    std::shared_ptr<int> p;
    CopyOnly() = default;
    CopyOnly(const CopyOnly&) = default;
    CopyOnly& operator=(const CopyOnly&) = default;
};

} // namespace

CT_TEST(spsc_basic_full_empty) {
    ct::SpscQueue<int, 4> q;
    static_assert(ct::SpscQueue<int, 4>::Capacity() == 4);
    int v = 0;
    CT_CHECK(q.Empty());
    CT_CHECK(!q.Pop(v));
    for (int i = 0; i < 4; ++i) CT_CHECK(q.Push(i));
    CT_CHECK(!q.Push(99));   // full: caller counts a drop
    CT_CHECK_EQ(q.Size(), size_t(4));
    for (int i = 0; i < 4; ++i) {
        CT_CHECK(q.Pop(v));
        CT_CHECK_EQ(v, i);
    }
    CT_CHECK(!q.Pop(v));
    // Wrap around many times.
    for (int i = 0; i < 1000; ++i) {
        CT_CHECK(q.Push(i));
        CT_CHECK(q.Pop(v) && v == i);
    }
}

CT_TEST(spsc_nontrivial_slot_released) {
    ct::SpscQueue<CopyOnly, 8> q;
    CopyOnly item;
    item.p = std::make_shared<int>(7);
    CT_CHECK(q.Push(item));
    CT_CHECK_EQ(item.p.use_count(), long(2));
    CopyOnly out;
    CT_CHECK(q.Pop(out));
    CT_CHECK_EQ(item.p.use_count(), long(2));   // item + out; the slot no longer holds a reference
    ct::SpscQueue<std::unique_ptr<std::string>, 2> uq;
    CT_CHECK(uq.Push(std::make_unique<std::string>("x")));
    std::unique_ptr<std::string> s;
    CT_CHECK(uq.Pop(s) && s && *s == "x");
}

CT_TEST(spsc_threads_10m_in_order) {
    static ct::SpscQueue<uint32_t, 1024> q;   // static: 4 KB of slots, keep it off the test stack
    constexpr uint32_t kCount = 10'000'000;
    std::atomic<bool> order_ok{true};
    std::atomic<uint32_t> received{0};
    auto consumer = [&] {
        uint32_t expected = 0, v = 0;
        while (expected < kCount) {
            if (q.Pop(v)) {
                if (v != expected) order_ok.store(false, std::memory_order_relaxed);
                ++expected;
            } else {
                YieldProcessor();
            }
        }
        received.store(expected, std::memory_order_release);
    };
    HANDLE t = StartRawThread(&consumer);
    CT_REQUIRE(t != nullptr);
    uint64_t full_spins = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
        while (!q.Push(i)) {
            ++full_spins;
            YieldProcessor();
        }
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CT_CHECK(order_ok.load());
    CT_CHECK_EQ(received.load(), kCount);
    CT_CHECK(q.Empty());
    if (ct::test::Verbose()) std::printf("    producer saw a full queue %llu times\n", static_cast<unsigned long long>(full_spins));
}

CT_TEST(pool_exhaustion_and_release) {
    ct::ObjectPool<std::vector<int>> pool;
    CT_REQUIRE(pool.Init(3, [](size_t i) { return std::vector<int>(i + 1, static_cast<int>(i)); }));
    CT_CHECK_EQ(pool.Count(), size_t(3));
    CT_CHECK_EQ(pool.InUse(), size_t(0));
    uint32_t a = 99, b = 99, c = 99, d = 99;
    std::vector<int>* pa = pool.Acquire(&a);
    std::vector<int>* pb = pool.Acquire(&b);
    std::vector<int>* pc = pool.Acquire(&c);
    CT_REQUIRE(pa && pb && pc);
    CT_CHECK(pool.Acquire(&d) == nullptr);   // exhausted: null, no allocation
    CT_CHECK_EQ(d, uint32_t(99));
    CT_CHECK_EQ(pool.InUse(), size_t(3));
    CT_CHECK(a != b && b != c && a != c);
    CT_CHECK_EQ(&pool.At(b), pb);
    CT_CHECK_EQ(pool.At(c).size(), size_t(c) + 1);
    pool.Release(b);
    CT_CHECK_EQ(pool.InUse(), size_t(2));
    uint32_t e = 99;
    CT_CHECK(pool.Acquire(&e) == pb);
    CT_CHECK_EQ(e, b);
    pool.Release(a);
    pool.Release(c);
    pool.Release(e);
    CT_CHECK_EQ(pool.InUse(), size_t(0));
}

CT_TEST(pool_cross_thread_release) {
    static ct::ObjectPool<int> pool;
    static ct::SpscQueue<uint32_t, 64> handoff;
    CT_REQUIRE(pool.Init(16, [](size_t i) { return static_cast<int>(i); }));
    constexpr int kRounds = 200'000;
    std::atomic<bool> done{false};
    auto releaser = [&] {
        uint32_t idx = 0;
        for (;;) {
            if (handoff.Pop(idx)) pool.Release(idx);
            else if (done.load(std::memory_order_acquire) && handoff.Empty()) break;
            else YieldProcessor();
        }
    };
    HANDLE t = StartRawThread(&releaser);
    CT_REQUIRE(t != nullptr);
    int acquired = 0;
    while (acquired < kRounds) {
        uint32_t idx = 0;
        if (int* obj = pool.Acquire(&idx)) {
            if (*obj != static_cast<int>(idx)) break;
            while (!handoff.Push(idx)) YieldProcessor();
            ++acquired;
        } else {
            YieldProcessor();
        }
    }
    done.store(true, std::memory_order_release);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CT_CHECK_EQ(acquired, kRounds);
    CT_CHECK_EQ(pool.InUse(), size_t(0));
}

CT_TEST(audio_ring_wraparound) {
    ct::AudioRing ring;
    CT_REQUIRE(ring.Init(8, 2));
    CT_CHECK_EQ(ring.Free(), size_t(8));
    float in[20], out[20];
    for (int i = 0; i < 20; ++i) in[i] = static_cast<float>(i);
    CT_CHECK_EQ(ring.Write(in, 6), size_t(6));
    CT_CHECK_EQ(ring.Read(out, 4), size_t(4));
    CT_CHECK(out[0] == 0.0f && out[7] == 7.0f);
    // 2 frames left (pos 4..5); write 6 more: wraps past the end of the 8-frame buffer.
    CT_CHECK_EQ(ring.Write(in, 6), size_t(6));
    CT_CHECK_EQ(ring.Available(), size_t(8));
    CT_CHECK_EQ(ring.Write(in, 1), size_t(0));   // full
    CT_CHECK_EQ(ring.Read(out, 10), size_t(8));  // partial read returns what is there
    const float expect[16] = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    bool ok = true;
    for (int i = 0; i < 16; ++i) ok = ok && out[i] == expect[i];
    CT_CHECK(ok);
    CT_CHECK_EQ(ring.Available(), size_t(0));
    CT_CHECK_EQ(ring.Write(in, 10), size_t(8));  // partial write
    ring.Clear();
    CT_CHECK_EQ(ring.Available(), size_t(0));
    CT_CHECK_EQ(ring.Free(), size_t(8));
}

CT_TEST(audio_ring_threads) {
    static ct::AudioRing ring;
    CT_REQUIRE(ring.Init(480, 2));
    constexpr size_t kFrames = 2'000'000;
    std::atomic<bool> ok{true};
    auto consumer = [&] {
        float buf[2 * 97];
        size_t got = 0;
        while (got < kFrames) {
            const size_t n = ring.Read(buf, 97);
            for (size_t i = 0; i < n; ++i)
                if (buf[i * 2] != static_cast<float>((got + i) % 100000) || buf[i * 2 + 1] != -buf[i * 2])
                    ok.store(false, std::memory_order_relaxed);
            got += n;
            if (n == 0) YieldProcessor();
        }
    };
    HANDLE t = StartRawThread(&consumer);
    CT_REQUIRE(t != nullptr);
    float buf[2 * 61];
    size_t sent = 0;
    while (sent < kFrames) {
        const size_t want = std::min<size_t>(61, kFrames - sent);
        for (size_t i = 0; i < want; ++i) {
            buf[i * 2] = static_cast<float>((sent + i) % 100000);
            buf[i * 2 + 1] = -buf[i * 2];
        }
        size_t done = 0;
        while (done < want) {
            const size_t n = ring.Write(buf + done * 2, want - done);
            if (n == 0) YieldProcessor();
            done += n;
        }
        sent += want;
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CT_CHECK(ok.load());
}

CT_TEST(arena_alignment_and_reset) {
    ct::Arena arena;
    CT_REQUIRE(arena.Init(64 * 1024));
    CT_CHECK_EQ(arena.Capacity(), size_t(64 * 1024));
    void* a = arena.Alloc(1, 1);
    void* b = arena.Alloc(8, 64);
    void* c = arena.Alloc(3);
    CT_REQUIRE(a && b && c);
    CT_CHECK_EQ(reinterpret_cast<uintptr_t>(b) % 64, uintptr_t(0));
    CT_CHECK_EQ(reinterpret_cast<uintptr_t>(c) % 16, uintptr_t(0));
    CT_CHECK_EQ(reinterpret_cast<uintptr_t>(a) % 4096, uintptr_t(0));   // VirtualAlloc base is page-aligned
    struct alignas(32) Vec { float v[8]; };
    Vec* vecs = arena.New<Vec>(10);
    CT_REQUIRE(vecs != nullptr);
    CT_CHECK_EQ(reinterpret_cast<uintptr_t>(vecs) % 32, uintptr_t(0));
    CT_CHECK(vecs[9].v[7] == 0.0f);   // value-initialized
    const size_t used = arena.Used();
    CT_CHECK(used >= 64 + 3 + 320);
    CT_CHECK(arena.Alloc(64 * 1024) == nullptr);   // does not fit: null, arena unchanged
    CT_CHECK_EQ(arena.Used(), used);
    arena.Reset();
    CT_CHECK_EQ(arena.Used(), size_t(0));
    CT_CHECK_EQ(arena.Alloc(64 * 1024), a);   // whole block after reset
    CT_CHECK(arena.Alloc(1) == nullptr);
    CT_CHECK(arena.New<int>(SIZE_MAX / 2) == nullptr);   // size overflow
}
