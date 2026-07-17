#pragma once

class EditorHost;

// Панель Hierarchy — список сущностей сцены: выбор, создание (ПКМ по пустому
// месту), дублирование/удаление (ПКМ по сущности). Без собственного состояния —
// всё через EditorHost.
class HierarchyPanel {
public:
    void Draw(EditorHost& host);
};
