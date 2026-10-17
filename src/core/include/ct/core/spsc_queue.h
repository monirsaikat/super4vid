#pragma once
// ct/core/spsc_queue.h
// Wait-free single-producer / single-consumer queue with a fixed power-of-two capacity, stored inline.
// Thread model: exactly one thread calls Push, exactly one (other) thread calls Pop. Size/Empty are approximate
// from any other thread. Lifetime: elements still queued are destroyed with the queue.
// Push returns false when full; the caller counts a drop (never block or allocate on a hot path).

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace ct {

#pragma warning(push)
#pragma warning(disable : 4324)   // padding from alignas(64) is the point: head, tail and slots on separate cache lines

template <class T, size_t CapacityPow2>
class SpscQueue {
    static_assert(CapacityPow2 >= 2 && (CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_default_constructible_v<T>, "slots are default-constructed");

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool Push(T&& v) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_head.load(std::memory_order_acquire) == CapacityPow2) return false;
        m_slots[tail & kMask] = std::move(v);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool Push(const T& v) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_head.load(std::memory_order_acquire) == CapacityPow2) return false;
        m_slots[tail & kMask] = v;
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool Pop(T& out) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire)) return false;
        T& slot = m_slots[head & kMask];
        out = std::move(slot);
        // Release whatever the slot still owns (copy-only types, moved-from containers) on the consumer thread.
        if constexpr (!std::is_trivially_copyable_v<T>) slot = T{};
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    size_t Size() const {
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        return tail >= head ? tail - head : 0;
    }
    bool Empty() const { return Size() == 0; }
    static constexpr size_t Capacity() { return CapacityPow2; }

private:
    static constexpr size_t kMask = CapacityPow2 - 1;
    alignas(64) std::atomic<size_t> m_head{0};   // next slot to pop (written by the consumer)
    alignas(64) std::atomic<size_t> m_tail{0};   // next slot to push (written by the producer)
    alignas(64) std::array<T, CapacityPow2> m_slots{};
};

#pragma warning(pop)

} // namespace ct
