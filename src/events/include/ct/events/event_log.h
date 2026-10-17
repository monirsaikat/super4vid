#pragma once
// ct/events/event_log.h
// A whole events.ctev loaded into typed arrays sorted by time, with the time queries the cursor renderer, keystroke
// badges and auto-zoom need (P07, P08). Pure logic: no Windows, no GPU.
// Thread model: Load* from one thread; afterwards all const queries are safe from any number of threads.
// Lifetime: spans returned by the queries point into the log and stay valid until the next Load*.

#include <ct/core/error.h>
#include <ct/core/types.h>
#include <ct/events/ctev.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ct::events {

struct CursorSample { TimeUs t_us = 0; int32_t x = 0, y = 0; uint32_t shape_id = 0; bool visible = true; };
struct ButtonEvent { TimeUs t_us = 0; Button button = Button::Left; bool down = false; int32_t x = 0, y = 0; };
struct WheelEvent { TimeUs t_us = 0; int16_t delta_v = 0, delta_h = 0; int32_t x = 0, y = 0; };
struct KeyEvent {
    TimeUs t_us = 0;
    uint16_t vk = 0, scancode = 0;
    bool down = false, extended = false;
    uint8_t modifiers = 0;   // kModCtrl | kModShift | kModAlt | kModWin
    uint32_t utf32_char = 0;
    std::string label;       // "Ctrl", "A", "Enter"
};
// A key-down of a non-modifier key with its modifiers, as shown by a keystroke badge: "Ctrl+Shift+S".
struct KeyStroke { TimeUs t_us = 0; uint16_t vk = 0; uint8_t modifiers = 0; std::string text; };
struct CursorShapeData {
    uint32_t id = 0;
    uint16_t w = 0, h = 0;
    int16_t hot_x = 0, hot_y = 0;
    std::vector<uint8_t> bgra;   // decoded, premultiplied, w*h*4 bytes
};
struct FocusEvent { TimeUs t_us = 0; uint64_t hwnd = 0; RectI rect; std::string title, exe; };
struct MarkerEvent { TimeUs t_us = 0; uint32_t kind = 0; std::string label; };
struct PauseEvent { TimeUs t_us = 0; bool paused = false; };
struct TypingBurst { TimeUs start_us = 0, end_us = 0; };   // end_us == start_us of the last key when not closed

// Interpolated cursor state at a time.
struct CursorState {
    bool valid = false;     // false before the first sample / in an empty log
    float x = 0, y = 0;     // region pixels, linearly interpolated between neighbouring samples
    uint32_t shape_id = 0;  // from the sample at or before t
    bool visible = false;
};

class EventLog {
public:
    // Key-down gaps shorter than this belong to one typing burst when the file has no TYPING records.
    static constexpr TimeUs kTypingGapUs = 1'000'000;

    bool Load(std::string_view path, Err* err);
    bool LoadFromMemory(std::string bytes, Err* err);
    void Clear();

    const EventHeader& Header() const { return m_header; }
    SizeI RegionSize() const { return SizeI{m_header.region_w, m_header.region_h}; }
    bool Truncated() const { return m_truncated; }        // the file ended inside a record (crash)
    uint64_t UnknownRecords() const { return m_unknown; }  // skipped record types
    TimeUs LastTimeUs() const { return m_last_t; }         // largest t_us of any record (0 when empty)

    std::span<const CursorSample> Cursor() const { return m_cursor; }
    std::span<const ButtonEvent> Buttons() const { return m_buttons; }
    std::span<const WheelEvent> Wheels() const { return m_wheels; }
    std::span<const KeyEvent> Keys() const { return m_keys; }
    std::span<const KeyStroke> Strokes() const { return m_strokes; }
    std::span<const CursorShapeData> Shapes() const { return m_shapes; }   // sorted by id
    std::span<const FocusEvent> Focus() const { return m_focus; }
    std::span<const MarkerEvent> Markers() const { return m_markers; }
    std::span<const PauseEvent> Pauses() const { return m_pauses; }
    // From TYPING records, or derived from key-downs (gap < kTypingGapUs) when the file has none.
    std::span<const TypingBurst> TypingBursts() const { return m_typing; }

    // Cursor: before the first sample -> invalid; after the last -> last sample; between -> linear position,
    // shape/visibility of the earlier sample (no interpolation across a hide/show).
    CursorState CursorAt(TimeUs t_us) const;
    // Samples with t0 <= t_us < t1 (smoothing filters read a window around t).
    std::span<const CursorSample> CursorSamplesBetween(TimeUs t0_us, TimeUs t1_us) const;
    // Time of the newest cursor sample at or before t (CURSOR_MOVE is written only on change, so this is "the cursor
    // last moved at"); -1 when there is none. Used for hide-when-idle.
    TimeUs LastCursorChangeAt(TimeUs t_us) const;
    const CursorShapeData* FindShape(uint32_t shape_id) const;

    std::span<const ButtonEvent> ButtonsBetween(TimeUs t0_us, TimeUs t1_us) const;   // t0 <= t < t1
    // Newest button-down at or before t with t - down.t_us <= max_age_us (click ripple); nullptr when none.
    const ButtonEvent* LastMouseDown(TimeUs t_us, TimeUs max_age_us) const;
    // Mouse downs in [t0, t1), any button unless `only` is given. Appends to `out` (caller reuses the vector).
    void MouseDownsBetween(TimeUs t0_us, TimeUs t1_us, std::vector<ButtonEvent>& out, const Button* only = nullptr) const;
    std::span<const WheelEvent> WheelsBetween(TimeUs t0_us, TimeUs t1_us) const;
    std::span<const KeyEvent> KeysBetween(TimeUs t0_us, TimeUs t1_us) const;
    std::span<const KeyStroke> StrokesBetween(TimeUs t0_us, TimeUs t1_us) const;
    // Strokes that a badge holding each stroke for hold_us shows at t: t - hold_us < stroke.t_us <= t.
    std::span<const KeyStroke> StrokesVisibleAt(TimeUs t_us, TimeUs hold_us) const;
    // Whether `vk` is held down at t (the last KEY record for vk at or before t is a down).
    bool IsKeyDown(uint16_t vk, TimeUs t_us) const;
    bool IsTyping(TimeUs t_us) const;   // inside a typing burst
    const FocusEvent* FocusAt(TimeUs t_us) const;   // newest focus change at or before t

    // Builds "Ctrl+Shift+S" from modifier bits and a key label (modifiers in the order Ctrl, Alt, Shift, Win).
    static std::string StrokeText(uint8_t modifiers, std::string_view label);

private:
    void Finish();

    EventHeader m_header;
    bool m_truncated = false;
    uint64_t m_unknown = 0;
    TimeUs m_last_t = 0;
    std::vector<CursorSample> m_cursor;
    std::vector<ButtonEvent> m_buttons;
    std::vector<WheelEvent> m_wheels;
    std::vector<KeyEvent> m_keys;
    std::vector<KeyStroke> m_strokes;
    std::vector<CursorShapeData> m_shapes;
    std::vector<FocusEvent> m_focus;
    std::vector<MarkerEvent> m_markers;
    std::vector<PauseEvent> m_pauses;
    std::vector<TypingBurst> m_typing;
    std::vector<std::pair<TimeUs, bool>> m_typing_records;   // raw TYPING records before Finish()
};

} // namespace ct::events
