#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// КОНТРОЛЛЕР ПЕРСОНАЖА И ФИЗИЧЕСКИЕ ТЕЛА (основной бэкенд — Jolt).
//
// Жалоба: «пинает и дёргается, если на нём стоять; коллизии неточные». Здесь
// то, что видит игрок, числами: стоит ли персонаж на ящике спокойно, не
// улетает ли ящик, в который вошли, и не проходит ли ящик сквозь персонажа.
// ---------------------------------------------------------------------------

using namespace sage::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

GameObject StaticFloor(Scene& scene) {
    GameObject floor = scene.CreateObject("Floor");
    floor.GetTransform().Position = {0.0f, -0.5f, 0.0f};
    floor.GetTransform().Scale = {40.0f, 1.0f, 40.0f};
    scene.Registry().emplace<RigidBodyComponent>(floor.Entity(), RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(floor.Entity());
    return floor;
}

GameObject Crate(Scene& scene, const glm::vec3& pos, float mass) {
    GameObject box = scene.CreateObject("Crate");
    box.GetTransform().Position = pos;
    RigidBodyComponent rb{BodyType::Dynamic};
    rb.Mass = mass;
    scene.Registry().emplace<RigidBodyComponent>(box.Entity(), rb);
    scene.Registry().emplace<ColliderComponent>(box.Entity());
    return box;
}

GameObject Hero(Scene& scene, const glm::vec3& pos) {
    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = pos;
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    return hero;
}

} // namespace

// Стоять на динамическом ящике — спокойно: ни персонаж, ни ящик не дрожат.
TEST(CharacterBodies_standing_on_a_crate_is_still) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("StandOnCrate");
    StaticFloor(scene);
    GameObject box = Crate(scene, {0.0f, 0.5f, 0.0f}, 20.0f);
    GameObject hero = Hero(scene, {0.0f, 1.05f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    for (int i = 0; i < 90; ++i) physics.Step(scene, kDt);   // устоялись

    float heroLo = 1e9f, heroHi = -1e9f, boxMove = 0.0f, boxSpeed = 0.0f;
    int airborne = 0;
    const glm::vec3 boxStart = box.GetTransform().Position;
    glm::vec3 boxPrev = boxStart;
    for (int i = 0; i < 240; ++i) {
        physics.Step(scene, kDt);
        const float y = hero.GetTransform().Position.y;
        heroLo = std::min(heroLo, y);
        heroHi = std::max(heroHi, y);
        const glm::vec3 b = box.GetTransform().Position;
        boxMove = std::max(boxMove, glm::length(b - boxStart));
        boxSpeed = std::max(boxSpeed, glm::length(b - boxPrev) / kDt);
        boxPrev = b;
        if (!scene.Registry().get<CharacterControllerComponent>(hero.Entity()).Grounded) ++airborne;
    }
    std::printf("       на ящике: персонаж ходит по высоте на %.4f м, ящик сдвинулся на %.4f м, "
                "скорость ящика до %.3f м/с, кадров «в воздухе» %d\n",
                heroHi - heroLo, boxMove, boxSpeed, airborne);
    CHECK_TRUE(heroLo > 0.95f);                // стоит НА ящике, а не провалился в него
    CHECK_TRUE(heroHi - heroLo < 0.005f);
    CHECK_TRUE(boxMove < 0.01f);
    CHECK_TRUE(boxSpeed < 0.05f);
    CHECK_EQ(airborne, 0);
}

// Войти в ящик — толкнуть его шагом, а не пнуть: ящик не быстрее идущего и
// не подлетает.
TEST(CharacterBodies_walking_into_a_crate_pushes_it_gently) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("PushCrate");
    StaticFloor(scene);
    GameObject box = Crate(scene, {1.5f, 0.5f, 0.0f}, 20.0f);
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    for (int i = 0; i < 30; ++i) physics.Step(scene, kDt);
    const float walk = 3.0f;
    float boxSpeed = 0.0f, boxLift = 0.0f;
    glm::vec3 prev = box.GetTransform().Position;
    for (int i = 0; i < 120; ++i) {
        auto& cc = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
        cc.DesiredVelocity = {walk, 0.0f, 0.0f};
        cc.HasRequest = true;
        physics.Step(scene, kDt);
        const glm::vec3 b = box.GetTransform().Position;
        boxSpeed = std::max(boxSpeed, glm::length(b - prev) / kDt);
        boxLift = std::max(boxLift, b.y - 0.5f);
        prev = b;
    }
    std::printf("       толчок: ящик до %.2f м/с (персонаж идёт %.1f), подлетел на %.3f м, сдвинут на %.2f м\n",
                boxSpeed, walk, boxLift, box.GetTransform().Position.x - 1.5f);
    CHECK_TRUE(box.GetTransform().Position.x > 1.6f);   // толкнули
    CHECK_TRUE(boxSpeed < walk * 1.25f);                // но не пнули
    CHECK_TRUE(boxLift < 0.05f);
}

