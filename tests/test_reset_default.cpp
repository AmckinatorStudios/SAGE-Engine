#include "TestFramework.h"

#include "ResetDefault.h"
#include "sage/render/Material.h"
#include "sage/scene/Components.h"

// ---------------------------------------------------------------------------
// «Сбросить к значению по умолчанию» берёт значение у НОВОГО экземпляра того
// же компонента (ResetDefault.h), а не из таблицы, переписанной в редактор:
// таблица разошлась бы с компонентом при первой правке его полей.
// ---------------------------------------------------------------------------
TEST(reset_default_takes_the_value_a_new_component_starts_with) {
    LightComponent light;
    light.Intensity = 7.5f;
    light.Range = 99.0f;
    const LightComponent fresh{};
    const float* intensity = sage::editor::DefaultFieldOf(light, light.Intensity);
    CHECK_TRUE(intensity != nullptr);
    if (intensity) CHECK_EQ(*intensity, fresh.Intensity);
    const float* range = sage::editor::DefaultFieldOf(light, light.Range);
    if (range) CHECK_EQ(*range, fresh.Range);

    Transform tr;
    tr.Scale = glm::vec3(3.0f);
    const glm::vec3* scale = sage::editor::DefaultFieldOf(tr, tr.Scale);
    CHECK_TRUE(scale != nullptr);
    if (scale) CHECK_TRUE(*scale == glm::vec3(1.0f));

    Material m;
    m.Roughness = 0.01f;
    const float* rough = sage::editor::DefaultFieldOf(m, m.Roughness);
    if (rough) CHECK_EQ(*rough, Material{}.Roughness);

    // Поле чужого объекта — не поле этого: значения по умолчанию у него нет.
    float stray = 1.0f;
    CHECK_TRUE(sage::editor::DefaultFieldOf(light, stray) == nullptr);
}
