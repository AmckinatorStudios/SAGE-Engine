#pragma once
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/visual/UIGradient.h"

// ---------------------------------------------------------------------------
// ПРОЦЕДУРНЫЕ ФОРМЫ (§20 ТЗ).
//
// Круг, кольцо, дуга, треугольник, многоугольник, линия — это не картинки и не
// частные случаи прямоугольника. Раньше их не было вовсе: индикатор перезарядки
// рисовался «полосой», круглый аватар — квадратом, стрелка — символом шрифта.
//
// Форма — ОДИН компонент с полем «какая», а не десять компонентов: у всех у них
// один и тот же набор свойств (заливка, обводка, скругление концов), различается
// только геометрия, и разводить их по типам значило бы десять раз повторить
// одно и то же. Свою форму разработчик добавляет своим компонентом — ядро
// рисования об этом знать не обязано (§98).
// ---------------------------------------------------------------------------
namespace sage::ui {

struct UIShape : UIComponentOf<UIShape> {
    static const UIComponentType& StaticType();

    enum class Kind {
        Rectangle,
        RoundedRect,
        Circle,
        Ellipse,
        Line,
        Triangle,
        Polygon,
        Arc,
        Ring,
    };

    Kind Type = Kind::Circle;
    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
    UIGradient Gradient;

    UICorners Radius{8.0f};      // для Rectangle/RoundedRect
    float Thickness = 0.0f;      // 0 — заливка; > 0 — обводка/кольцо/дуга
    float StartAngle = 0.0f;     // градусы, для Arc/Ring/Polygon
    float SweepAngle = 360.0f;   // градусы, для Arc
    int Sides = 6;               // для Polygon
    float Softness = 0.0f;       // мягкость края

    // Для Line/Polygon: точки в ДОЛЯХ прямоугольника узла (0..1). В долях, а не
    // в пикселях: форма обязана переживать смену размера узла, иначе её нельзя
    // ни растянуть, ни положить в раскладку.
    std::vector<glm::vec2> Points;
};

const char* const* UIShapeKindNames();
int UIShapeKindCount();

} // namespace sage::ui
