#pragma once
// ct/core/ring_buffer.h
// AudioRing: single-producer / single-consumer ring of interleaved float frames (frame = `channels` samples).
// Thread model: one thread calls Write, one thread calls Read; Available/Free from either. Init and Clear only
// when neither side is active (Clear may also be called by the consumer alone: it drops everything written so far).
// Write/Read are partial: they return how many frames fit / were available. No allocation after Init.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ct {

#pragma warning(push)
#pragma warning(disable : 4324)   // alignas(64) padding keeps the two cursors on separate cache lines

class AudioRing {
public:
    AudioRing() = default;
    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    bool Init(size_t frames, int channels);
    size_t Write(const float* interleaved, size_t frames);
    size_t Read(float* out, size_t frames);
    size_t Available() const;   // frames ready to read
    size_t Free() const;        // frames that can be written
    void Clear();
    size_t CapacityFrames() const { return m_capacity; }
    int Channels() const { return m_channels; }

private:
    std::unique_ptr<float[]> m_data;
    size_t m_capacity = 0;   // frames
    int m_channels = 0;
    alignas(64) std::atomic<uint64_t> m_write{0};   // total frames written
    alignas(64) std::atomic<uint64_t> m_read{0};    // total frames read
};

#pragma warning(pop)

} // namespace ct
