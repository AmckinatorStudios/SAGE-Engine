#pragma once
#include "imgui.h"

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

    // Какой значок у сущности в списке. Публично — потому что это ДОГОВОР с
    // человеком, а не подробность рисования: значок отвечает на вопрос «что это
    // за предмет», и правило «все формы — один значок, а типы света разные»
    // проверяется, а не держится на памяти того, кто последним правил список.
    static const char* IconFor(entt::registry& reg, entt::entity e);

private:
    // Линии связи дерева рисуются сами (см. DrawTreeLines): у встроенных в
    // ImGui горизонталь обрывается далеко от значка.
    struct Row {
        float Y = 0.0f;      // верх строки
        float IconX = 0.0f;  // где начинается значок
        float ArrowX = 0.0f; // левый край стрелки раскрытия (если она есть)
        bool Arrow = false;  // у строки есть стрелка — линия обязана в неё не влезать
        int Depth = 0;
    };
    std::vector<Row> m_rows;
    int m_depth = 0;
    void DrawTreeLines(const ImVec2& parentPos, float indent, int childDepth);

    void DrawNode(EditorHost& host, Scene& scene, entt::entity e);

    sage::editor::rectselect::State m_rect;
    // Кого рамка задела в ЭТОМ кадре. Собирается по ходу рисования дерева:
    // прямоугольник строки известен только сразу после её отрисовки.
    std::vector<int> m_rectHits;
    bool m_rectActive = false;

    // ВЫБОР ДИАПАЗОНА С SHIFT: якорь и порядок строк НА ЭКРАНЕ.
    //
    // Порядок — тот, который человек видит: дерево, обойдённое сверху вниз, со
    // свёрнутыми ветками (их детей на экране нет, и в диапазон они не входят).
    // Собирается по ходу рисования, поэтому сам щелчок только ЗАПОМИНАЕТСЯ:
    // строки ниже к этому моменту ещё не нарисованы, и диапазон до них не
    // посчитать. Разбирается он в конце кадра, когда список полон.
    std::vector<int> m_visibleRows;
    std::vector<ImVec2> m_rowCenters;   // где эти строки на экране (для самопроверки)
    int m_anchorId = -1;
    int m_shiftClickId = -1;

public:
    // Сколько строк нарисовано в этом кадре и где их середина. Нужно
    // самопроверке: она выбирает диапазон настоящими щелчками с Shift.
    int RowCount() const { return (int)m_rowCenters.size(); }
    ImVec2 RowCenter(int i) const { return m_rowCenters[(size_t)i]; }
    int RowId(int i) const { return m_visibleRows[(size_t)i]; }
};
