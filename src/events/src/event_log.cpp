#include <ct/events/event_log.h>

#include <ct/core/fs.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace ct::events {

namespace {

// Virtual-key codes of modifier keys (winuser.h values; this module stays Windows-free).
bool IsModifierVk(uint16_t vk) {
    switch (vk) {
    case 0x10: case 0x11: case 0x12:                                    // VK_SHIFT, VK_CONTROL, VK_MENU
    case 0x5B: case 0x5C:                                               // VK_LWIN, VK_RWIN
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:   // VK_LSHIFT .. VK_RMENU
        return true;
    default:
        return false;
    }
}

std::string FixedString(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') ++n;
    return std::string(s, n);
}

template <class P>
bool ReadPayload(const RecordView& r, P& out) {
    if (r.payload_size < sizeof(P)) return false;   // short (damaged) record
    std::memcpy(&out, r.payload, sizeof(P));
    return true;
}

// Validates a CURSOR_SHAPE record and decodes it. Every size is checked before anything is allocated.
bool DecodeShape(const RecordView& r, CursorShapeData& s) {
    CursorShapePayload p;
    if (!ReadPayload(r, p) || p.format != 0) return false;
    if (p.w == 0 || p.h == 0 || p.w > kMaxShapeSize || p.h > kMaxShapeSize) return false;
    if (p.data_size > r.payload_size - sizeof(p)) return false;
    const uint8_t* data = r.payload + sizeof(p);
    const size_t pixels = size_t(p.w) * size_t(p.h);
    if (p.compression == 0) {
        if (p.data_size != pixels * 4) return false;
        s.bgra.assign(data, data + p.data_size);
    } else if (p.compression == 1) {
        if (!RleDecode(data, p.data_size, pixels, s.bgra)) return false;
    } else {
        return false;
    }
    s.id = p.shape_id;
    s.w = p.w;
    s.h = p.h;
    s.hot_x = p.hot_x;
    s.hot_y = p.hot_y;
    return true;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Timestamps come from files and may be garbage (INT64_MIN/MAX): differences and offsets saturate instead of
// overflowing (signed overflow is UB).
constexpr TimeUs kMinT = std::numeric_limits<TimeUs>::min();
constexpr TimeUs kMaxT = std::numeric_limits<TimeUs>::max();

TimeUs SatSub(TimeUs a, TimeUs b) {
    if (b > 0 && a < kMinT + b) return kMinT;
    if (b < 0 && a > kMaxT + b) return kMaxT;
    return a - b;
}

// Elements with t0 <= t_us < t1 of an array sorted by t_us.
template <class T>
std::span<const T> Range(const std::vector<T>& v, TimeUs t0, TimeUs t1) {
    if (t1 <= t0) return {};
    auto lo = std::lower_bound(v.begin(), v.end(), t0, [](const T& e, TimeUs t) { return e.t_us < t; });
    auto hi = std::lower_bound(lo, v.end(), t1, [](const T& e, TimeUs t) { return e.t_us < t; });
    return std::span<const T>(v.data() + (lo - v.begin()), static_cast<size_t>(hi - lo));
}

// Index of the last element with t_us <= t, or -1.
template <class T>
ptrdiff_t LastAtOrBefore(const std::vector<T>& v, TimeUs t) {
    auto it = std::upper_bound(v.begin(), v.end(), t, [](TimeUs tt, const T& e) { return tt < e.t_us; });
    return (it - v.begin()) - 1;
}

template <class T>
void SortByTime(std::vector<T>& v) {
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.t_us < b.t_us; });
}

} // namespace

void EventLog::Clear() {
    m_header = EventHeader{};
    m_truncated = false;
    m_unknown = 0;
    m_last_t = 0;
    m_cursor.clear();
    m_buttons.clear();
    m_wheels.clear();
    m_keys.clear();
    m_strokes.clear();
    m_shapes.clear();
    m_focus.clear();
    m_markers.clear();
    m_pauses.clear();
    m_typing.clear();
    m_typing_records.clear();
}

