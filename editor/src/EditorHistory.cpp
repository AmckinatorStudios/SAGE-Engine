#include "EditorHistory.h"

void EditorHistory::PushSnapshot(std::string snapshot) {
    if (m_undo.size() >= kMaxEntries) m_undo.erase(m_undo.begin());
    m_undo.push_back(std::move(snapshot));
    m_redo.clear(); // новая мутация обрывает ветку повторов
}

bool EditorHistory::Push() {
    if (!m_enabled) return false;
    PushSnapshot(m_capture());
    return true;
}

void EditorHistory::CapturePending() {
    if (!m_enabled) return;
    m_pending = m_capture();
}

bool EditorHistory::CommitPending() {
    if (!m_enabled || m_pending.empty()) return false;
    PushSnapshot(std::move(m_pending));
    m_pending.clear();
    return true;
}

bool EditorHistory::Undo() {
    if (!m_enabled || m_undo.empty()) return false;
    m_redo.push_back(m_capture());
    if (!m_restore(m_undo.back())) {
        m_redo.pop_back(); // откат не удался — историю не ломаем
        return false;
    }
    m_undo.pop_back();
    return true;
}

bool EditorHistory::Redo() {
    if (!m_enabled || m_redo.empty()) return false;
    m_undo.push_back(m_capture());
    if (!m_restore(m_redo.back())) {
        m_undo.pop_back();
        return false;
    }
    m_redo.pop_back();
    return true;
}

void EditorHistory::Clear() {
    m_undo.clear();
    m_redo.clear();
    m_pending.clear();
}
