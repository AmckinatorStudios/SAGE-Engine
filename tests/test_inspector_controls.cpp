// ===========================================================================
//  ЕДИНЫЕ КОНТРОЛЫ ИНСПЕКТОРА (editor/src/InspectorControls.h): шаг и формат по
//  диапазону поля, связь пропорций размера.
// ===========================================================================
#include "TestFramework.h"

#include <cstring>

#include "../editor/src/InspectorControls.h"

using namespace sage::editor;

TEST(InspectorControls_step_follows_the_range) {
    const ictl::NumOpts frac = ictl::OptsForRange(0.0f, 1.0f);
    CHECK_NEAR(frac.Step, 0.01f, 1e-6f);
    CHECK_TRUE(std::strcmp(frac.Format, "%.2f") == 0);
    CHECK_NEAR(ictl::OptsForRange(0.0f, 16.0f).Step, 0.1f, 1e-6f);
    const ictl::NumOpts px = ictl::OptsForRange(0.0f, 4096.0f);
    CHECK_NEAR(px.Step, 1.0f, 1e-6f);
    CHECK_TRUE(std::strcmp(px.Format, "%.0f") == 0);
    // Почти без границ — без округления до целых (значение шкалы 0.7 не «1»).
    CHECK_TRUE(std::strcmp(ictl::OptsForRange(-100000.0f, 100000.0f).Format, "%.0f") != 0);
}

TEST(InspectorControls_linked_size_keeps_the_ratio) {
    ictl::NumOpts o;
    o.Min = 0.0f;
    o.Max = 8192.0f;
    const glm::vec2 a = ictl::LinkedSize({240, 80}, {480, 80}, o);
    CHECK_NEAR(a.y, 160.0f, 1e-4f);
    const glm::vec2 b = ictl::LinkedSize({240, 80}, {240, 40}, o);
    CHECK_NEAR(b.x, 120.0f, 1e-4f);
    // Нулевая сторона — соотношения нет, правка как есть.
    const glm::vec2 c = ictl::LinkedSize({0, 80}, {50, 80}, o);
    CHECK_NEAR(c.y, 80.0f, 1e-4f);
    // Пределы соблюдаются.
    const glm::vec2 d = ictl::LinkedSize({100, 1000}, {10000, 1000}, o);
    CHECK_NEAR(d.y, 8192.0f, 1e-4f);
}