bool EventLog::Load(std::string_view path, Err* err) {
    Clear();
    std::string bytes;
    if (!fs::ReadAll(path, bytes)) {
        if (err) err->Set("Could not read the events file " + std::string(path));
        return false;
    }
    return LoadFromMemory(std::move(bytes), err);
}

bool EventLog::LoadFromMemory(std::string bytes, Err* err) {
    Clear();
    EventReader reader;
    if (!reader.Parse(std::move(bytes), err)) return false;
    m_header = reader.Header();
    RecordView r;
    bool any = false;
    // A record of a known type whose payload is too short or inconsistent means the file is damaged (not just torn
    // at the end): the whole load fails instead of guessing.
    bool damaged = false;
    auto need = [&](bool ok) {
        if (!ok) damaged = true;
        return ok;
    };
    while (reader.Next(r)) {
        if (!any || r.t_us > m_last_t) m_last_t = r.t_us;
        any = true;
        switch (static_cast<RecordType>(r.type)) {
        case RecordType::CursorMove: {
            CursorMovePayload p;
            if (need(ReadPayload(r, p))) m_cursor.push_back({r.t_us, p.x, p.y, p.shape_id, p.visible != 0});
            break;
        }
        case RecordType::MouseButton: {
            MouseButtonPayload p;
            if (need(ReadPayload(r, p)) && p.button <= static_cast<uint8_t>(Button::X2))
                m_buttons.push_back({r.t_us, static_cast<Button>(p.button), p.down != 0, p.x, p.y});
            break;
        }
        case RecordType::MouseWheel: {
            MouseWheelPayload p;
            if (need(ReadPayload(r, p))) m_wheels.push_back({r.t_us, p.delta_v, p.delta_h, p.x, p.y});
            break;
        }
        case RecordType::Key: {
            KeyPayload p;
            if (!need(ReadPayload(r, p))) break;
            KeyEvent k;
            k.t_us = r.t_us;
            k.vk = p.vk;
            k.scancode = p.scancode;
            k.down = p.down != 0;
            k.extended = p.extended != 0;
            k.modifiers = p.modifiers;
            k.utf32_char = p.utf32_char;
            k.label = FixedString(p.label, sizeof(p.label));
            m_keys.push_back(std::move(k));
            break;
        }
        case RecordType::CursorShape: {
            CursorShapeData s;
            if (!DecodeShape(r, s)) {
                damaged = true;
                break;
            }
            if (!FindShape(s.id)) {
                auto it = std::lower_bound(m_shapes.begin(), m_shapes.end(), s.id,
                                           [](const CursorShapeData& a, uint32_t id) { return a.id < id; });
                m_shapes.insert(it, std::move(s));
            }
            break;
        }
        case RecordType::WindowFocus: {
            WindowFocusPayload p;
            if (!need(ReadPayload(r, p))) break;
            m_focus.push_back({r.t_us, p.hwnd, RectI{p.x, p.y, p.w, p.h}, FixedString(p.title, sizeof(p.title)),
                               FixedString(p.exe, sizeof(p.exe))});
            break;
        }
        case RecordType::Marker: {
            MarkerPayload p;
            if (need(ReadPayload(r, p))) m_markers.push_back({r.t_us, p.kind, FixedString(p.label, sizeof(p.label))});
            break;
        }
        case RecordType::Pause: {
            PausePayload p;
            if (need(ReadPayload(r, p))) m_pauses.push_back({r.t_us, p.paused != 0});
            break;
        }
        case RecordType::Typing: {
            TypingPayload p;
            if (need(ReadPayload(r, p))) m_typing_records.push_back({r.t_us, p.started != 0});
            break;
        }
        default:
            ++m_unknown;
            break;
        }
    }
    if (damaged) {
        Clear();
        if (err) err->Set("The events file is damaged (a record has invalid sizes)");
        return false;
    }
    m_truncated = reader.Truncated();
    Finish();
    return true;
}