// Ящик, упавший на персонажа, не проходит сквозь него: персонаж — твёрдое
// препятствие и для тел, а не только тела для персонажа.
TEST(CharacterBodies_a_falling_crate_does_not_pass_through_the_character) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("CrateOnHead");
    StaticFloor(scene);
    GameObject hero = Hero(scene, {0.0f, 0.0f, 0.0f});
    GameObject box = Crate(scene, {0.0f, 4.0f, 0.0f}, 5.0f);
    PhysicsScene physics(Backend::Jolt, scene);
    if (!physics.SupportsCharacters()) return;
    float lowest = 1e9f;
    for (int i = 0; i < 120; ++i) {
        physics.Step(scene, kDt);
        const glm::vec3 b = box.GetTransform().Position;
        // Пока ящик над капсулой, ниже макушки (1.8) его центр не опускается.
        if (std::abs(b.x) < 0.3f && std::abs(b.z) < 0.3f) lowest = std::min(lowest, b.y);
    }
    std::printf("       ящик над персонажем опустился до %.2f м (макушка 1.8)\n", lowest);
    CHECK_TRUE(lowest > 1.8f + 0.4f);
}

// Лёгкие предметы — то, на что встают в играх чаще всего (ящик, бочка). Раньше
// персонаж выдавливал килограммовый ящик из-под себя: тот вылетал со
// скоростью 9–11 м/с, а персонаж падал на пол.
TEST(CharacterBodies_light_crates_hold_whether_standing_or_walking_on_top) {
    if (!PhysicsWorld::HasJolt()) return;
    for (float mass : {1.0f, 3.0f, 10.0f}) {
        for (int walkOnTop = 0; walkOnTop < 2; ++walkOnTop) {
            Scene scene("LightCrate");
            StaticFloor(scene);
            GameObject box = Crate(scene, {0.0f, 0.5f, 0.0f}, mass);
            GameObject hero = Hero(scene, {0.0f, 1.05f, 0.0f});
            PhysicsScene physics(Backend::Jolt, scene);
            if (!physics.SupportsCharacters()) return;
            for (int i = 0; i < 60; ++i) physics.Step(scene, kDt);
            float lo = 1e9f, hi = -1e9f, boxSpeed = 0.0f;
            int airborne = 0;
            glm::vec3 prev = box.GetTransform().Position;
            for (int i = 0; i < 180; ++i) {
                auto& cc = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
                if (walkOnTop) {   // топчется по крышке
                    cc.DesiredVelocity = {0.3f * std::sin(i * 0.1f), 0.0f, 0.3f * std::cos(i * 0.1f)};
                    cc.HasRequest = true;
                }
                physics.Step(scene, kDt);
                lo = std::min(lo, hero.GetTransform().Position.y);
                hi = std::max(hi, hero.GetTransform().Position.y);
                boxSpeed = std::max(boxSpeed, glm::length(box.GetTransform().Position - prev) / kDt);
                prev = box.GetTransform().Position;
                if (!cc.Grounded) ++airborne;
            }
            std::printf("       ящик %.0f кг, %s: высота %.3f..%.3f, ящик до %.2f м/с, кадров в воздухе %d\n", mass,
                        walkOnTop ? "ходит" : "стоит", lo, hi, boxSpeed, airborne);
            CHECK_TRUE(lo > 0.99f);
            CHECK_TRUE(hi - lo < 0.01f);
            CHECK_TRUE(boxSpeed < 0.1f);
            CHECK_EQ(airborne, 0);
        }
    }
}
