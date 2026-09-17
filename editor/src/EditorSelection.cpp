#include "EditorSelection.h"

#include <algorithm>

bool EditorSelection::Contains(int id) const {
    return std::find(m_items.begin(), m_items.end(), id) != m_items.end();
}

void EditorSelection::SetPrimary(int id) {
    m_primary = id;
    m_items.clear();
    if (id != -1) m_items.push_back(id);
}

void EditorSelection::Set(const std::vector<int>& ids, bool additive) {
    if (!additive) m_items.clear();
    for (int id : ids) {
        if (id == -1) continue;
        if (!Contains(id)) m_items.push_back(id);
    }
    // ПЕРВИЧНАЯ — последняя добавленная: под неё встаёт инспектор и пивот
    // гизмо. Пустой набор означает «ничего не выбрано», а не «первичная
    // осталась прежней»: иначе инспектор показывал бы поля объекта, который на
    // экране уже не подсвечен.
    m_primary = m_items.empty() ? -1 : m_items.back();
}

void EditorSelection::Toggle(int id) {
    if (id == -1) return;
    auto it = std::find(m_items.begin(), m_items.end(), id);
    if (it != m_items.end()) {
        m_items.erase(it);
        m_primary = m_items.empty() ? -1 : m_items.back();
    } else {
        m_items.push_back(id);
        m_primary = id; // добавленная становится первичной
    }
}

void EditorSelection::Lock(int entityId, const std::filesystem::path& assetPath) {
    m_locked = true;
    m_lockedEntityId = entityId;
    m_lockedAssetPath = assetPath;
}