void EventLog::Finish() {
    SortByTime(m_cursor);
    SortByTime(m_buttons);
    SortByTime(m_wheels);
    SortByTime(m_keys);
    SortByTime(m_focus);
    SortByTime(m_markers);
    SortByTime(m_pauses);
    std::stable_sort(m_typing_records.begin(), m_typing_records.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    // Strokes: key-downs of non-modifier keys; auto-repeat (a down after a down of the same vk) is not a new stroke.
    std::vector<uint16_t> held;
    for (const KeyEvent& k : m_keys) {
        auto it = std::find(held.begin(), held.end(), k.vk);
        if (!k.down) {
            if (it != held.end()) held.erase(it);
            continue;
        }
        if (it != held.end()) continue;
        held.push_back(k.vk);
        if (IsModifierVk(k.vk)) continue;
        std::string label = k.label;
        if (label.empty() && k.utf32_char) AppendUtf8(label, k.utf32_char);
        if (label.empty()) continue;
        m_strokes.push_back({k.t_us, k.vk, k.modifiers, StrokeText(k.modifiers, label)});
    }

    // Typing bursts: from TYPING records when present, else derived from non-modifier key-downs.
    if (!m_typing_records.empty()) {
        bool open = false;
        for (const auto& [t, started] : m_typing_records) {
            if (started) {
                if (open) continue;
                m_typing.push_back({t, t});
                open = true;
            } else if (open) {
                m_typing.back().end_us = t;
                open = false;
            }
        }
        if (open) m_typing.back().end_us = std::max(m_typing.back().start_us, m_last_t);
    } else {
        for (const KeyEvent& k : m_keys) {
            if (!k.down || IsModifierVk(k.vk)) continue;
            if (!m_typing.empty() && SatSub(k.t_us, m_typing.back().end_us) < kTypingGapUs)
                m_typing.back().end_us = k.t_us;
            else
                m_typing.push_back({k.t_us, k.t_us});
        }
    }
    m_typing_records.clear();
    m_typing_records.shrink_to_fit();
}

CursorState EventLog::CursorAt(TimeUs t_us) const {
    CursorState s;
    const ptrdiff_t i = LastAtOrBefore(m_cursor, t_us);
    if (i < 0) return s;
    const CursorSample& a = m_cursor[static_cast<size_t>(i)];
    s.valid = true;
    s.shape_id = a.shape_id;
    s.visible = a.visible;
    s.x = static_cast<float>(a.x);
    s.y = static_cast<float>(a.y);
    if (static_cast<size_t>(i) + 1 < m_cursor.size()) {
        const CursorSample& b = m_cursor[static_cast<size_t>(i) + 1];
        if (b.t_us > a.t_us && a.visible == b.visible) {
            // In double: b - a can exceed the int64 range; 0 <= t - a <= b - a, so u stays in [0, 1].
            const double u = std::clamp((double(t_us) - double(a.t_us)) / (double(b.t_us) - double(a.t_us)), 0.0, 1.0);
            s.x = static_cast<float>(a.x + (b.x - a.x) * u);
            s.y = static_cast<float>(a.y + (b.y - a.y) * u);
        }
    }
    return s;
}

std::span<const CursorSample> EventLog::CursorSamplesBetween(TimeUs t0_us, TimeUs t1_us) const {
    return Range(m_cursor, t0_us, t1_us);
}

TimeUs EventLog::LastCursorChangeAt(TimeUs t_us) const {
    const ptrdiff_t i = LastAtOrBefore(m_cursor, t_us);
    return i < 0 ? -1 : m_cursor[static_cast<size_t>(i)].t_us;
}

const CursorShapeData* EventLog::FindShape(uint32_t shape_id) const {
    auto it = std::lower_bound(m_shapes.begin(), m_shapes.end(), shape_id,
                               [](const CursorShapeData& a, uint32_t id) { return a.id < id; });
    return (it != m_shapes.end() && it->id == shape_id) ? &*it : nullptr;
}

std::span<const ButtonEvent> EventLog::ButtonsBetween(TimeUs t0_us, TimeUs t1_us) const {
    return Range(m_buttons, t0_us, t1_us);
}

const ButtonEvent* EventLog::LastMouseDown(TimeUs t_us, TimeUs max_age_us) const {
    for (ptrdiff_t i = LastAtOrBefore(m_buttons, t_us); i >= 0; --i) {
        const ButtonEvent& b = m_buttons[static_cast<size_t>(i)];
        if (SatSub(t_us, b.t_us) > max_age_us) return nullptr;
        if (b.down) return &b;
    }
    return nullptr;
}

void EventLog::MouseDownsBetween(TimeUs t0_us, TimeUs t1_us, std::vector<ButtonEvent>& out, const Button* only) const {
    for (const ButtonEvent& b : Range(m_buttons, t0_us, t1_us))
        if (b.down && (!only || b.button == *only)) out.push_back(b);
}

std::span<const WheelEvent> EventLog::WheelsBetween(TimeUs t0_us, TimeUs t1_us) const {
    return Range(m_wheels, t0_us, t1_us);
}

std::span<const KeyEvent> EventLog::KeysBetween(TimeUs t0_us, TimeUs t1_us) const { return Range(m_keys, t0_us, t1_us); }

std::span<const KeyStroke> EventLog::StrokesBetween(TimeUs t0_us, TimeUs t1_us) const {
    return Range(m_strokes, t0_us, t1_us);
}

std::span<const KeyStroke> EventLog::StrokesVisibleAt(TimeUs t_us, TimeUs hold_us) const {
    // Strokes with t_us - hold_us < t <= t_us, without forming t_us + 1 or t_us - hold_us out of range.
    if (hold_us <= 0) return {};
    auto later = [](TimeUs t, const KeyStroke& k) { return t < k.t_us; };
    auto hi = std::upper_bound(m_strokes.begin(), m_strokes.end(), t_us, later);
    auto lo = t_us < kMinT + hold_us ? m_strokes.begin()   // t_us - hold_us is below every representable time
                                     : std::upper_bound(m_strokes.begin(), hi, t_us - hold_us, later);
    return std::span<const KeyStroke>(m_strokes.data() + (lo - m_strokes.begin()), static_cast<size_t>(hi - lo));
}

bool EventLog::IsKeyDown(uint16_t vk, TimeUs t_us) const {
    for (ptrdiff_t i = LastAtOrBefore(m_keys, t_us); i >= 0; --i) {
        const KeyEvent& k = m_keys[static_cast<size_t>(i)];
        if (k.vk == vk) return k.down;
    }
    return false;
}

bool EventLog::IsTyping(TimeUs t_us) const {
    auto it = std::upper_bound(m_typing.begin(), m_typing.end(), t_us,
                               [](TimeUs t, const TypingBurst& b) { return t < b.start_us; });
    if (it == m_typing.begin()) return false;
    --it;
    return t_us <= it->end_us;
}

const FocusEvent* EventLog::FocusAt(TimeUs t_us) const {
    const ptrdiff_t i = LastAtOrBefore(m_focus, t_us);
    return i < 0 ? nullptr : &m_focus[static_cast<size_t>(i)];
}

std::string EventLog::StrokeText(uint8_t modifiers, std::string_view label) {
    std::string s;
    if (modifiers & kModCtrl) s += "Ctrl+";
    if (modifiers & kModAlt) s += "Alt+";
    if (modifiers & kModShift) s += "Shift+";
    if (modifiers & kModWin) s += "Win+";
    s.append(label);
    return s;
}

} // namespace ct::events
