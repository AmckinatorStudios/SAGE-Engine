// Точка опоры (Transform::Pivot): поворот и масштаб вокруг неё, смена опоры
// без сдвига объекта, разложение матрицы с опорой (гизмо, физика), иерархия,
// сохранение сцены и старые сцены без опоры.
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace {
glm::vec3 Apply(const glm::mat4& m, const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); }

float MaxDiff(const glm::mat4& a, const glm::mat4& b) {
    float d = 0.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) d = std::max(d, std::abs(a[c][r] - b[c][r]));
    return d;
}
} // namespace

TEST(Pivot_zero_keeps_the_old_matrix) {
    Transform t;
    t.Position = {1, 2, 3};
    t.Rotation = {10, 20, 30};
    t.Scale = {2, 1, 0.5f};
    const glm::mat4 before = t.GetMatrix();
    CHECK_NEAR(before[3].x, 1.0f, 1e-6);
    CHECK_NEAR(before[3].y, 2.0f, 1e-6);
    CHECK_NEAR(before[3].z, 3.0f, 1e-6);
}

TEST(Pivot_rotation_turns_around_the_pivot) {
    Transform t;
    t.Position = {5, 0, 0};
    t.Pivot = {1, 0, 0};
    // Без поворота опора не сдвигает объект: Position значит то же, что и раньше.
    CHECK_NEAR(Apply(t.GetMatrix(), {0, 0, 0}).x, 5.0f, 1e-5);
    t.Rotation = {0, 90, 0};
    const glm::mat4 m = t.GetMatrix();
    // Сама опора стоит на месте...
    const glm::vec3 pivot = Apply(m, t.Pivot);
    CHECK_NEAR(pivot.x, 6.0f, 1e-5);
    CHECK_NEAR(pivot.z, 0.0f, 1e-5);
    // ...а начало объекта обходит её по кругу радиуса 1.
    const glm::vec3 origin = Apply(m, {0, 0, 0});
    CHECK_NEAR(glm::length(origin - pivot), 1.0f, 1e-5);
    CHECK_NEAR(origin.x, 6.0f, 1e-5);
    CHECK_NEAR(std::abs(origin.z), 1.0f, 1e-5);
}

TEST(Pivot_scale_grows_from_the_pivot) {
    // Опора внизу: объект растёт вверх, низ стоит на полу.
    Transform t;
    t.Pivot = {0, -0.5f, 0};
    t.Scale = {2, 2, 2};
    const glm::mat4 m = t.GetMatrix();
    CHECK_NEAR(Apply(m, {0, -0.5f, 0}).y, -0.5f, 1e-5);
    CHECK_NEAR(Apply(m, {0, 0.5f, 0}).y, 1.5f, 1e-5);
}

TEST(Pivot_change_does_not_move_the_object) {
    Transform t;
    t.Position = {1, 2, 3};
    t.Rotation = {15, 40, -20};
    t.Scale = {1.5f, 0.5f, 2.0f};
    t.Pivot = {0.2f, 0.1f, -0.3f};
    const glm::mat4 before = t.GetMatrix();
    t.SetPivotKeepPlace({-1.0f, 0.5f, 2.0f});
    CHECK_NEAR(MaxDiff(before, t.GetMatrix()), 0.0f, 1e-5);
    CHECK_NEAR(t.Pivot.z, 2.0f, 1e-6);
    t.SetPivotKeepPlace(glm::vec3(0.0f));   // сброс — тоже без сдвига
    CHECK_NEAR(MaxDiff(before, t.GetMatrix()), 0.0f, 1e-5);
}

TEST(Pivot_survives_matrix_decomposition) {
    // Гизмо и физика пишут объект матрицей: опора остаётся, матрица — та же.
    Transform t;
    t.Position = {3, -1, 2};
    t.Rotation = {30, 10, 5};
    t.Scale = {1, 2, 3};
    t.Pivot = {0.5f, 1.0f, -0.25f};
    const glm::mat4 m = t.GetMatrix();
    Transform u;
    u.Pivot = t.Pivot;
    u.SetFromMatrix(m);
    CHECK_NEAR(MaxDiff(m, u.GetMatrix()), 0.0f, 1e-4);
    CHECK_NEAR(u.Position.x, 3.0f, 1e-4);
    CHECK_NEAR(u.Position.y, -1.0f, 1e-4);
    CHECK_NEAR(u.Position.z, 2.0f, 1e-4);
}

