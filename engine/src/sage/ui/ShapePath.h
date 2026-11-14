#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/ui/UIAnchor.h"
#include "sage/ui/components/Visual.h"

// ---------------------------------------------------------------------------
// СВОЙ КОНТУР ФОРМЫ (Shape::Geometry::Custom): путь, его кривые, заливка с
// отверстиями, обводка с соединениями и концами, «точка внутри».
//
// Всё здесь — математика без графики: путь разбирается, кривые разбиваются
// на отрезки, область режется на треугольники, обводка строится
// треугольниками. Рисует готовые треугольники UIRenderer::FillTriangles, и
// ему всё равно, откуда они взялись.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Точка контура: положение и ручки Безье (смещения от точки; нули — угол).
struct PathPoint {
    glm::vec2 P{0.0f};
    glm::vec2 In{0.0f};    // ручка к предыдущей точке
    glm::vec2 Out{0.0f};   // ручка к следующей
    bool Curved() const { return In != glm::vec2(0.0f) || Out != glm::vec2(0.0f); }
};

struct PathContour {
    std::vector<PathPoint> Points;
    bool Closed = true;
};

// Разбор и запись пути (Shape::Path). Пустой путь — прямоугольник 0..1.
// Неразборчивое — false, а out получает то, что успело разобраться.
bool ParseShapePath(const std::string& text, std::vector<PathContour>& out);
std::string FormatShapePath(const std::vector<PathContour>& contours);

// Ломаные контуров в координатах прямоугольника r (кривые — отрезками).
struct Polyline {
    std::vector<glm::vec2> Points;
    bool Closed = true;
};
std::vector<Polyline> FlattenShapePath(const std::vector<PathContour>& contours, const UIRect& r);

// Замкнутые ломаные формы ЛЮБОЙ геометрии в r — основа заливки, маски и
// попадания мышью. Открытые контуры своей формы не входят: залить их нечем.
std::vector<std::vector<glm::vec2>> ShapeRings(const Shape& shape, const UIRect& r);

// Треугольники области по правилу «чёт-нечет» (контур внутри контура —
// отверстие). По три точки на треугольник.
std::vector<glm::vec2> TriangulateRings(const std::vector<std::vector<glm::vec2>>& rings);

// Обводка ломаной треугольниками. Полоса лежит от offset0 до offset1 по
// ЛЕВОЙ нормали хода (ось Y вниз): у контура с положительной площадью
// (по часовой на экране) левая нормаль смотрит НАРУЖУ.
std::vector<glm::vec2> StrokeTriangles(const std::vector<glm::vec2>& pts, bool closed, float offset0,
                                       float offset1, Shape::Join join, Shape::Cap cap);

// Ориентированная площадь (> 0 — по часовой при оси Y вниз).
float SignedArea(const std::vector<glm::vec2>& ring);

// Какие контуры — отверстия (лежат внутри нечётного числа других).
std::vector<bool> RingHoles(const std::vector<std::vector<glm::vec2>>& rings);

// Точка внутри формы (по правилу «чёт-нечет»).
bool ShapeContains(const Shape& shape, const UIRect& r, glm::vec2 p);

// Альфа-маска формы w x h (0..255), со сглаживанием края: по ней режут
// детей Panel с формой (Clip to Shape).
std::vector<unsigned char> RasterizeShape(const Shape& shape, int w, int h);

} // namespace sage::ui
