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
