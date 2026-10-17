#pragma once
// ct/core/object_pool.h
// Fixed set of pre-created objects handed out by index. Acquire returns nullptr when exhausted (a counted drop,
// never an allocation). The free list is an SPSC ring of indices.
// Thread model: one thread calls Acquire, one (possibly other) thread calls Release; Init before both start.
// The index must travel from the acquirer to the releaser through a synchronizing handoff (e.g. SpscQueue).
// Lifetime: objects live as long as the pool; releasing an index does not destroy the object.

#include <ct/core/assert.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ct {

#pragma warning(push)
#pragma warning(disable : 4324)   // alignas(64) padding keeps the two cursors on separate cache lines

template <class T>
class ObjectPool {
public:
    ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // make(size_t index) -> T. Not thread-safe; call before the acquiring/releasing threads use the pool.
    template <class Factory>
    bool Init(size_t count, Factory make) {
        if (count == 0 || count > UINT32_MAX) return false;
        m_objects.clear();
        m_objects.reserve(count);
        for (size_t i = 0; i < count; ++i) m_objects.push_back(make(i));
        m_free = std::make_unique<uint32_t[]>(count);
        m_in_use = std::make_unique<std::atomic<uint8_t>[]>(count);
        for (size_t i = 0; i < count; ++i) {
            m_free[i] = static_cast<uint32_t>(i);
            m_in_use[i].store(0, std::memory_order_relaxed);
        }
        m_count = count;
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(count, std::memory_order_release);
        return true;
    }

    T* Acquire(uint32_t* index_out = nullptr) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire)) return nullptr;
        const uint32_t index = m_free[head % m_count];
        m_head.store(head + 1, std::memory_order_release);
        const uint8_t was = m_in_use[index].exchange(1, std::memory_order_relaxed);
        CT_ASSERT(was == 0);
        (void)was;
        if (index_out) *index_out = index;
        return &m_objects[index];
    }

    void Release(uint32_t index) {
        CT_ASSERT(index < m_count);
        const uint8_t was = m_in_use[index].exchange(0, std::memory_order_relaxed);
        CT_ASSERT(was == 1);   // double release
        if (was != 1) return;
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        (void)m_head.load(std::memory_order_acquire);   // the acquirer's read of this free slot happened before
        m_free[tail % m_count] = index;
        m_tail.store(tail + 1, std::memory_order_release);
    }

    T& At(uint32_t index) {
        CT_ASSERT(index < m_count);
        return m_objects[index];
    }
    size_t Count() const { return m_count; }
    size_t InUse() const {
        const size_t free = m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire);
        return free <= m_count ? m_count - free : 0;
    }

private:
    std::vector<T> m_objects;
    std::unique_ptr<uint32_t[]> m_free;
    std::unique_ptr<std::atomic<uint8_t>[]> m_in_use;
    size_t m_count = 0;
    alignas(64) std::atomic<size_t> m_head{0};   // advanced by Acquire
    alignas(64) std::atomic<size_t> m_tail{0};   // advanced by Release
};

#pragma warning(pop)

} // namespace ct
