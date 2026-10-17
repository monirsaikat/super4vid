#define CT_LOG_CATEGORY "events"
#include <ct/recorder/event_recorder.h>

#include <ct/capture/cursor_tracker.h>
#include <ct/core/assert.h>
#include <ct/core/log.h>
#include <ct/core/spsc_queue.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/events/ctev.h>
#include <ct/platform/hooks_thread.h>
#include <ct/recorder/keys.h>
#include <ct/recorder/typing.h>

#include <dwmapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ct::recorder {

namespace {

enum class Kind : uint8_t { None, Button, Wheel, Key, Focus, Marker, Pause };

// Fixed-size record pushed by the hook procedures (no allocation). Keys that are not recorded (shortcuts-only
// mode) are still pushed as anonymous key-downs (vk 0) so TYPING bursts can be derived without their content.
struct RawEvent {
    TimeUs t_us = 0;
    Kind kind = Kind::None;
    uint8_t button = 0, down = 0, mods = 0, extended = 0;
    uint8_t record_key = 0;   // write a KEY record
    uint8_t typing_key = 0;   // non-modifier key-down (TYPING derivation)
    uint8_t paused = 0;
    uint16_t vk = 0, scancode = 0;
    int16_t delta_v = 0, delta_h = 0;
    int32_t x = 0, y = 0;
    uint64_t hwnd = 0;
};

struct Control {
    RawEvent ev;
    uint32_t marker_kind = 0;
    std::string label;
};

uint32_t FlagsFor(KeystrokeMode mode) {
    switch (mode) {
    case KeystrokeMode::Off: return 0;
    case KeystrokeMode::ShortcutsOnly: return events::kFlagKeystrokes;
    case KeystrokeMode::All: return events::kFlagKeystrokes | events::kFlagAllKeys;
    }
    return 0;
}

} // namespace

struct EventRecorder::Impl final : platform::HooksHandler {
    Config config;
    const SessionClock* clock = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> active{false};

    // ct-hooks -> ct-events-io
    SpscQueue<RawEvent, 65536> ring;
    SpscQueue<capture::CursorSample, 4096> samples;
    SpscQueue<capture::CursorShape, 64> shapes;
    capture::CursorTracker tracker;
    std::atomic<uint64_t> ring_dropped{0};
    bool shortcut_down[256] = {};   // hooks thread: key-downs written in shortcuts-only mode (their ups follow)

    // controlling thread -> ct-events-io
    SrwLock control_lock;
    std::vector<Control> controls;

    platform::HooksThread hooks;
    Thread io;
    Event stop{true, false};
    Event wake{false, false};
    Event flush_done{false, false};
    std::atomic<bool> flush_requested{false};

    // ct-events-io only
    events::EventWriter writer;
    TypingDetector typing;
    bool caps_on = false;
    TimeUs last_t = 0;
    std::vector<RawEvent> batch_raw;
    std::vector<capture::CursorSample> batch_samples;
    std::vector<Control> batch_controls;
    std::vector<capture::CursorShape> pending_shapes;
    std::wstring text_buf;

    Live live;
    mutable SrwLock label_lock;
    char last_label[32] = {};
    std::atomic<uint64_t> records{0}, bytes{0}, typing_bursts{0};
    std::atomic<uint32_t> flush_failures{0};

    // ---- ct-hooks (HooksHandler) ----
    void Push(const RawEvent& e) {
        if (!ring.Push(e)) ring_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    void OnMouseHook(UINT msg, const MSLLHOOKSTRUCT& info) override {
        if (!active.load(std::memory_order_acquire)) return;
        RawEvent e;
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: e.kind = Kind::Button; e.button = 0; e.down = msg == WM_LBUTTONDOWN; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: e.kind = Kind::Button; e.button = 1; e.down = msg == WM_RBUTTONDOWN; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: e.kind = Kind::Button; e.button = 2; e.down = msg == WM_MBUTTONDOWN; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            e.kind = Kind::Button;
            e.button = HIWORD(info.mouseData) == XBUTTON2 ? 4 : 3;
            e.down = msg == WM_XBUTTONDOWN;
            break;
        case WM_MOUSEWHEEL: e.kind = Kind::Wheel; e.delta_v = static_cast<int16_t>(HIWORD(info.mouseData)); break;
        case WM_MOUSEHWHEEL: e.kind = Kind::Wheel; e.delta_h = static_cast<int16_t>(HIWORD(info.mouseData)); break;
        default: return;   // WM_MOUSEMOVE: the tracker polls the position
        }
        e.t_us = clock->NowMediaUs();
        e.x = info.pt.x - config.region.x;
        e.y = info.pt.y - config.region.y;
        Push(e);
    }

