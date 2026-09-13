#pragma once

#include <vector>

#include <entt/entt.hpp>

#include "../RectSelect.h"

class EditorHost;
class Scene;

// Панель Hierarchy — ДЕРЕВО сущностей сцены с иерархией родитель/ребёнок:
// выбор, создание (ПКМ по пустому месту), Create Child / Duplicate / Unparent /
// Delete (ПКМ по сущности), перетаскивание одной сущности на другую для смены
// родителя (drag-drop) и перетаскивание в пустую зону — открепление в корень.
// Своего состояния — только рамка выделения: она живёт между кадрами (ведут её
// несколько кадров подряд), остальное берётся из EditorHost/Scene.
class HierarchyPanel {
public:
    void Draw(EditorHost& host, bool* open);

private:
    void DrawNode(EditorHost& host, Scene& scene, entt::entity e);

    sage::editor::rectselect::State m_rect;
    // Кого рамка задела в ЭТОМ кадре. Собирается по ходу рисования дерева:
    // прямоугольник строки известен только сразу после её отрисовки.
    std::vector<int> m_rectHits;
    bool m_rectActive = false;
};
