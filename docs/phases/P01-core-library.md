# P01 · Core library

**Goal:** the dependency-free toolbox every later phase uses: strings/UTF, QPC time, lock-free SPSC queue, pools,
ring buffer, arena, threads with MMCSS, events/locks, JSON, filesystem, settings, tracing/stats. Fully unit-tested.

**Read first:** `docs/02-conventions.md` sections 4-9, `docs/04-file-formats.md` section 4 (settings), `docs/06` section 1.
**Depends on:** P00. **Size:** M (about 3,000 lines incl. tests).

## Deliverables

```
src/core/include/ct/core/{strings.h,time.h,spsc_queue.h,object_pool.h,ring_buffer.h,arena.h,thread.h,sync.h,async.h,json.h,fs.h,config.h,trace.h,stats.h,log.h(upgraded)}
src/core/src/{strings.cpp,time.cpp,thread.cpp,async.cpp,json.cpp,fs.cpp,config.cpp,trace.cpp,stats.cpp,log.cpp}
tests/core/{strings_test.cpp,time_test.cpp,containers_test.cpp,thread_test.cpp,json_test.cpp,fs_test.cpp,config_test.cpp,bench_test.cpp}
```

## Interfaces (exact signatures; implement all)

### 1.1 strings.h
```cpp
namespace ct {
std::wstring Utf8ToWide(std::string_view s);                 // MultiByteToWideChar(CP_UTF8, 0, ...); invalid bytes -> U+FFFD
std::string  WideToUtf8(std::wstring_view s);
std::string  Format(const char* fmt, ...);                   // two-pass vsnprintf; never truncates
std::string  FormatV(const char* fmt, va_list);
void         AppendFormat(std::string& out, const char* fmt, ...);
std::vector<std::string_view> Split(std::string_view s, char sep, bool keep_empty = false);
std::string_view Trim(std::string_view s);
bool StartsWith(std::string_view s, std::string_view p);  bool EndsWith(std::string_view s, std::string_view p);
bool IEquals(std::string_view a, std::string_view b);       // ASCII case-insensitive
std::string ToLowerAscii(std::string_view s);
std::string PathJoin(std::string_view a, std::string_view b);   // handles trailing/leading separators, uses '\\'
std::string_view PathExt(std::string_view p);  std::string_view PathStem(std::string_view p);  std::string_view PathParent(std::string_view p);  std::string_view PathName(std::string_view p);
std::string Uuid4();                                          // BCryptGenRandom 16 bytes -> 8-4-4-4-12 lowercase
std::string FormatTimecode(TimeUs t, int fps_num, int fps_den, bool with_frames); // "01:02:03.12" or "01:02:03.123"
std::string FormatBytes(uint64_t bytes);                      // "1.5 MB"
uint32_t Fnv1a32(const void* data, size_t len);  uint64_t Fnv1a64(const void* data, size_t len);
}
```

### 1.2 time.h
```cpp
namespace ct {
struct Clock {
    static int64_t QpcNow();                      // QueryPerformanceCounter
    static int64_t QpcFrequency();                // cached
    static TimeUs  QpcToUs(int64_t qpc, int64_t epoch_qpc);   // (qpc-epoch)*1e6/freq using _umul128/_udiv128, exact, no overflow for 100 years
    static int64_t UsToQpc(TimeUs us, int64_t epoch_qpc);
    static TimeUs  NowUs();                       // since process start
    static int64_t ProcessStartQpc();
};
TimeUs FrameTimeUs(int64_t frame_index, int fps_num, int fps_den);    // exact: index * 1e6 * den / num (128-bit intermediate)
int64_t FrameIndexAt(TimeUs t, int fps_num, int fps_den);            // floor
class HighResTimer {                             // CreateWaitableTimerExW(... CREATE_WAITABLE_TIMER_HIGH_RESOLUTION ...); falls back to the normal timer
public:
    bool Init();  HANDLE Handle() const;
    bool ArmAbsoluteQpc(int64_t qpc);            // SetWaitableTimerEx with negative relative 100ns computed from now
    bool WaitUntilQpc(int64_t qpc, HANDLE abort_event = nullptr);   // returns false if aborted; spins the last 200 µs for accuracy
};
class Stopwatch { public: void Start(); double ElapsedMs() const; TimeUs ElapsedUs() const; };
struct UtcTime { static std::string NowIso8601(); static std::string NowFileStamp(); /* "2026-10-17 12-03-44" */ static std::string LocalFileStamp(); };
}
```

