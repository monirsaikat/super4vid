#pragma once
// ct/core/arena.h
// Bump allocator over one VirtualAlloc block for per-frame scratch (compositor, UI). Reset() frees everything at once.
// Thread model: one thread at a time. Lifetime: memory is released in the destructor; destructors of objects
// created with New<T> are never run, so T must be trivially destructible.

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ct {

class Arena {
public:
    Arena() = default;
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    bool Init(size_t bytes);                        // VirtualAlloc(MEM_RESERVE | MEM_COMMIT), page-aligned base
    void* Alloc(size_t size, size_t align = 16);    // nullptr when the arena is full; align must be a power of two
    template <class T>
    T* New(size_t n = 1) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        if (n != 0 && sizeof(T) > SIZE_MAX / n) return nullptr;
        void* p = Alloc(sizeof(T) * n, alignof(T) < 16 ? 16 : alignof(T));
        if (!p) return nullptr;
        T* first = static_cast<T*>(p);
        for (size_t i = 0; i < n; ++i) new (first + i) T();
        return first;
    }
    void Reset() { m_used = 0; }
    size_t Used() const { return m_used; }
    size_t Capacity() const { return m_size; }

private:
    uint8_t* m_base = nullptr;
    size_t m_size = 0;
    size_t m_used = 0;
};

} // namespace ct