    void OnKeyboardHook(UINT msg, const KBDLLHOOKSTRUCT& info) override {
        if (!active.load(std::memory_order_acquire)) return;
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        const uint16_t vk = static_cast<uint16_t>(info.vkCode & 0xFF);
        RawEvent e;
        e.kind = Kind::Key;
        e.down = down;
        e.typing_key = down && !keys::IsModifierVk(vk);
        const uint8_t mods = keys::AsyncModifiers();
        if (config.keystrokes == KeystrokeMode::All) {
            e.record_key = 1;
        } else {   // ShortcutsOnly (Off installs no keyboard hook)
            if (down) {
                e.record_key = keys::IsShortcut(vk, mods);
                shortcut_down[vk] = e.record_key != 0;
            } else {
                e.record_key = shortcut_down[vk];
                shortcut_down[vk] = false;
            }
        }
        if (!e.record_key && !e.typing_key) return;
        if (e.record_key) {
            e.vk = vk;
            e.scancode = static_cast<uint16_t>(info.scanCode);
            e.mods = mods;
            e.extended = (info.flags & LLKHF_EXTENDED) ? 1 : 0;
        }
        e.t_us = clock->NowMediaUs();
        Push(e);
    }

    void OnForeground(HWND hwnd) override {
        if (!active.load(std::memory_order_acquire) || !config.focus) return;
        RawEvent e;
        e.kind = Kind::Focus;
        e.hwnd = reinterpret_cast<uint64_t>(hwnd);
        e.t_us = clock->NowMediaUs();
        Push(e);
    }

    void OnTimer() override {
        if (!active.load(std::memory_order_acquire)) return;
        tracker.Poll(clock->NowMediaUs(), config.region, samples, shapes);
    }

    // ---- controlling thread ----
    void PushControl(Control c) {
        ExclusiveGuard g(control_lock);
        controls.push_back(std::move(c));
    }

    // ---- ct-events-io ----
    void Note(TimeUs t) {
        if (t > last_t) last_t = t;
    }

    void WriteShape(const capture::CursorShape& s, TimeUs t) {
        if (writer.HasShape(s.id)) return;
        const uint32_t id = writer.CursorShape(t, s.id, s.w, s.h, s.hot_x, s.hot_y,
                                               reinterpret_cast<const uint8_t*>(s.bgra_premul.data()));
        if (id) records.fetch_add(1, std::memory_order_relaxed);
    }