### 1.3 containers
```cpp
// spsc_queue.h — single producer, single consumer, fixed capacity (power of two), wait-free.
template <class T, size_t CapacityPow2> class SpscQueue {
public:
    bool Push(T&& v);  bool Push(const T& v);     // false when full (caller counts a drop)
    bool Pop(T& out);                             // false when empty
    size_t Size() const;  bool Empty() const;  static constexpr size_t Capacity();
private:
    alignas(64) std::atomic<size_t> m_head{0};  alignas(64) std::atomic<size_t> m_tail{0};
    alignas(64) std::array<T, CapacityPow2> m_slots{};
};
// object_pool.h — fixed set of objects; one thread acquires, one (possibly other) thread releases.
template <class T> class ObjectPool {
public:
    template <class Factory> bool Init(size_t count, Factory make);   // make(size_t index) -> T
    T* Acquire(uint32_t* index_out = nullptr);   // nullptr when exhausted
    void Release(uint32_t index);
    T& At(uint32_t index);  size_t Count() const;  size_t InUse() const;
};
// ring_buffer.h — SPSC float ring for audio (interleaved frames)
class AudioRing { public: bool Init(size_t frames, int channels); size_t Write(const float* interleaved, size_t frames); size_t Read(float* out, size_t frames); size_t Available() const; size_t Free() const; void Clear(); };
// arena.h
class Arena { public: bool Init(size_t bytes /* VirtualAlloc */); void* Alloc(size_t size, size_t align = 16); template<class T> T* New(size_t n = 1); void Reset(); size_t Used() const; };
```

### 1.4 thread.h, sync.h, async.h
```cpp
namespace ct {
class Thread {
public:
    struct Options { const char* name = "ct-worker"; int priority = 0 /* THREAD_PRIORITY_NORMAL */; const wchar_t* mmcss_task = nullptr /* L"Capture", L"Pro Audio", L"Playback" */; bool com_mta = true; };
    bool Start(const Options&, std::function<void()> body);   // sets SetThreadDescription, priority, AvSetMmThreadCharacteristicsW inside the thread, CoInitializeEx(MTA) if com_mta
    bool Join(uint32_t timeout_ms = INFINITE);                 // false on timeout (caller CT_FATALs)
    bool IsRunning() const;  HANDLE Handle() const;  uint32_t Id() const;
};
class Event { public: explicit Event(bool manual_reset = false, bool initial = false); void Set(); void Reset(); bool Wait(uint32_t ms = INFINITE) const; HANDLE Handle() const; };
class SrwLock { public: void LockExclusive(); void UnlockExclusive(); void LockShared(); void UnlockShared(); };
class ExclusiveGuard { explicit ExclusiveGuard(SrwLock&); ~ExclusiveGuard(); };  class SharedGuard { ... };
struct Async {
    static void Init(int io_threads = 2);                       // "ct-io-pool-N"
    static void Run(std::function<void()> job);                 // IO pool
    static void SetUiPoster(void (*post)(void* user, std::function<void()>*), void* user);   // App installs: PostMessageW(hwnd, WM_CT_INVOKE, 0, (LPARAM)fn)
    static void PostToUi(std::function<void()> fn);
    static void Shutdown();
};
}
```

