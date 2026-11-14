// Часть «Форма» (sage::ui::Shape): заливка и обводка по контуру формы
// (ShapeGeometry.h). Один код на элемент Shape и на Panel с оформлением Shape.
#include "UIPartsDetail.h"

#include <algorithm>

#include "sage/ui/ShapeGeometry.h"

namespace sage::ui::parts {
namespace {

// Ширина полосы сглаживания края — один экранный пиксель: столько SDF
// скруглённых квадов размывает свой край, и формы рядом с ними не выглядят
// «зубчатыми» соседями.
constexpr float kFringe = 1.0f;

glm::vec4 Faded(glm::vec4 c) { return {c.r, c.g, c.b, 0.0f}; }

// Полоса между сдвигами o0 и o1 одного цвета, с мягкими краями. Тоньше
// пикселя — полупрозрачная линия в пиксель: так тонкая обводка не исчезает
// и не мерцает от положения на сетке.
void Band(UIRenderer& ui, const Shape& s, const UIRect& r, float o0, float o1, glm::vec4 color) {
    float width = o1 - o0;
    if (width <= 0.0f || color.a <= 0.0f) return;
    if (width < kFringe) {
        color.a *= width / kFringe;
        const float mid = (o0 + o1) * 0.5f;
        o0 = mid - kFringe * 0.5f;
        o1 = mid + kFringe * 0.5f;
        width = kFringe;
    }
    const float h = kFringe * 0.5f;
    const auto in0 = ShapeOutline(s.Form, s.CornerRadius, r, o0 - h);
    const auto in1 = ShapeOutline(s.Form, s.CornerRadius, r, o0 + h);
    const auto out0 = ShapeOutline(s.Form, s.CornerRadius, r, o1 - h);
    const auto out1 = ShapeOutline(s.Form, s.CornerRadius, r, o1 + h);
    const std::vector<glm::vec4> solid(in1.size(), color), clear(in1.size(), Faded(color));
    ui.StripPolygon(in0, clear, in1, solid);
    if (width > kFringe) ui.StripPolygon(in1, solid, out0, solid);
    ui.StripPolygon(out0, solid, out1, clear);
}

} // namespace

void DrawShape(const PartDrawContext& c, const Shape& src, const UIRect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    UIRenderer& ui = *c.Ui;
    // Числа формы — в опорных единицах холста, как всё в интерфейсе.
    Shape s = src;
    s.CornerRadius *= c.Scale;
    const float stroke = std::max(src.StrokeWidth, 0.0f) * c.Scale;
    auto withAlpha = [&](glm::vec4 col) {
        col.a = AlphaOf(c, col.a);
        return col;
    };

    if (s.Fill != Shape::FillKind::None) {
        // Заливка — до середины полосы сглаживания, дальше край гаснет: форма
        // не становится толще на пиксель от собственного сглаживания.
        const auto ring = ShapeOutline(s.Form, s.CornerRadius, r, -kFringe * 0.5f);
        const auto edge = ShapeOutline(s.Form, s.CornerRadius, r, kFringe * 0.5f);
        std::vector<glm::vec4> colors(ring.size()), clear(ring.size());
        for (size_t i = 0; i < ring.size(); ++i) {
            colors[i] = withAlpha(ShapeFillColor(s, r, ring[i]));
            clear[i] = Faded(colors[i]);
        }
        const glm::vec2 center = ShapeCenter(r);
        ui.FillPolygon(center, withAlpha(ShapeFillColor(s, r, center)), ring, colors);
        ui.StripPolygon(ring, colors, edge, clear);
    }

    if (stroke > 0.0f) {
        float o0 = -stroke, o1 = 0.0f;   // внутри
        if (s.StrokeAlignment == Shape::StrokeAlign::Center) {
            o0 = -stroke * 0.5f;
            o1 = stroke * 0.5f;
        } else if (s.StrokeAlignment == Shape::StrokeAlign::Outside) {
            o0 = 0.0f;
            o1 = stroke;
        }
        Band(ui, s, r, o0, o1, withAlpha(s.StrokeColor));
    }
}

const char* const kGeometry[] = {SAGE_UI_TEXT("Rectangle"), SAGE_UI_TEXT("Rounded Rectangle"),
                                 SAGE_UI_TEXT("Circle"), SAGE_UI_TEXT("Ellipse")};
const char* const kShapeFill[] = {SAGE_UI_TEXT("None"), SAGE_UI_TEXT("Solid Color"),
                                  SAGE_UI_TEXT("Linear Gradient"), SAGE_UI_TEXT("Radial Gradient")};
const char* const kStrokeAlign[] = {SAGE_UI_TEXT("Inside"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("Outside")};

const std::vector<PartField>& ShapeFields() {
    using G = Shape::Geometry;
    using F = Shape::FillKind;
    // Каждой геометрии и каждой заливке — только свои поля: скругление — у
    // скруглённого прямоугольника, второй цвет — у градиента, угол — у
    // линейного.
    static const std::vector<PartField> f = {
        {"geometry", SAGE_UI_TEXT("Geometry"), PartField::Kind::Enum, offsetof(Shape, Form), 0.0f, 0.0f, nullptr,
         kGeometry, 4},
        {"cornerRadius", SAGE_UI_TEXT("Corner radius"), PartField::Kind::Float, offsetof(Shape, CornerRadius),
         0.0f, 256.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "geometry", (int)G::RoundedRectangle},
        {"fill", SAGE_UI_TEXT("Fill"), PartField::Kind::Enum, offsetof(Shape, Fill), 0.0f, 0.0f, nullptr,
         kShapeFill, 4},
        {"color", SAGE_UI_TEXT("Color"), PartField::Kind::Color, offsetof(Shape, Color), 0.0f, 1.0f,
         "The fill; for a gradient — where it starts (the centre of a radial one).", nullptr, 0,
         PartField::Widget::Auto, "fill", ShowIfNot((int)F::None)},
        {"color2", SAGE_UI_TEXT("End color"), PartField::Kind::Color, offsetof(Shape, Color2), 0.0f, 1.0f,
         nullptr, nullptr, 0, PartField::Widget::Auto, "fill",
         ShowIfAny({(int)F::LinearGradient, (int)F::RadialGradient})},
        {"gradientAngle", SAGE_UI_TEXT("Angle"), PartField::Kind::Float, offsetof(Shape, GradientAngle),
         -360.0f, 360.0f, "0 — left to right, 90 — top to bottom.", nullptr, 0, PartField::Widget::Auto,
         "fill", (int)F::LinearGradient},
        {"strokeWidth", SAGE_UI_TEXT("Stroke thickness"), PartField::Kind::Float, offsetof(Shape, StrokeWidth),
         0.0f, 64.0f, "0 — no stroke."},
        {"strokeColor", SAGE_UI_TEXT("Stroke color"), PartField::Kind::Color, offsetof(Shape, StrokeColor),
         0.0f, 1.0f},
        {"strokeAlign", SAGE_UI_TEXT("Stroke alignment"), PartField::Kind::Enum,
         offsetof(Shape, StrokeAlignment), 0.0f, 0.0f, "Where the stroke lies against the edge of the shape.",
         kStrokeAlign, 3},
    };
    return f;
}

void DrawShapePart(const PartDrawContext& c) {
    DrawShape(c, *static_cast<const Shape*>(c.Data), c.Rect);
}

} // namespace sage::ui::parts
