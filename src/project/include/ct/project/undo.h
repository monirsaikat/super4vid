#pragma once
// ct/project/undo.h
// Snapshot undo/redo: every undoable edit pushes the project JSON from before the edit. Undo restores it (keeping the
// project path, the UI state and the media `missing` flags of the current project, which are not document history).
// Coalescing turns a drag into one entry. Also tracks the dirty flag against the last save.
// Thread model: UI thread only. Lifetime: plain object owned by the editor document.

#include <ct/project/project.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace ct::project {

class UndoStack {
public:
    explicit UndoStack(size_t max_entries = 200, size_t max_bytes = 64ull << 20);

    // Call before mutating `before`. Clears the redo stack. Inside a coalesce group only the first push is recorded.
    void Push(const Project& before, const char* label);
    void PushJson(std::string before_json, const char* label);   // when the caller already has ToJson(before)
    // current_json: ToJson(current) when the caller already has it (saves one serialization).
    bool Undo(Project& current, const std::string* current_json = nullptr);
    bool Redo(Project& current, const std::string* current_json = nullptr);
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    const char* UndoLabel() const;   // "" when empty
    const char* RedoLabel() const;

    // A drag = one undo entry: between Begin and End only the first Push is recorded. A Begin with another key
    // starts a new group.
    void BeginCoalesce(const char* key);
    void EndCoalesce();
    bool Coalescing() const { return m_coalescing; }

    size_t Entries() const { return m_undo.size() + m_redo.size(); }
    size_t UndoEntries() const { return m_undo.size(); }
    size_t Bytes() const { return m_bytes; }
    void Clear();   // also marks the current state as saved

    // Dirty flag: the current state differs from the one passed to MarkSaved (after Save / Load).
    void MarkSaved() { m_saved_state = m_state; }
    bool IsDirty() const { return m_state != m_saved_state; }

private:
    struct Entry {
        std::string json;
        std::string label;
        uint64_t state;   // state serial of the snapshot
    };
    void Enforce();
    bool Restore(const Entry& e, Project& current);

    std::deque<Entry> m_undo, m_redo;
    size_t m_max_entries, m_max_bytes;
    size_t m_bytes = 0;
    bool m_coalescing = false;
    bool m_coalesce_pushed = false;
    std::string m_coalesce_key;
    uint64_t m_state = 0, m_saved_state = 0, m_next_state = 0;
};

} // namespace ct::project