### 1.5 json.h
```cpp
namespace ct {
class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    static bool Parse(std::string_view text, Json& out, std::string* error = nullptr);   // error: "line 3 col 14: expected ','"
    std::string Dump(int indent = 2) const;                      // indent 0 = compact; numbers: integers exact, doubles shortest round-trip ("%.17g" then trim)
    Type GetType() const;  bool IsNull() const; bool IsBool() const; bool IsNumber() const; bool IsString() const; bool IsArray() const; bool IsObject() const;
    bool AsBool(bool def = false) const;  double AsNumber(double def = 0) const;  int64_t AsInt64(int64_t def = 0) const;  int AsInt(int def = 0) const;  const std::string& AsString() const;  std::string AsString(std::string_view def) const;
    const Json& operator[](std::string_view key) const;          // static Null for missing
    const Json& operator[](size_t index) const;
    Json& operator[](std::string_view key);                      // creates
    bool Has(std::string_view key) const;  size_t Size() const;
    Json& Push(Json v);  Json& Set(std::string_view key, Json v);  bool Erase(std::string_view key);
    const std::vector<std::pair<std::string, Json>>& Items() const;   // insertion order preserved
    const std::vector<Json>& Elements() const;
    static Json Object();  static Json Array();  Json() = default;  Json(bool);  Json(double);  Json(int64_t);  Json(int);  Json(std::string);  Json(std::string_view);  Json(const char*);
    // helpers used by config/project code
    template <class T> T Get(std::string_view key, T def) const;
};
}
```
Parser requirements: RFC 8259, UTF-8 input, `\uXXXX` incl. surrogate pairs, depth limit 256, numbers via `strtod` with `_locale_t` C locale (never the user locale), rejects trailing commas, accepts BOM. Dump escapes `"\\\b\f\n\r\t` and control chars; non-ASCII emitted raw UTF-8.

### 1.6 fs.h
```cpp
namespace ct::fs {
bool ReadAll(std::string_view path, std::string& out);
bool WriteAtomic(std::string_view path, std::string_view data);   // write <path>.tmp, FlushFileBuffers, MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)
bool Exists(std::string_view path);  bool IsDirectory(std::string_view path);
bool CreateDirectories(std::string_view path);
bool Delete(std::string_view path);  bool DeleteTree(std::string_view dir);
struct Entry { std::string name; bool is_dir; uint64_t size; int64_t mtime_unix; };
bool List(std::string_view dir, std::vector<Entry>& out);
uint64_t FileSize(std::string_view path);  uint64_t FreeSpaceBytes(std::string_view path_on_volume);
std::string TempPath(std::string_view ext);
std::wstring ToWinPath(std::string_view utf8);   // adds \\?\ when > 240 chars, normalizes '/' to '\\'
std::string Absolute(std::string_view path);  std::string Relative(std::string_view base_dir, std::string_view path);  bool SameVolume(std::string_view a, std::string_view b);
bool ShowInExplorer(std::string_view path);      // SHOpenFolderAndSelectItems
bool OpenWithShell(std::string_view path);       // ShellExecuteW "open"
}
```

### 1.7 config.h
`struct Settings` mirroring `docs/04` section 4 with typed fields and defaults; `static Settings Defaults()`, `bool Load(std::string_view path, Err*)` (missing file = defaults; unknown keys preserved in `m_extra` and written back), `bool Save(std::string_view path, Err*) const`, `Json ToJson() const`, `static Settings FromJson(const Json&)`, `int Version() const` and `Migrate()` hook. Hotkey strings parse to `{ UINT modifiers; UINT vk; }` through `bool ParseHotkey(std::string_view, Hotkey&)` / `std::string HotkeyToString(Hotkey)`.