    void WriteTypingEdges(const TypingDetector::Edge* edges, int n) {
        for (int i = 0; i < n; ++i) {
            writer.Typing(edges[i].t_us, edges[i].started);
            records.fetch_add(1, std::memory_order_relaxed);
            if (edges[i].started) typing_bursts.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void WriteSample(const capture::CursorSample& s) {
        if (s.shape_id != 0 && !writer.HasShape(s.shape_id)) {
            // The shape was pushed before the sample; it may still be in the queue or in the pending list.
            capture::CursorShape sh;
            while (shapes.Pop(sh)) pending_shapes.push_back(std::move(sh));
            for (auto& p : pending_shapes) if (p.id == s.shape_id) WriteShape(p, s.t_us);
        }
        writer.CursorMove(s.t_us, s.x, s.y, s.shape_id, s.visible);
        records.fetch_add(1, std::memory_order_relaxed);
        live.cursor_x.store(s.x, std::memory_order_relaxed);
        live.cursor_y.store(s.y, std::memory_order_relaxed);
        Note(s.t_us);
    }

    void WriteFocus(TimeUs t, HWND hwnd) {
        events::WindowFocusPayload p;
        p.hwnd = reinterpret_cast<uint64_t>(hwnd);
        if (IsWindow(hwnd)) {
            wchar_t title[256] = {};
            const int n = GetWindowTextW(hwnd, title, 256);
            if (n > 0) events::CopyLabel(p.title, sizeof(p.title), WideToUtf8(std::wstring_view(title, static_cast<size_t>(n))));
            RECT rc{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) GetWindowRect(hwnd, &rc);
            p.x = rc.left - config.region.x;
            p.y = rc.top - config.region.y;
            p.w = rc.right - rc.left;
            p.h = rc.bottom - rc.top;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
                wchar_t path[MAX_PATH] = {};
                DWORD len = MAX_PATH;
                if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
                    const std::string utf8 = WideToUtf8(std::wstring_view(path, len));
                    events::CopyLabel(p.exe, sizeof(p.exe), PathName(utf8));
                }
                CloseHandle(proc);
            }
        }
        writer.WindowFocus(t, p);
        records.fetch_add(1, std::memory_order_relaxed);
    }

