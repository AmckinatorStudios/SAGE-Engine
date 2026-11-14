// ===========================================================================
//  ГЕОМЕТРИЯ ФОРМЫ (sage/ui/ShapeGeometry.h) — без графики.
//
//  Контуры с разным сдвигом обязаны совпадать по числу точек: из них
//  строятся полосы обводки и сглаживания, и несовпадение рвёт полосу.
// ===========================================================================
#include "TestFramework.h"

#include <cmath>

#include "sage/scene/Scene.h"
#include "sage/ui/ShapeGeometry.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"

using sage::ui::Shape;
using sage::ui::UIRect;

TEST(UIShape_outlines_with_any_offset_have_the_same_points) {
    const UIRect r{10, 20, 200, 100};
    for (Shape::Geometry g : {Shape::Geometry::Rectangle, Shape::Geometry::RoundedRectangle,
                              Shape::Geometry::Circle, Shape::Geometry::Ellipse}) {
        const size_t n = sage::ui::ShapeOutline(g, 16.0f, r).size();
        CHECK_TRUE(n >= 4);
        for (float off : {-30.0f, -8.0f, -0.5f, 0.5f, 6.0f})
            CHECK_EQ(sage::ui::ShapeOutline(g, 16.0f, r, off).size(), n);
    }
}

TEST(UIShape_circle_lies_inside_the_smaller_side) {
    const UIRect r{0, 0, 200, 100};
    for (glm::vec2 p : sage::ui::ShapeOutline(Shape::Geometry::Circle, 0.0f, r))
        CHECK_NEAR(glm::length(p - glm::vec2(100, 50)), 50.0f, 1e-3f);
    // Эллипс — во весь прямоугольник: крайняя правая точка на правом краю.
    float maxX = 0.0f;
    for (glm::vec2 p : sage::ui::ShapeOutline(Shape::Geometry::Ellipse, 0.0f, r)) maxX = std::max(maxX, p.x);
    CHECK_NEAR(maxX, 200.0f, 1e-3f);
}

TEST(UIShape_rounded_corner_is_clamped_and_grows_with_offset) {
    const UIRect r{0, 0, 100, 40};
    // Скругление больше половины стороны — не больше 20: иначе форма ломается.
    for (glm::vec2 p : sage::ui::ShapeOutline(Shape::Geometry::RoundedRectangle, 500.0f, r)) {
        CHECK_TRUE(p.x >= -1e-3f && p.x <= 100.0f + 1e-3f);
        CHECK_TRUE(p.y >= -1e-3f && p.y <= 40.0f + 1e-3f);
    }
    // Контур, сдвинутый наружу на 5, — на 5 дальше от исходного во всех точках
    // прямых сторон: обводка ровной толщины.
    const auto out = sage::ui::ShapeOutline(Shape::Geometry::Rectangle, 0.0f, r, 5.0f);
    CHECK_NEAR(out[0].x, -5.0f, 1e-4f);
    CHECK_NEAR(out[2].y, 45.0f, 1e-4f);
}

TEST(UIShape_gradients_run_from_start_to_end) {
    Shape s;
    s.Color = {1, 0, 0, 1};
    s.Color2 = {0, 0, 1, 1};
    const UIRect r{0, 0, 200, 100};
    s.Fill = Shape::FillKind::LinearGradient;
    s.GradientAngle = 0.0f;
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {0, 50}).r, 1.0f, 1e-4f);
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {200, 50}).b, 1.0f, 1e-4f);
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {100, 50}).r, 0.5f, 1e-4f);
    s.GradientAngle = 90.0f;   // сверху вниз
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {0, 100}).b, 1.0f, 1e-4f);
    s.Fill = Shape::FillKind::RadialGradient;
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {100, 50}).r, 1.0f, 1e-4f);   // центр
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {200, 50}).b, 1.0f, 1e-4f);   // край
    s.Fill = Shape::FillKind::None;
    CHECK_NEAR(sage::ui::ShapeFillColor(s, r, {100, 50}).a, 0.0f, 1e-4f);
}

