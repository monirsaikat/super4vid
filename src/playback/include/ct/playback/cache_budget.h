#pragma once
// ct/playback/cache_budget.h
// Process-wide byte budget shared by every MediaCache's decoded-frame cache (docs/03 section 9: 256 MB default,
// configurable in settings). Caches Add() what they allocate and evict their own frames before exceeding Bytes().
// Thread model: lock-free atomics, any thread.

#include <cstddef>
#include <cstdint>

namespace ct::playback {

struct CacheBudget {
    static constexpr size_t kDefaultBytes = 256ull * 1024 * 1024;
    static void SetBytes(size_t);   // the configured budget (settings); Bytes() adds the temporary extra
    static size_t Bytes();
    static size_t Used();
    static void Add(int64_t delta);
    static bool Fits(size_t bytes) { return Used() + bytes <= Bytes(); }
    // Adds `bytes` to Used() only if the total stays within Bytes() (atomic: concurrent caches cannot overshoot).
    static bool TryReserve(size_t bytes);
    // Temporary extra on top of the configured budget (ExportJob while it runs: +n, then -n). Kept apart from
    // SetBytes so a settings change during an export is neither lost nor undercut when the job gives its bytes back.
    static void AddExtra(int64_t delta);
    static size_t ConfiguredBytes();
};

} // namespace ct::playback
