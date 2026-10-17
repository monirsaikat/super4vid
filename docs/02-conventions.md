# 02 · Coding conventions

Read before writing any code. These rules keep the code fast, debuggable, and consistent across agents.

## 1. Language subset

- C++20, MSVC. No exceptions, no RTTI (`dynamic_cast`, `typeid` are forbidden). No `std::iostream`, `std::regex`,
  `std::locale`, `std::function` in hot paths, `std::shared_ptr` in hot paths, `std::thread` (use `ct::Thread`),
  `std::chrono` for media time (use `int64_t` microseconds).
- Allowed std: `<string> <string_view> <vector> <array> <span> <optional> <algorithm> <atomic> <memory> <utility> <cstdint> <cstring> <cmath> <bit> <unordered_map> <map>`.
- COM pointers: `Microsoft::WRL::ComPtr<T>` from `<wrl/client.h>`. Never raw `IUnknown*` ownership.
- Ownership: `std::unique_ptr` or values. Raw pointers are non-owning observers only.
- `constexpr`/`inline` over macros, except the `CT_*` macros below.
- No global constructors with side effects (no COM/MF/D3D calls before `main`).

## 2. Naming

| Thing | Style | Example |
|---|---|---|
| Namespace | `ct`, sub-namespaces per module | `ct::capture::DesktopDuplicator` |
| Types | PascalCase | `FrameQueue`, `Mp4Writer` |
| Functions, methods | PascalCase | `AcquireFrame()`, `WriteSample()` |
| Variables, parameters | snake_case | `frame_index`, `t_us` |
| Members | `m_` + snake_case | `m_device`, `m_frame_count` |
| Constants | `k` + PascalCase | `kMaxTextures`, `kTickHz` |
| Enums | `enum class`, PascalCase values | `enum class RateControl { Quality, Vbr, Cbr };` |
| Macros | `CT_` prefix | `CT_RETURN_IF_FAILED` |
| Files | snake_case | `desktop_duplicator.h/.cpp` |
| Time variables | suffix with unit | `t_us`, `duration_100ns`, `period_ms` |
| Pixel sizes | `width`/`height` in physical pixels; `dip` suffix for DPI-independent | `width`, `size_dip` |

## 3. File layout

```
src/<module>/include/ct/<module>/<name>.h   public header: #pragma once, doc comment, minimal includes
src/<module>/src/<name>.cpp                 implementation; include own header first
src/<module>/src/<name>_internal.h          private helpers (optional)
tests/<module>/<name>_test.cpp              tests
```

Header template:

```cpp
#pragma once
// ct/capture/desktop_duplicator.h
// Captures one monitor with DXGI Desktop Duplication into caller-owned BGRA textures.
// Thread model: all methods are called from the capture thread only, except Stop() (any thread).

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>
#include <wrl/client.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

namespace ct::capture {

class DesktopDuplicator {
public:
    struct Config { int monitor_index = 0; RectI region{}; };
    HRESULT Init(platform::D3DDevice& device, const Config& cfg);
    // Returns S_OK with a new frame, S_FALSE on timeout, DXGI_ERROR_ACCESS_LOST when Reinit() is required.
    HRESULT Acquire(uint32_t timeout_ms, Frame& out);
    void    Release();
    HRESULT Reinit();
private:
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dup;
    // ...
};

} // namespace ct::capture
```

Include order: own header, project headers (`<ct/...>`), Windows headers, std headers. `<windows.h>` is included
only via `<ct/platform/win.h>`, which sets `WIN32_LEAN_AND_MEAN`/`NOMINMAX` and undefines `GetObject`, `DrawText`,
`min`, `max`, `CreateWindow` collisions where needed.

## 4. Errors

Three kinds of functions, by return type:

```cpp
HRESULT  Foo();            // anything that talks to COM / D3D / MF / WASAPI. S_OK, S_FALSE, or a failure code.
bool     Bar(Err* err);    // logic-level functions; on false, *err describes why (user-presentable).
void     Baz();            // cannot fail (asserts internally).
```

Macros in `<ct/core/error.h>`:

```cpp
#define CT_RETURN_IF_FAILED(expr)  do { HRESULT _hr = (expr); if (FAILED(_hr)) { ::ct::LogHr(_hr, #expr, __FILE__, __LINE__); return _hr; } } while (0)
#define CT_LOG_IF_FAILED(expr)     do { HRESULT _hr = (expr); if (FAILED(_hr)) ::ct::LogHr(_hr, #expr, __FILE__, __LINE__); } while (0)
#define CT_RETURN_IF_NULL(ptr, hr) do { if (!(ptr)) { ::ct::LogHr((hr), #ptr " was null", __FILE__, __LINE__); return (hr); } } while (0)
#define CT_RETURN_FALSE_IF(cond, err, msg) do { if (cond) { if (err) err->Set(msg); return false; } } while (0)
```

- `ct::HrToString(HRESULT)` formats `0x8876086C (DXGI_ERROR_ACCESS_LOST)`: known DXGI/MF/WASAPI codes by name, else `FormatMessage`.
- `struct Err { std::string message; HRESULT hr = S_OK; void Set(std::string_view m, HRESULT h = S_OK); }`.
- Never ignore a `FAILED(hr)` silently. Either return it, log it, or document why `S_FALSE`/failure is expected.
- Assertions: `CT_ASSERT(cond)` (Debug only, `__debugbreak()`), `CT_VERIFY(cond)` (evaluated in Release too, logs on failure), `CT_FATAL("msg")` (writes a crash dump and terminates; use only for programming errors that make continuing unsafe).

