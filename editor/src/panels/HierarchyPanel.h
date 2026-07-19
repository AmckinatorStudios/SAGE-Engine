#pragma once

#include <entt/entt.hpp>

class EditorHost;
class Scene;

// Панель Hierarchy — ДЕРЕВО сущностей сцены с иерархией родитель/ребёнок:
// выбор, создание (ПКМ по пустому месту), Create Child / Duplicate / Unparent /
// Delete (ПКМ по сущности), перетаскивание одной сущности на другую для смены
// родителя (drag-drop) и перетаскивание в пустую зону — открепление в корень.
// Без собственного состояния — всё через EditorHost/Scene.
class HierarchyPanel {
public:
    void Draw(EditorHost& host);

private:
    void DrawNode(EditorHost& host, Scene& scene, entt::entity e);
};
