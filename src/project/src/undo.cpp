#include <ct/project/undo.h>

#include <ct/project/serialize.h>

#include <utility>

namespace ct::project {

UndoStack::UndoStack(size_t max_entries, size_t max_bytes)
    : m_max_entries(max_entries < 1 ? 1 : max_entries), m_max_bytes(max_bytes) {}

void UndoStack::Push(const Project& before, const char* label) {
    if (m_coalescing && m_coalesce_pushed) {
        // Later steps of a drag: no entry, but the state did change.
        for (const Entry& e : m_redo) m_bytes -= e.json.size();
        m_redo.clear();
        m_state = ++m_next_state;
        return;
    }
    PushJson(ToJson(before), label);
}

void UndoStack::PushJson(std::string before_json, const char* label) {
    for (const Entry& e : m_redo) m_bytes -= e.json.size();
    m_redo.clear();
    const bool skip = m_coalescing && m_coalesce_pushed;
    if (m_coalescing) m_coalesce_pushed = true;
    if (!skip) {
        m_bytes += before_json.size();
        m_undo.push_back(Entry{std::move(before_json), label ? label : "", m_state});
    }
    m_state = ++m_next_state;
    Enforce();
}

// Oldest undo entries go first; when only redo entries are left (after undoing everything, a redo snapshot can be
// larger than the undo entry it replaced), the farthest redo goes.
void UndoStack::Enforce() {
    while (m_undo.size() + m_redo.size() > m_max_entries || m_bytes > m_max_bytes) {
        if (!m_undo.empty()) {
            m_bytes -= m_undo.front().json.size();
            m_undo.pop_front();
        } else if (!m_redo.empty()) {
            m_bytes -= m_redo.front().json.size();
            m_redo.pop_front();
        } else {
            break;
        }
    }
}

bool UndoStack::Restore(const Entry& e, Project& current) {
    Project restored;
    restored.path = current.path;
    if (!FromJson(e.json, restored, nullptr)) return false;
    // Not document history: where it is saved, what the UI shows, which files exist right now.
    restored.ui = current.ui;
    for (Media& m : restored.media)
        if (const Media* cur = current.FindMedia(m.id)) m.missing = cur->missing;
    current = std::move(restored);
    return true;
}

bool UndoStack::Undo(Project& current, const std::string* current_json) {
    if (m_undo.empty()) return false;
    EndCoalesce();
    Entry e = std::move(m_undo.back());
    m_undo.pop_back();
    Entry redo{current_json ? *current_json : ToJson(current), e.label, m_state};
    if (!Restore(e, current)) {
        m_bytes -= e.json.size();   // unreadable snapshot (cannot happen for our own JSON): drop it
        return false;
    }
    m_bytes += redo.json.size();
    m_bytes -= e.json.size();
    m_state = e.state;
    m_redo.push_back(std::move(redo));
    Enforce();
    return true;
}

bool UndoStack::Redo(Project& current, const std::string* current_json) {
    if (m_redo.empty()) return false;
    EndCoalesce();
    Entry e = std::move(m_redo.back());
    m_redo.pop_back();
    Entry undo{current_json ? *current_json : ToJson(current), e.label, m_state};
    if (!Restore(e, current)) {
        m_bytes -= e.json.size();
        return false;
    }
    m_bytes += undo.json.size();
    m_bytes -= e.json.size();
    m_state = e.state;
    m_undo.push_back(std::move(undo));
    Enforce();
    return true;
}

const char* UndoStack::UndoLabel() const { return m_undo.empty() ? "" : m_undo.back().label.c_str(); }
const char* UndoStack::RedoLabel() const { return m_redo.empty() ? "" : m_redo.back().label.c_str(); }

void UndoStack::BeginCoalesce(const char* key) {
    const std::string k = key ? key : "";
    if (m_coalescing && k == m_coalesce_key) return;
    m_coalescing = true;
    m_coalesce_pushed = false;
    m_coalesce_key = k;
}

void UndoStack::EndCoalesce() {
    m_coalescing = false;
    m_coalesce_pushed = false;
    m_coalesce_key.clear();
}

void UndoStack::Clear() {
    m_undo.clear();
    m_redo.clear();
    m_bytes = 0;
    EndCoalesce();
    m_saved_state = m_state;
}

} // namespace ct::project
