#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// КОНТРОЛЛЕР ПЕРСОНАЖА И ГЕОМЕТРИЯ УРОВНЯ.
//
// Уровень собран так, как его собирают в редакторе: стены, пол, потолок —
// объекты с моделью и Mesh Collider, без Rigid Body. Персонаж — своей
// капсулой контроллера, а не моделью: видимое тело в столкновениях не
// участвует (у героя есть меш с коллайдером по мешу — он не мешает ему идти).
// ---------------------------------------------------------------------------

using namespace sage::physics;

namespace {
constexpr float kDt = 1.0f / 60.0f;

GameObject Block(Scene& scene, const glm::vec3& pos, const glm::vec3& scale, const glm::vec3& euler = {}) {
    GameObject g = scene.CreateObject("Level");
    g.EnsureRenderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    g.GetTransform().Position = pos;
    g.GetTransform().Scale = scale;
    g.GetTransform().Rotation = euler;
    ColliderComponent c;
    c.Shape = ShapeType::Mesh;
    scene.Registry().emplace<ColliderComponent>(g.Entity(), c);
    return g;
}

GameObject Floor(Scene& scene) { return Block(scene, {0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}); }

GameObject Hero(Scene& scene, const glm::vec3& pos) {
    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = pos;
    // Видимое тело героя с коллайдером по мешу — частая настройка из шаблона.
    hero.EnsureRenderer().Ref = MeshRef{MeshRef::Type::Capsule, ""};
    ColliderComponent own;
    own.Shape = ShapeType::Mesh;
    scene.Registry().emplace<ColliderComponent>(hero.Entity(), own);
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    return hero;
}

CharacterControllerComponent& Cc(Scene& scene, GameObject g) {
    return scene.Registry().get<CharacterControllerComponent>(g.Entity());
}

void Walk(PhysicsScene& physics, Scene& scene, GameObject hero, const glm::vec3& v, int frames) {
    for (int i = 0; i < frames; ++i) {
        Cc(scene, hero).DesiredVelocity = v;
        Cc(scene, hero).HasRequest = true;
        physics.Step(scene, kDt);
    }
}
} // namespace

// Стена из Mesh Collider без тела: персонаж упирается в неё, а не проходит.
TEST(CharacterLevel_stops_at_a_mesh_wall) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Wall");
    Floor(scene);
    Block(scene, {2.0f, 1.5f, 0.0f}, {0.2f, 3.0f, 6.0f});
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    Walk(physics, scene, hero, {4.0f, 0.0f, 0.0f}, 120);
    const float x = hero.GetTransform().Position.x;
    std::printf("       у стены: x = %.3f (грань стены 1.9, радиус 0.35)\n", x);
    CHECK_TRUE(x < 1.9f - 0.35f + 0.02f);
    CHECK_TRUE(x > 1.9f - 0.35f - 0.05f);
    CHECK_TRUE(Cc(scene, hero).Grounded);
}

// Угол из двух стен: персонаж, идущий в угол по диагонали, стоит в нём, а не
// дрожит и не просачивается.
TEST(CharacterLevel_rests_in_a_corner_without_jitter) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Corner");
    Floor(scene);
    Block(scene, {2.0f, 1.5f, 0.0f}, {0.2f, 3.0f, 6.0f});
    Block(scene, {0.0f, 1.5f, 2.0f}, {6.0f, 3.0f, 0.2f});
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    Walk(physics, scene, hero, {3.0f, 0.0f, 3.0f}, 120);
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (int i = 0; i < 60; ++i) {
        Walk(physics, scene, hero, {3.0f, 0.0f, 3.0f}, 1);
        lo = glm::min(lo, hero.GetTransform().Position);
        hi = glm::max(hi, hero.GetTransform().Position);
    }
    std::printf("       в углу: (%.3f, %.3f), дрожь %.4f м\n", hi.x, hi.z, glm::length(hi - lo));
    CHECK_TRUE(hi.x < 1.9f - 0.35f + 0.02f);
    CHECK_TRUE(hi.z < 1.9f - 0.35f + 0.02f);
    CHECK_TRUE(glm::length(hi - lo) < 0.01f);
}