TEST(UIShape_is_an_element_and_panel_appearance) {
    // Форма — самостоятельный элемент (без детей) и оформление Panel по
    // умолчанию; оба — один компонент.
    const sage::ui::Preset* shape = sage::ui::FindPreset("Shape");
    const sage::ui::Preset* panel = sage::ui::FindPreset("Panel");
    CHECK_TRUE(shape && shape->HasShape && !shape->Container);
    CHECK_TRUE(panel && panel->HasShape && panel->Container && !panel->HasFill);

    Scene scene("S");
    GameObject e = scene.CreateEmptyObject("S");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, e.Entity(), "Shape"));
    CHECK_TRUE(sage::ui::InferType(scene.Registry(), e.Entity()) == "Shape");
    CHECK_FALSE(sage::ui::HoldsChildren(scene.Registry(), e.Entity()));
}

// ===========================================================================
//  СВОЙ КОНТУР (sage/ui/ShapePath.h)
// ===========================================================================
#include "sage/ui/ShapePath.h"
#include "sage/ui/UIMasks.h"

namespace {
float TriArea(const std::vector<glm::vec2>& t) {
    float a = 0.0f;
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
        const glm::vec2 u = t[i + 1] - t[i], v = t[i + 2] - t[i];
        a += std::abs(u.x * v.y - u.y * v.x) * 0.5f;
    }
    return a;
}
} // namespace

TEST(UIShape_path_survives_text_and_back) {
    // Квадрат с кривой стороной и отдельный открытый контур.
    const std::string path = "M 0 0 L 1 0 C 1 0.3 1 0.7 1 1 L 0 1 Z M 0.2 0.5 L 0.8 0.5";
    std::vector<sage::ui::PathContour> c;
    CHECK_TRUE(sage::ui::ParseShapePath(path, c));
    CHECK_EQ((int)c.size(), 2);
    CHECK_EQ((int)c[0].Points.size(), 4);
    CHECK_TRUE(c[0].Closed);
    CHECK_FALSE(c[1].Closed);
    CHECK_NEAR(c[0].Points[1].Out.y, 0.3f, 1e-5f);   // ручка выхода второй точки
    CHECK_NEAR(c[0].Points[2].In.y, -0.3f, 1e-5f);   // ручка входа третьей
    std::vector<sage::ui::PathContour> back;
    CHECK_TRUE(sage::ui::ParseShapePath(sage::ui::FormatShapePath(c), back));
    CHECK_TRUE(sage::ui::FormatShapePath(back) == sage::ui::FormatShapePath(c));
    // Кривая, замыкающая контур, — ручка первой точки, а не лишняя точка.
    CHECK_TRUE(sage::ui::ParseShapePath("M 0 0 L 1 0 L 1 1 C 0.5 1 0 0.5 0 0 Z", back));
    CHECK_EQ((int)back[0].Points.size(), 3);
    CHECK_FALSE(sage::ui::ParseShapePath("M 0 0 Q 1 1", back));   // неизвестная команда — ошибка
}

TEST(UIShape_contour_inside_another_is_a_hole) {
    // Квадрат 100x100 с квадратным отверстием 50x50 в середине.
    Shape s;
    s.Form = Shape::Geometry::Custom;
    s.Path = "M 0 0 L 1 0 L 1 1 L 0 1 Z M 0.25 0.25 L 0.75 0.25 L 0.75 0.75 L 0.25 0.75 Z";
    const UIRect r{0, 0, 100, 100};
    const auto rings = sage::ui::ShapeRings(s, r);
    CHECK_EQ((int)rings.size(), 2);
    CHECK_NEAR(TriArea(sage::ui::TriangulateRings(rings)), 7500.0f, 1.0f);   // 10000 - 2500
    CHECK_TRUE(sage::ui::ShapeContains(s, r, {10, 10}));
    CHECK_FALSE(sage::ui::ShapeContains(s, r, {50, 50}));   // в отверстии
    const auto holes = sage::ui::RingHoles(rings);
    CHECK_FALSE(holes[0]);
    CHECK_TRUE(holes[1]);
    // Вогнутый контур (буква «Г») режется ушами без лишней площади.
    const std::vector<std::vector<glm::vec2>> ell = {{{0, 0}, {10, 0}, {10, 2}, {2, 2}, {2, 10}, {0, 10}}};
    CHECK_NEAR(TriArea(sage::ui::TriangulateRings(ell)), 36.0f, 0.01f);
}

