#pragma once
// ct/ui/widgets.h
// Shared custom widgets in the docs/05 style (used by the editor and the recorder bar): icon buttons, segmented
// control, toggle switch, level meter, popover, collapsible section headers, property rows with keyframe toggles,
// and the toast stack.
// Thread model: UI thread, inside an ImGui frame.

#include <ct/core/types.h>

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace ct::ui {

// Tooltip "Name  Shortcut" for the last item (docs/05 section 7: every control has one).
void ItemTooltip(const char* name, const char* shortcut = nullptr);

// Square (or `size`) button showing an icon glyph (ICON_*), optional label after it. `active` draws it pressed
// (toggle buttons). Hover state always; tooltip when `tooltip` is set. Unique id comes from `id`.
bool IconButton(const char* id, const char* icon, const char* tooltip = nullptr, bool active = false,
                const char* label = nullptr, ImVec2 size = ImVec2(0, 0));
// Filled primary (accent) or destructive button.
enum class ButtonKind { Normal, Primary, Danger, Record };
bool Button(const char* label, ButtonKind kind = ButtonKind::Normal, ImVec2 size = ImVec2(0, 0));

// Pill-shaped segmented control; returns true when *current changed.
bool SegmentedControl(const char* id, int* current, const char* const* items, int count, float item_width = 0);
// iOS-style toggle switch with a label on its left (in the property label column when inside a PropRow).
bool ToggleSwitch(const char* id, bool* v);

// 3-segment level meter (docs/05 section 2): green < -18 dBFS, amber < -6, red above. level_db <= -60 = silent.
void MeterBar(const char* id, float level_db, ImVec2 size);
// Meter segment color for a level (exposed for tests).
ImU32 MeterColor(float level_db);

// Popover: a styled popup that fades/slides in over 160 ms. Open with ImGui::OpenPopup(id).
bool BeginPopover(const char* id);
void EndPopover();

// Collapsible section header (properties panel). Remembers its state (ImGui storage). Returns open.
bool SectionHeader(const char* label, bool default_open = true, const char* right_text = nullptr);

// --- Property rows -------------------------------------------------------------------------------------------
// Keyframe toggle hook for keyframeable fields: the panel implements it against state.Edit (widgets never touch
// the model). The ◇ diamond is filled when HasKey() (a key within half a frame of the playhead).
struct KeyframeHook {
    virtual ~KeyframeHook() = default;
    virtual bool HasKey() const = 0;      // key at the playhead
    virtual bool Animated() const = 0;    // the property has any keys
    virtual void Toggle(float current_value) = 0;   // add a key at the playhead, or remove the one there
};

// Label column width (DIP) used by every property row.
constexpr float kPropLabelWidth = 92;

// Drag float with unit (`fmt` like "%.2f", unit like "%" / "°" / "px" / "dB", may be null). Alt+click resets to
// `reset`, Ctrl+click types a value, the ◇ toggle is drawn when `kf` is set. The drag field is the last item
// submitted, so ImGui::IsItemActivated()/IsItemDeactivated() after the call refer to it (undo coalescing).
bool PropFloat(const char* label, float& v, float min, float max, const char* fmt, const char* unit,
               KeyframeHook* kf = nullptr, float reset = 0.0f, float speed = 0.0f);
bool PropInt(const char* label, int& v, int min, int max, const char* unit, int reset = 0);
bool PropColor(const char* label, ColorF& c, bool alpha = true);
bool PropEnum(const char* label, int& v, const char* const* items, int count);
bool PropToggle(const char* label, bool& v);
// Time in microseconds shown as seconds ("12.500 s"); drag speed 10 ms per pixel.
bool PropTime(const char* label, TimeUs& v, TimeUs min, TimeUs max);
bool PropText(const char* label, std::string& s, bool multiline = false);
// Read-only value row.
void PropValue(const char* label, const char* value);
// Starts a property row: label in the label column, cursor placed at the field column. Returns the field width.
float PropRowBegin(const char* label);

// --- Toasts (bottom-right, stacked, 6 s, hover pauses; docs/05 section 4) ---------------------------------------
enum class ToastKind { Info, Success, Warning, Error };
struct ToastButton {
    std::string label;
    std::function<void()> on_click;
};

class ToastStack {
public:
    int Push(std::string text, ToastKind kind = ToastKind::Info, std::vector<ToastButton> buttons = {},
             float seconds = 6.0f);
    void Dismiss(int id);
    // Draws in the main viewport's bottom-right corner (above `bottom_inset` DIP, e.g. the status bar).
    void Draw(float bottom_inset = 0);
    bool Empty() const { return m_toasts.empty(); }
    // True while toasts animate or count down (the app keeps rendering).
    bool Animating() const { return !m_toasts.empty(); }
    size_t Count() const { return m_toasts.size(); }

private:
    struct Toast {
        int id;
        std::string text;
        ToastKind kind;
        std::vector<ToastButton> buttons;
        float seconds;
        double shown_at;      // ImGui time
        double paused_total;  // seconds spent hovered
        double hover_since;   // < 0 when not hovered
        double closing_at;    // < 0 until dismissed / expired
    };
    std::vector<Toast> m_toasts;
    int m_next_id = 1;
};

// Short time formats into a caller buffer (no allocation): "1:05.25" (m:ss.cc), "01:12.03" (frames at fps).
const char* FormatDuration(char* buf, size_t n, TimeUs t);
// HH:MM:SS.FF with FF = frame number within the second at fps_num/fps_den.
const char* FormatTimecode(char* buf, size_t n, TimeUs t, int fps_num, int fps_den);

} // namespace ct::ui