### 1.8 trace.h, stats.h, log.h upgrade
```cpp
// trace.h — TraceLogging provider "Camtesia", GUID generated once and pasted as a constant
namespace ct { struct Trace { static void Register(); static void Unregister(); static void Event(const char* name, int64_t value = 0); }; }
#define CT_TRACE_SCOPE(name) ::ct::TraceScope CT_CONCAT(_ts_, __LINE__)(name)   // TraceLoggingWrite start/stop + optional Stats timing
// stats.h — named counters/gauges, lock-free, dumped once per second by --stats and by the F11 overlay
namespace ct { struct Stats {
    struct Counter { std::atomic<uint64_t> v{0}; void Add(uint64_t n = 1); };
    struct Gauge   { std::atomic<double> v{0}; void Set(double); };
    struct Timing  { void AddUs(int64_t us); double AvgMs() const; double MaxMs() const; void Reset(); };
    static Counter& C(const char* name);  static Gauge& G(const char* name);  static Timing& T(const char* name);   // interned by name, created once
    static void Dump(std::string& out);   // "capture.frames 36000  capture.dropped 0  convert.ms avg 0.42 max 1.3 ..."
    static void ResetTimings();
}; }
```
Log upgrade: producers append into a 1 MB ring (SRW-protected memcpy only, no file IO on the caller's thread); a `ct-log` thread flushes every 500 ms, on Warn/Error wakeups, on `Flush()`. `Tick()` becomes a no-op kept for API stability. Rotation: when the file exceeds 10 MB start a new one, keep 5.

## Tests (all `CT_TEST`, no hardware)
- strings: UTF-8 <-> UTF-16 with emoji, invalid sequences, empty; `Format` > 1 KB; `Split` edge cases; path helpers with mixed separators; `Uuid4` format and 10,000 uniqueness; `FormatTimecode` at 29.97 fps.
- time: `QpcToUs` monotonic and exact against `double` for 1e12 ranges; `FrameTimeUs(n, 60, 1)` sums to exactly `n * 1e6 / 60` rounding pattern 16666/16667 with no drift at n = 3,600,000; `HighResTimer::WaitUntilQpc` wakes within 500 µs for 1,000 waits of 2 ms (report p99).
- containers: SPSC with producer/consumer threads, 10 M items, order and count preserved; pool exhaustion returns null; `AudioRing` wrap-around; `Arena` alignment.
- thread: name visible (`GetThreadDescription`), MMCSS task applied (`AvSetMmThreadCharacteristics` returns a handle; skip when it fails in CI), `Join` timeout path.
- json: round-trip of `docs/04` samples byte-identical after `Parse -> Dump -> Parse -> Dump`; unicode escapes; 1e308, -0, 9007199254740993 (int64 exactness); deep nesting 255 ok / 257 rejected; 1,000 random mutated documents never crash.
- fs: atomic write survives a simulated failure (write to a read-only path returns false, original intact); long path (300 chars) create/read/delete; `Relative`/`Absolute`.
- config: defaults -> save -> load equality; unknown keys preserved; hotkey parse/format.
- bench (`--bench`): SPSC push/pop >= 20 M ops/s single-threaded ping; JSON parse of a 1 MB document < 20 ms; `Format` 1 M calls.

## Acceptance (1.9)
`ct_core_tests` passes in Debug and Release with `--repeat 3`; bench numbers written to `PROGRESS.md`; no `/W4` warnings; `log.cpp` never blocks a caller for more than the memcpy (verified by a test that logs 100,000 lines from a time-critical thread in < 50 ms).

## Pitfalls
- `QueryPerformanceFrequency` is 10 MHz on most modern Windows machines but not guaranteed; never hard-code it.
- `vsnprintf` with `%s` and `std::string` is undefined; the tests must use `.c_str()`; add `__attribute__`-like checking is unavailable on MSVC, so review call sites.
- `SpscQueue` with non-trivial `T` must move out and destroy the slot on `Pop`.
- `SetWaitableTimerEx` due time is **negative** for relative; absolute uses FILETIME, which is not QPC: compute relative from `QpcNow()` at arm time.
- `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` can fail across volumes; `WriteAtomic` always writes the temp file into the destination directory.
- `strtod` honours the C locale set by `setlocale`; use `_strtod_l` with `_create_locale(LC_NUMERIC, "C")` created once.