    void WriteKey(const RawEvent& e) {
        events::KeyPayload k;
        k.vk = e.vk;
        k.scancode = e.scancode;
        k.down = e.down;
        k.modifiers = e.mods;
        k.extended = e.extended;
        keys::LabelFor(e.vk, e.scancode, e.extended != 0, k.label);
        if (e.vk == VK_CAPITAL && e.down) caps_on = !caps_on;
        if (config.keystrokes == KeystrokeMode::All && e.down && !(e.mods & (keys::kCtrl | keys::kAlt | keys::kWin)) &&
            !keys::IsModifierVk(e.vk)) {
            BYTE state[256] = {};
            if (e.mods & keys::kShift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
            if (caps_on) state[VK_CAPITAL] = 0x01;
            k.utf32_char = keys::CharFor(e.vk, e.scancode, state);
        }
        writer.Key(e.t_us, k);
        records.fetch_add(1, std::memory_order_relaxed);
        if (e.down) {
            ExclusiveGuard g(label_lock);
            std::memcpy(last_label, k.label, sizeof(last_label));
        }
    }

    void WriteRaw(const RawEvent& e) {
        switch (e.kind) {
        case Kind::Button:
            writer.MouseButton(e.t_us, static_cast<events::Button>(e.button), e.down != 0, e.x, e.y);
            records.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Wheel:
            writer.MouseWheel(e.t_us, e.delta_v, e.delta_h, e.x, e.y);
            records.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Key: {
            if (e.typing_key) {
                TypingDetector::Edge edges[2];
                WriteTypingEdges(edges, typing.OnKeyDown(e.t_us, edges));
            }
            if (e.record_key) WriteKey(e);
            break;
        }
        case Kind::Focus:
            WriteFocus(e.t_us, reinterpret_cast<HWND>(e.hwnd));
            break;
        default:
            break;
        }
        Note(e.t_us);
    }

    void WriteControl(const Control& c) {
        switch (c.ev.kind) {
        case Kind::Marker:
            writer.Marker(c.ev.t_us, c.marker_kind, c.label);
            records.fetch_add(1, std::memory_order_relaxed);
            break;
        case Kind::Pause: {
            if (c.ev.paused) {
                TypingDetector::Edge edge[1];
                WriteTypingEdges(edge, typing.Finish(edge));
            }
            writer.Pause(c.ev.t_us, c.ev.paused != 0);
            records.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case Kind::Focus:
            WriteFocus(c.ev.t_us, reinterpret_cast<HWND>(c.ev.hwnd));
            break;
        default:
            break;
        }
        Note(c.ev.t_us);
    }

    // Moves everything queued so far into the writer, merged by time (each source is already in time order).
    void Drain() {
        capture::CursorShape sh;
        while (shapes.Pop(sh)) pending_shapes.push_back(std::move(sh));
        RawEvent e;
        while (ring.Pop(e)) batch_raw.push_back(e);
        capture::CursorSample s;
        while (samples.Pop(s)) batch_samples.push_back(s);
        {
            ExclusiveGuard g(control_lock);
            for (auto& c : controls) batch_controls.push_back(std::move(c));
            controls.clear();
        }
        size_t ir = 0, is = 0, ic = 0;
        constexpr TimeUs kEnd = INT64_MAX;
        for (;;) {
            const TimeUs tr = ir < batch_raw.size() ? batch_raw[ir].t_us : kEnd;
            const TimeUs ts = is < batch_samples.size() ? batch_samples[is].t_us : kEnd;
            const TimeUs tc = ic < batch_controls.size() ? batch_controls[ic].ev.t_us : kEnd;
            if (tr == kEnd && ts == kEnd && tc == kEnd) break;
            if (tc <= tr && tc <= ts) WriteControl(batch_controls[ic++]);
            else if (ts <= tr) WriteSample(batch_samples[is++]);
            else WriteRaw(batch_raw[ir++]);
        }
        // Shapes not referenced yet (a sample still in flight) are written now; the move that uses them comes later.
        for (auto& p : pending_shapes) WriteShape(p, last_t);
        pending_shapes.clear();
        batch_raw.clear();
        batch_samples.clear();
        batch_controls.clear();
    }

    void DoFlush() {
        if (!writer.Flush()) {
            flush_failures.fetch_add(1, std::memory_order_relaxed);
        }
        bytes.store(writer.BytesWritten(), std::memory_order_relaxed);
        live.records.store(records.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void IoLoop() {
        const int64_t flush_period = Clock::QpcFrequency();   // 1 s
        int64_t next_flush = Clock::QpcNow() + flush_period;
        HANDLE handles[2] = {stop.Handle(), wake.Handle()};
        for (;;) {
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, 100);
            const bool stopping = r == WAIT_OBJECT_0;
            Drain();
            if (active.load(std::memory_order_acquire)) {
                TypingDetector::Edge edge[1];
                WriteTypingEdges(edge, typing.Tick(clock->NowMediaUs(), edge));
            }
            const bool requested = flush_requested.exchange(false);
            const int64_t now = Clock::QpcNow();
            if (requested || now >= next_flush || stopping) {
                DoFlush();
                next_flush = now + flush_period;
                if (requested) flush_done.Set();
            }
            hooks.CheckWatchdog();
            if (stopping) break;
        }
        TypingDetector::Edge edge[1];
        WriteTypingEdges(edge, typing.Finish(edge));
        DoFlush();
        if (!writer.Close()) flush_failures.fetch_add(1, std::memory_order_relaxed);
        bytes.store(writer.BytesWritten(), std::memory_order_relaxed);
    }
};

EventRecorder::EventRecorder() : m(std::make_unique<Impl>()) {}
EventRecorder::~EventRecorder() { Stop(); }

bool EventRecorder::Start(const Config& config, std::string_view events_path, const SessionClock& clock, Err* err) {
    if (m->running.load()) {
        if (err) err->Set("event recorder already running");
        return false;
    }
    // A fresh Impl per session: queues, tracker, detector and counters start empty.
    m = std::make_unique<Impl>();
    Impl& s = *m;
    s.config = config;
    if (s.config.cursor_hz < 1) s.config.cursor_hz = 120;
    s.clock = &clock;

    events::EventHeader header;
    header.epoch_qpc = clock.epoch_qpc;
    header.qpc_frequency = Clock::QpcFrequency();
    header.region_x = config.region.x;
    header.region_y = config.region.y;
    header.region_w = config.region.w;
    header.region_h = config.region.h;
    header.flags = FlagsFor(config.keystrokes);
    if (!s.writer.Open(events_path, header, err)) return false;
    s.caps_on = (GetKeyState(VK_CAPITAL) & 1) != 0;
    s.tracker.Reset();
    s.active.store(true, std::memory_order_release);

    Thread::Options opt;
    opt.name = "ct-events-io";
    if (!s.io.Start(opt, [&s] { s.IoLoop(); })) {
        s.writer.Close();
        if (err) err->Set("could not start the events thread");
        return false;
    }
    platform::HooksThread::Config hc;
    hc.mouse = true;
    hc.keyboard = config.keystrokes != KeystrokeMode::Off;
    hc.focus = config.focus;
    hc.timer_hz = s.config.cursor_hz;
    hc.swallow_tag = config.test_swallow_tag;
    if (!s.hooks.Start(hc, &s)) {
        s.active.store(false);
        s.stop.Set();
        s.io.Join(5000);
        if (err) err->Set("could not install the input hooks");
        return false;
    }
    s.running.store(true, std::memory_order_release);
    if (config.focus) {
        if (HWND fg = GetForegroundWindow()) {
            Control c;
            c.ev.kind = Kind::Focus;
            c.ev.hwnd = reinterpret_cast<uint64_t>(fg);
            c.ev.t_us = clock.NowMediaUs();
            s.PushControl(std::move(c));
        }
    }
    CT_LOG_INFO("event recorder started: region %dx%d at (%d,%d), keys %d, cursor %d Hz", config.region.w,
                config.region.h, config.region.x, config.region.y, static_cast<int>(config.keystrokes), s.config.cursor_hz);
    return true;
}

void EventRecorder::SetPaused(bool paused) {
    if (!m->running.load()) return;
    if (m->active.load() == !paused) return;
    Control c;
    c.ev.kind = Kind::Pause;
    c.ev.paused = paused ? 1 : 0;
    c.ev.t_us = m->clock->NowMediaUs();
    // Stop producing before the PAUSE record is queued, start again after the resume record.
    if (paused) m->active.store(false, std::memory_order_release);
    m->PushControl(std::move(c));
    if (!paused) m->active.store(true, std::memory_order_release);
    m->wake.Set();
}

void EventRecorder::Marker(std::string_view label, uint32_t kind) {
    if (!m->running.load()) return;
    Control c;
    c.ev.kind = Kind::Marker;
    c.ev.t_us = m->clock->NowMediaUs();
    c.marker_kind = kind;
    c.label.assign(label);
    m->PushControl(std::move(c));
    m->wake.Set();
}

bool EventRecorder::Flush(uint32_t timeout_ms) {
    if (!m->running.load()) return false;
    m->flush_done.Reset();
    m->flush_requested.store(true);
    m->wake.Set();
    return m->flush_done.Wait(timeout_ms);
}

void EventRecorder::Stop() {
    if (!m || !m->running.exchange(false)) return;
    m->hooks.Stop();   // no more producers on ct-hooks
    m->active.store(false);
    m->stop.Set();
    if (!m->io.Join(5000)) CT_FATAL("ct-events-io did not stop within 5 s");
    const Stats st = GetStats();
    CT_LOG_INFO("event recorder stopped: %llu records, %llu bytes, %llu ring drops, %u hook reinstalls",
                static_cast<unsigned long long>(st.records), static_cast<unsigned long long>(st.bytes),
                static_cast<unsigned long long>(st.ring_dropped), st.hook_reinstalls);
}

bool EventRecorder::IsRunning() const { return m->running.load(); }

const EventRecorder::Live& EventRecorder::LiveInfo() const { return m->live; }

void EventRecorder::LastKeyLabel(char out[32]) const {
    ExclusiveGuard g(m->label_lock);
    std::memcpy(out, m->last_label, 32);
    out[31] = 0;
}

EventRecorder::Stats EventRecorder::GetStats() const {
    Stats st;
    st.records = m->records.load(std::memory_order_relaxed);
    st.bytes = m->bytes.load(std::memory_order_relaxed);
    st.ring_dropped = m->ring_dropped.load(std::memory_order_relaxed);
    st.cursor_dropped = m->tracker.DroppedSamples();
    st.cursor_polls = m->tracker.Polls();
    st.hook_reinstalls = m->hooks.Reinstalls();
    st.flush_failures = m->flush_failures.load(std::memory_order_relaxed);
    st.typing_bursts = m->typing_bursts.load(std::memory_order_relaxed);
    return st;
}

} // namespace ct::recorder
