#pragma once
#include <vector>

#include "sage/ui/core/UIContext.h"
#include "sage/ui/layout/UILayoutSolver.h"

// ---------------------------------------------------------------------------
// ПОПАДАНИЕ КУРСОРОМ (§52–54 ТЗ).
//
// Прямоугольник — НЕ единственный способ определить попадание. Круглая кнопка,
// скруглённая карточка, значок с прозрачными углами, шестиугольник карты — по
// прямоугольнику всё это ловит мышь там, где ничего не нарисовано, и это видно
// сразу: подсказка выскакивает рядом с иконкой, а не на ней.
//
// ПОРЯДОК ОДНОЗНАЧЕН (§54): слой → порядок → место в дереве. Тот же ключ, по
// которому идёт рисование. Никаких отдельных правил для ввода — иначе «кликается
// не то, что видно» становится неизбежностью.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIDocument;

struct UIHitResult {
    UINodeId Node = kUIInvalidNode;
    int Index = -1;
    glm::vec2 Local{0.0f, 0.0f}; // точка в координатах узла
};

// Верхний узел под точкой, принимающий ввод.
UIHitResult UIHitTest(const UIDocument& doc, const UILayoutSolver& layout,
                      const UIContext& ctx, glm::vec2 point);

// Все узлы под точкой, сверху вниз — нужно распространению событий (§51):
// путь от корня до цели строится один раз и переиспользуется всеми фазами.
void UIHitPath(const UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
               glm::vec2 point, std::vector<UINodeId>& outPath);

// Попадает ли точка в конкретный узел (с учётом его формы попадания и масок).
bool UIHitNode(const UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
               UINodeId id, glm::vec2 point);

} // namespace sage::ui
