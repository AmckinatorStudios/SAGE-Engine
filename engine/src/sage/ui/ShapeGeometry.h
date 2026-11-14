#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "sage/ui/UIAnchor.h"
#include "sage/ui/components/Visual.h"

// ---------------------------------------------------------------------------
// ГЕОМЕТРИЯ ФОРМЫ — общая для элемента Shape и оформления Panel.
//
// Форма — это КОНТУР (замкнутый список точек по часовой стрелке) и цвет в
// любой его точке. Отрисовка (UIRenderer::FillPolygon / StrokePolygon) не
// знает, круг перед ней или скруглённый прямоугольник: она получает точки и
// цвета. Здесь — только математика, без графики, поэтому она проверяется
// модульными тестами.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Контур формы в прямоугольнике r, сдвинутый наружу на offset (< 0 — внутрь):
// так строятся и внешний, и внутренний край обводки, и полоса сглаживания.
// pixelScale — сколько экранных пикселей в единице r: от него зависит,
// сколько точек нужно дуге, чтобы она выглядела гладкой.
std::vector<glm::vec2> ShapeOutline(Shape::Geometry form, float cornerRadius, const UIRect& r,
                                    float offset = 0.0f);

// Цвет заливки в точке p (градиент — по своему правилу, сплошная — один цвет).
glm::vec4 ShapeFillColor(const Shape& shape, const UIRect& r, glm::vec2 p);

// Центр формы — вершина веера заливки.
inline glm::vec2 ShapeCenter(const UIRect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

} // namespace sage::ui