TEST(UIShape_stroke_has_joins_and_caps) {
    const std::vector<glm::vec2> zig = {{0, 0}, {10, 0}, {10, 10}};
    using J = Shape::Join;
    using C = Shape::Cap;
    const auto bevel = sage::ui::StrokeTriangles(zig, false, -1, 1, J::Bevel, C::Butt);
    const auto round = sage::ui::StrokeTriangles(zig, false, -1, 1, J::Round, C::Butt);
    const auto capped = sage::ui::StrokeTriangles(zig, false, -1, 1, J::Bevel, C::Round);
    CHECK_EQ((int)(bevel.size() % 3), 0);
    CHECK_TRUE(round.size() > bevel.size());    // дуга — больше треугольников
    CHECK_TRUE(capped.size() > bevel.size());   // круглые концы
    // Площадь обводки: два отрезка по 10 толщиной 2 плюс угол.
    CHECK_TRUE(TriArea(bevel) > 39.0f && TriArea(bevel) < 42.0f);
}

TEST(UIShape_rasterized_mask_has_soft_edges) {
    Shape circle;
    circle.Form = Shape::Geometry::Circle;
    const auto a = sage::ui::RasterizeShape(circle, 32, 32);
    CHECK_EQ((int)a.size(), 32 * 32);
    CHECK_EQ((int)a[16 * 32 + 16], 255);   // центр
    CHECK_EQ((int)a[0], 0);                // угол
    bool partial = false;
    for (unsigned char v : a) partial = partial || (v > 10 && v < 245);
    CHECK_TRUE(partial);                   // край сглажен
}

TEST(UIShape_clip_to_shape_limits_the_mouse) {
    // Круглая Panel с обрезкой: кнопка во весь её квадрат нажимается только
    // в круге — угол, которого не видно, мышь не ловит.
    Scene scene("clip");
    GameObject panel = scene.CreateEmptyObject("Avatar");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, panel.Entity(), "Panel"));
    auto& reg = scene.Registry();
    reg.get<sage::ui::Element>(panel.Entity()).Anchor = UIAnchor::TopLeft;
    reg.get<sage::ui::Element>(panel.Entity()).Position = {0, 0};
    reg.get<sage::ui::Element>(panel.Entity()).Size = {100, 100};
    reg.get<sage::ui::Shape>(panel.Entity()).Form = Shape::Geometry::Circle;
    reg.emplace<sage::ui::Mask>(panel.Entity()).Form = sage::ui::Mask::Shape::Auto;
    GameObject btn = scene.CreateEmptyObject("Btn");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, btn.Entity(), "Button"));
    reg.get<sage::ui::Element>(btn.Entity()).Anchor = UIAnchor::TopLeft;
    reg.get<sage::ui::Element>(btn.Entity()).Position = {0, 0};
    reg.get<sage::ui::Element>(btn.Entity()).Size = {100, 100};
    scene.SetParent(btn.Entity(), panel.Entity());
    const int center = sage::ui::HitTest(scene, 50, 50, 800, 600);
    const int corner = sage::ui::HitTest(scene, 3, 3, 800, 600);
    CHECK_TRUE(center >= 0 && center != panel.Id());   // кнопка (или её надпись)
    CHECK_TRUE(corner != center);                       // в углу — уже не кнопка
}
