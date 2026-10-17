// Out-of-line parts of the containers: AudioRing (ring_buffer.h) and Arena (arena.h).
#include <ct/core/arena.h>
#include <ct/core/ring_buffer.h>

#include <ct/core/assert.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ct {

bool AudioRing::Init(size_t frames, int channels) {
    if (frames == 0 || channels <= 0) return false;
    m_data = std::make_unique<float[]>(frames * static_cast<size_t>(channels));
    m_capacity = frames;
    m_channels = channels;
    m_write.store(0, std::memory_order_relaxed);
    m_read.store(0, std::memory_order_release);
    return true;
}

size_t AudioRing::Write(const float* interleaved, size_t frames) {
    if (!m_data || frames == 0) return 0;
    const uint64_t w = m_write.load(std::memory_order_relaxed);
    const uint64_t r = m_read.load(std::memory_order_acquire);
    const size_t free = m_capacity - static_cast<size_t>(w - r);
    const size_t n = std::min(frames, free);
    if (n == 0) return 0;
    const size_t ch = static_cast<size_t>(m_channels);
    const size_t pos = static_cast<size_t>(w % m_capacity);
    const size_t first = std::min(n, m_capacity - pos);
    std::memcpy(m_data.get() + pos * ch, interleaved, first * ch * sizeof(float));
    if (n > first) std::memcpy(m_data.get(), interleaved + first * ch, (n - first) * ch * sizeof(float));
    m_write.store(w + n, std::memory_order_release);
    return n;
}

size_t AudioRing::Read(float* out, size_t frames) {
    if (!m_data || frames == 0) return 0;
    const uint64_t r = m_read.load(std::memory_order_relaxed);
    const uint64_t w = m_write.load(std::memory_order_acquire);
    const size_t n = std::min(frames, static_cast<size_t>(w - r));
    if (n == 0) return 0;
    const size_t ch = static_cast<size_t>(m_channels);
    const size_t pos = static_cast<size_t>(r % m_capacity);
    const size_t first = std::min(n, m_capacity - pos);
    std::memcpy(out, m_data.get() + pos * ch, first * ch * sizeof(float));
    if (n > first) std::memcpy(out + first * ch, m_data.get(), (n - first) * ch * sizeof(float));
    m_read.store(r + n, std::memory_order_release);
    return n;
}

size_t AudioRing::Available() const {
    return static_cast<size_t>(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
}

size_t AudioRing::Free() const { return m_capacity - Available(); }

void AudioRing::Clear() { m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release); }

Arena::~Arena() {
    if (m_base) VirtualFree(m_base, 0, MEM_RELEASE);
}

bool Arena::Init(size_t bytes) {
    if (m_base) {
        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
    }
    m_size = 0;
    m_used = 0;
    if (bytes == 0) return false;
    m_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_base) return false;
    m_size = bytes;
    return true;
}

void* Arena::Alloc(size_t size, size_t align) {
    CT_ASSERT(align != 0 && (align & (align - 1)) == 0);
    if (!m_base || align == 0 || (align & (align - 1)) != 0) return nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(m_base);
    const uintptr_t cur = base + m_used;
    const uintptr_t aligned = (cur + (align - 1)) & ~static_cast<uintptr_t>(align - 1);
    if (aligned < cur) return nullptr;
    const size_t offset = static_cast<size_t>(aligned - base);
    if (offset > m_size || size > m_size - offset) return nullptr;
    m_used = offset + size;
    return m_base + offset;
}

} // namespace ct