TEST(Pivot_gizmo_rotation_keeps_the_pivot_in_place) {
    // Так работает гизмо: его матрица — объект, сдвинутый на опору; поворот
    // гизмо вокруг своего начала, объект — гизмо, сдвинутое обратно.
    Transform t;
    t.Position = {2, 0, 0};
    t.Pivot = {0, 1, 0};
    const glm::mat4 world = t.GetMatrix();
    const glm::mat4 gizmo = world * glm::translate(glm::mat4(1.0f), t.Pivot);
    const glm::mat4 turned = gizmo * glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    t.SetFromMatrix(turned * glm::translate(glm::mat4(1.0f), -t.Pivot));
    const glm::vec3 pivotWorld = Apply(t.GetMatrix(), t.Pivot);
    CHECK_NEAR(pivotWorld.x, 2.0f, 1e-4);
    CHECK_NEAR(pivotWorld.y, 1.0f, 1e-4);
    CHECK_NEAR(t.Rotation.z, 90.0f, 1e-3);
}

TEST(Pivot_works_through_the_hierarchy) {
    Scene scene("P");
    GameObject parent = scene.CreateObject("Parent");
    GameObject child = scene.CreateObject("Child");
    scene.SetParent(child.Entity(), parent.Entity());
    Transform& pt = parent.GetTransform();
    pt.Position = {10, 0, 0};
    pt.Pivot = {0, 0, 2};
    child.GetTransform().Position = {0, 0, 2};   // ребёнок стоит ровно в опоре родителя
    child.GetTransform().Pivot = {1, 0, 0};
    const glm::vec3 before = glm::vec3(scene.WorldMatrix(child.Entity())[3]);
    pt.Rotation = {0, 180, 0};
    // Родитель повернулся вокруг своей опоры — ребёнок в ней и остался.
    const glm::vec3 after = glm::vec3(scene.WorldMatrix(child.Entity())[3]);
    CHECK_NEAR(after.x, before.x, 1e-4);
    CHECK_NEAR(after.z, before.z, 1e-4);
    // Опора ребёнка в мире = опора ребёнка, перенесённая родителем.
    const glm::vec3 childPivot = Apply(scene.WorldMatrix(child.Entity()), child.GetTransform().Pivot);
    CHECK_NEAR(childPivot.x, 9.0f, 1e-4);   // повёрнутый на 180° родитель отражает +X в −X
}

TEST(Pivot_is_saved_and_loaded) {
    Scene scene("S");
    GameObject a = scene.CreateObject("WithPivot");
    a.GetTransform().Pivot = {0.25f, -0.5f, 1.0f};
    a.GetTransform().Rotation = {0, 45, 0};
    GameObject b = scene.CreateObject("NoPivot");
    const std::string text = SceneSerializer::SaveToString(scene);
    // Объект без опоры не пишет ключ: старые сцены не меняются при сохранении.
    CHECK_EQ(text.find("\"pivot\"") != std::string::npos, true);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    GameObject la = back->FindByName("WithPivot");
    GameObject lb = back->FindByName("NoPivot");
    CHECK_TRUE(la.Valid() && lb.Valid());
    if (!la.Valid() || !lb.Valid()) return;
    CHECK_NEAR(la.GetTransform().Pivot.y, -0.5f, 1e-6);
    CHECK_NEAR(la.GetTransform().Pivot.z, 1.0f, 1e-6);
    CHECK_NEAR(MaxDiff(back->WorldMatrix(la.Entity()), scene.WorldMatrix(a.Entity())), 0.0f, 1e-5);
    CHECK_TRUE(lb.GetTransform().Pivot == glm::vec3(0.0f));
}

TEST(Pivot_old_scene_without_key_loads_at_origin) {
    Scene scene("Old");
    GameObject a = scene.CreateObject("Box");
    a.GetTransform().Position = {1, 2, 3};
    a.GetTransform().Rotation = {0, 30, 0};
    std::string text = SceneSerializer::SaveToString(scene);
    CHECK_TRUE(text.find("\"pivot\"") == std::string::npos);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    GameObject la = back->FindByName("Box");
    CHECK_TRUE(la.Valid() && la.GetTransform().Pivot == glm::vec3(0.0f));
    if (la.Valid()) CHECK_NEAR(back->WorldMatrix(la.Entity())[3].x, 1.0f, 1e-5);
}