## 5. Logging and tracing

```cpp
CT_LOG_TRACE("acquire took %.2f ms", ms);   // compiled out in Release unless CT_LOG=trace at runtime? No: always compiled, filtered at runtime by level.
CT_LOG_DEBUG(...), CT_LOG_INFO(...), CT_LOG_WARN(...), CT_LOG_ERROR(...)
```

- printf-style, UTF-8. `%s` takes `const char*`; use `.c_str()`. Wide strings: `%ls`.
- The logger is lock-free for producers (SPSC per thread -> one writer thread), never blocks a capture/audio thread, and flushes on `CT_LOG_ERROR`, on crash, and every 500 ms.
- Each line: `2026-10-17 12:03:44.123 [I] [capture ] desktop_duplicator.cpp:142 message`.
- Hot paths use `CT_TRACE_SCOPE("FrameConvert")` (TraceLogging ETW event + optional in-process timing) instead of log lines. Never log per frame at INFO.

## 6. Threads and synchronization

- Every thread is created through `ct::Thread` with a name (shown in debuggers via `SetThreadDescription`), a priority, and an optional MMCSS task (`"Capture"`, `"Pro Audio"`, `"Playback"`). Thread table lives in `docs/03-architecture.md`.
- **One owner per resource.** A resource (decoder, duplicator, encoder, file) is touched by exactly one thread. Other threads talk to it through `SpscQueue<T>` or by posting a message to the UI thread.
- Shared state that is rarely written: `ct::SrwLock` (`SRWLOCK` wrapper) with `Shared/Exclusive` guards. Flags and counters: `std::atomic` with explicit memory orders (`relaxed` for stats, `acquire/release` for handoff).
- No mutex, no allocation, no logging at INFO, no COM calls other than the owned object on the capture, encode and audio threads after `Start()`. Pre-allocate in `Init()`.
- Waiting: `WaitForMultipleObjects` on events/timers, never `Sleep(1)` loops. High-resolution waits use `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`.
- The UI thread never blocks longer than 2 ms. Anything slower (file IO, MF object creation, device enumeration) goes to the IO thread pool (`ct::Async::Run(lambda)`) and completes via a posted message.
- COM: every thread that touches COM calls `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` in its entry function (the UI thread uses `COINIT_APARTMENTTHREADED` because of shell dialogs). `MFStartup` once per process (in `ct::media::MfRuntime`).

## 7. Time

- Internal unit: `int64_t` microseconds (`t_us`). Session epoch = QPC value captured at `RecordingSession::Start`.
- `ct::Clock::NowUs()` = `(qpc - epoch) * 1'000'000 / freq` computed with 128-bit intermediate (`_umul128` / `_udiv128`) to avoid overflow.
- Media Foundation: `t_100ns = t_us * 10`. Convert only inside `ct_media`.
- Frame timestamps are **tick-aligned**: frame `n` at `n * 1'000'000 / fps` (integer division with carried remainder, so 60 fps gives 16666, 16667, 16667, ... and no drift). Audio timestamps come from the device QPC position.

## 8. Memory

- Pools for every per-frame object: textures (`TexturePool`), samples, audio packets (`ObjectPool<AudioPacket>`), event records (ring buffer). Pool exhaustion is a counted drop, never an allocation.
- Decoded frame cache per media: hard cap in bytes (default 256 MB total across media, LRU).
- `Arena` for per-frame scratch in the compositor and UI (reset each frame).
- Large buffers via `VirtualAlloc` (page-aligned), small objects via CRT heap (Segment Heap is enabled in the manifest).
- Debug builds: `_CrtSetDbgFlag(_CRTDBG_LEAK_CHECK_DF)` and D3D `ReportLiveDeviceObjects` at exit; both must be clean.

## 9. Strings and paths

- UTF-8 `std::string` internally. Convert at the Win32 boundary with `ct::Utf8ToWide` / `ct::WideToUtf8`. Use the `W` versions of Win32 APIs.
- Paths: `std::wstring` only inside `ct::fs`; everywhere else UTF-8. Always prefix long paths with `\\?\` inside `ct::fs` when the length exceeds 240 characters.
- User-visible text lives in `src/ui/include/ct/ui/strings.h` as `constexpr const char*` constants.

## 10. Performance rules

- Measure before and after: every optimization commit states the metric and both numbers.
- GPU: no `Map` of default textures on the hot path; readback only in tests and thumbnails via staging textures with a 2-frame delay.
- No per-frame shader/state object creation; all pipeline state objects are created in `Init()`.
- Avoid `std::string` formatting per frame; UI text that changes per frame uses stack buffers and `snprintf`.
- Branch on `m_stats.enabled` before computing statistics that cost more than a few instructions.
- `[[likely]]`/`[[unlikely]]` on error branches in loops that run per frame or per audio packet.

## 11. Git

- Commit per task: `P04: NV12 texture pool with reuse check (task 4.3)`. Body: what changed, how verified (test names, measured numbers).
- Never commit `out/`, `*.user`, recordings, or anything larger than 2 MB except `assets/icon.ico` and test media listed in `tests/data/README.md`.
- `main` always builds in both configs. Work on `main` directly unless the user set up branches.

## 12. Comments and docs

- Each public header starts with a 2-5 line comment: what, thread model, lifetime.
- Explain **why** in comments, not what. Link the Microsoft Learn page when an API has non-obvious constraints (e.g. "DXGI dup must be recreated after DXGI_ERROR_ACCESS_LOST: https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api").
- No commented-out code. No `TODO` without a `PROGRESS.md` item.