// Узкий проход: шире капсулы — проходит, уже — нет.
TEST(CharacterLevel_narrow_passage_fits_only_when_wide_enough) {
    if (!PhysicsWorld::HasJolt()) return;
    for (float gap : {0.85f, 0.6f}) {
        Scene scene("Gap");
        Floor(scene);
        // Две стены вдоль X, между ними проход шириной gap (по Z).
        Block(scene, {3.0f, 1.5f, gap * 0.5f + 0.5f}, {2.0f, 3.0f, 1.0f});
        Block(scene, {3.0f, 1.5f, -gap * 0.5f - 0.5f}, {2.0f, 3.0f, 1.0f});
        GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
        PhysicsScene physics(Backend::Jolt, scene);
        if (!physics.SupportsCharacters()) return;
        Walk(physics, scene, hero, {3.0f, 0.0f, 0.0f}, 180);
        const float x = hero.GetTransform().Position.x;
        std::printf("       проход %.2f м: x = %.2f\n", gap, x);
        if (gap > 0.7f) CHECK_TRUE(x > 4.5f);   // прошёл насквозь
        // Упёрся в торцы стен: круглая капсула входит в щель 0.6 м лишь на
        // r - sqrt(r² - (gap/2)²) = 0.17 м — дальше её держат рёбра.
        else CHECK_TRUE(x < 2.0f - 0.35f + 0.17f + 0.02f);
    }
}

// Потолок: прыжок под низким потолком упирается в него — голова не уходит в
// плиту, и персонаж сразу падает обратно, а не «прилипает» к потолку.
TEST(CharacterLevel_ceiling_stops_a_jump) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Ceiling");
    Floor(scene);
    Block(scene, {0.0f, 2.5f, 0.0f}, {6.0f, 0.2f, 6.0f});   // низ плиты — 2.4
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    Walk(physics, scene, hero, {0.0f, 0.0f, 0.0f}, 30);
    Cc(scene, hero).VerticalVelocity = 8.0f;   // прыжок до ~3.3 м без потолка
    float top = 0.0f;
    int landedAt = -1;
    for (int i = 0; i < 120; ++i) {
        Walk(physics, scene, hero, {0.0f, 0.0f, 0.0f}, 1);
        top = std::max(top, hero.GetTransform().Position.y);
        if (i > 3 && landedAt < 0 && Cc(scene, hero).Grounded) landedAt = i;
    }
    std::printf("       под потолком: ноги поднялись до %.3f (потолок 2.4, рост 1.8), приземлился на кадре %d\n",
                top, landedAt);
    CHECK_TRUE(top + 1.8f < 2.4f + 0.02f);
    CHECK_TRUE(landedAt > 0 && landedAt < 40);
}

// Пандус из Mesh Collider (повёрнутая плита): персонаж поднимается и стоит на
// нём, не проваливаясь сквозь треугольники.
TEST(CharacterLevel_walks_up_a_mesh_ramp) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Ramp");
    Floor(scene);
    Block(scene, {4.0f, 0.5f, 0.0f}, {6.0f, 0.2f, 3.0f}, {0.0f, 0.0f, 20.0f});
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    Walk(physics, scene, hero, {2.0f, 0.0f, 0.0f}, 150);
    const glm::vec3 p = hero.GetTransform().Position;
    // Высота поверхности пандуса над точкой x: 0.5 + tan(20°)(x - 4) + 0.1/cos(20°).
    const float surface = 0.5f + std::tan(glm::radians(20.0f)) * (p.x - 4.0f) + 0.1f / std::cos(glm::radians(20.0f));
    std::printf("       пандус: x = %.2f, ноги %.3f, поверхность %.3f\n", p.x, p.y, surface);
    CHECK_TRUE(p.x > 3.0f);
    CHECK_NEAR(p.y, surface, 0.06f);
    CHECK_TRUE(Cc(scene, hero).Grounded);
}

// Падение с высоты на тонкий пол из сетки — без пролёта насквозь.
TEST(CharacterLevel_fast_fall_onto_a_thin_mesh_floor_does_not_tunnel) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Fall");
    Block(scene, {0.0f, -0.025f, 0.0f}, {10.0f, 0.05f, 10.0f});
    GameObject hero = Hero(scene, {0.0f, 40.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    Walk(physics, scene, hero, {0.0f, 0.0f, 0.0f}, 240);
    CHECK_NEAR(hero.GetTransform().Position.y, 0.0f, 0.03f);
    CHECK_TRUE(Cc(scene, hero).Grounded);
}
