#include "sage/ui/ShapeGeometry.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

// Сколько отрезков на четверть дуги радиуса radius (в экранных пикселях):
// столько, чтобы отрезок был не длиннее пары пикселей, — гладко на глаз и
// без сотен вершин у мелкой кнопки.
int ArcSegments(float radius) {
    return std::clamp((int)std::ceil(radius * kPi * 0.5f / 2.0f), 2, 48);
}

// Дуга эллипса с центром c и радиусами (rx, ry) от угла a0 до a1 (радианы,
// ось Y вниз), без последней точки — её добавит следующая дуга.
void Arc(std::vector<glm::vec2>& out, glm::vec2 c, float rx, float ry, float a0, float a1, int n) {
    for (int i = 0; i < n; ++i) {
        const float a = a0 + (a1 - a0) * (float)i / (float)n;
        out.push_back({c.x + std::cos(a) * rx, c.y + std::sin(a) * ry});
    }
}

} // namespace

std::vector<glm::vec2> ShapeOutline(Shape::Geometry form, float cornerRadius, const UIRect& r,
                                    float offset) {
    std::vector<glm::vec2> out;
    const glm::vec2 c = ShapeCenter(r);
    switch (form) {
        case Shape::Geometry::Circle:
        case Shape::Geometry::Ellipse: {
            // Круг — вписанный в меньшую сторону; эллипс — во весь прямоугольник.
            float rx = r.w * 0.5f, ry = r.h * 0.5f;
            if (form == Shape::Geometry::Circle) rx = ry = std::min(rx, ry);
            // Число точек — по ИСХОДНОЙ форме: контуры с разным сдвигом (края
            // обводки, полоса сглаживания) обязаны совпадать по точкам.
            const float perimeter = 2.0f * kPi * std::sqrt((rx * rx + ry * ry) * 0.5f);
            const int n = std::clamp((int)std::ceil(perimeter / 2.5f), 24, 192);
            rx = std::max(rx + offset, 0.0f);
            ry = std::max(ry + offset, 0.0f);
            Arc(out, c, rx, ry, 0.0f, 2.0f * kPi, n);
            return out;
        }
        case Shape::Geometry::Rectangle:
        case Shape::Geometry::RoundedRectangle: {
            const float x0 = r.x - offset, y0 = r.y - offset;
            const float x1 = r.x + r.w + offset, y1 = r.y + r.h + offset;
            const float base = form == Shape::Geometry::RoundedRectangle
                                   ? std::min(std::max(cornerRadius, 0.0f), std::min(r.w, r.h) * 0.5f)
                                   : 0.0f;
            if (base <= 0.0f) {
                out = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
                return out;
            }
            // Сдвинутый контур скругляется на столько же больше (меньше):
            // обводка снаружи скруглённой кнопки ровной толщины по всему краю.
            // Число точек — по исходному радиусу (см. эллипс выше); сжатая в
            // ноль дуга просто сходится в угол.
            const float rad = std::max(base + offset, 0.0f);
            const int n = ArcSegments(base);
            // По часовой стрелке (ось Y вниз): левый верхний, правый верхний,
            // правый нижний, левый нижний угол.
            Arc(out, {x0 + rad, y0 + rad}, rad, rad, kPi, 1.5f * kPi, n);
            Arc(out, {x1 - rad, y0 + rad}, rad, rad, 1.5f * kPi, 2.0f * kPi, n);
            Arc(out, {x1 - rad, y1 - rad}, rad, rad, 0.0f, 0.5f * kPi, n);
            Arc(out, {x0 + rad, y1 - rad}, rad, rad, 0.5f * kPi, kPi, n);
            return out;
        }
    }
    return out;
}

glm::vec4 ShapeFillColor(const Shape& shape, const UIRect& r, glm::vec2 p) {
    switch (shape.Fill) {
        case Shape::FillKind::LinearGradient: {
            // Проекция точки на направление градиента, от одного края
            // прямоугольника до противоположного: при 45° градиент идёт из угла
            // в угол, а не обрывается на середине.
            const float a = glm::radians(shape.GradientAngle);
            const glm::vec2 dir{std::cos(a), std::sin(a)};
            const glm::vec2 c = ShapeCenter(r);
            const float half = std::abs(dir.x) * r.w * 0.5f + std::abs(dir.y) * r.h * 0.5f;
            const float t = half > 0.0f ? glm::dot(p - c, dir) / (2.0f * half) + 0.5f : 0.0f;
            return glm::mix(shape.Color, shape.Color2, std::clamp(t, 0.0f, 1.0f));
        }
        case Shape::FillKind::RadialGradient: {
            // По эллипсу, вписанному в прямоугольник: 0 в центре, 1 на краю.
            const glm::vec2 c = ShapeCenter(r);
            const glm::vec2 h{std::max(r.w * 0.5f, 1e-4f), std::max(r.h * 0.5f, 1e-4f)};
            const float t = glm::length((p - c) / h);
            return glm::mix(shape.Color, shape.Color2, std::clamp(t, 0.0f, 1.0f));
        }
        case Shape::FillKind::None: return glm::vec4(0.0f);
        default: return shape.Color;
    }
}

} // namespace sage::ui
