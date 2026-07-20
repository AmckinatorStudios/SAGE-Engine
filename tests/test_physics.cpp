// Модульные тесты расширенной физики — БЕЗ GL (физика независима от рендера):
// составные тела, соединения (Point/Distance/Hinge), поддержка бэкендов и
// сборка соединений через ECS (PhysicsScene). Джойнты проверяются на бэкенде по
// умолчанию (Jolt, если собран); если joints не поддержаны — ассерты
// соединений мягко пропускаются (тест всё равно проходит).
#include "TestFramework.h"

#include "sage/physics/PhysicsWorld.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

#include <glm/glm.hpp>

using namespace sage::physics;

namespace {
void StepFor(PhysicsWorld& w, float seconds) {
    const float dt = 1.0f / 60.0f;
    for (float t = 0; t < seconds; t += dt) w.Step(dt);
}
BodyHandle MakeFloor(PhysicsWorld& w) {
    BodyDesc f;
    f.Type = BodyType::Static;
    f.Shape = ShapeType::Box;
    f.HalfExtents = {10.0f, 0.5f, 10.0f}; // верх на y=0.5
    f.Position = {0, 0, 0};
    return w.CreateBody(f);
}
} // namespace

TEST(Physics_compound_body_rests_on_floor) {
    auto w = PhysicsWorld::Create(PhysicsWorld::DefaultBackend());
    w->SetGravity({0, -9.81f, 0});
    MakeFloor(*w);

    // Составное тело: два бокса, один над другим (низ y=-0.5, верх y=+1.5).
    BodyDesc d;
    d.Type = BodyType::Dynamic;
    d.Position = {0, 6, 0};
    d.Mass = 1.0f;
    ChildShape a; a.Shape = ShapeType::Box; a.HalfExtents = {0.5f, 0.5f, 0.5f}; a.Position = {0, 0, 0};
    ChildShape b; b.Shape = ShapeType::Box; b.HalfExtents = {0.5f, 0.5f, 0.5f}; b.Position = {0, 1, 0};
    d.Children = {a, b};
    BodyHandle body = w->CreateBody(d);
    CHECK_TRUE(body != kInvalidBody);

    StepFor(*w, 3.0f);

    glm::vec3 pos; glm::quat rot;
    w->GetBodyTransform(body, pos, rot);
    // Низ составного тела (центр − 0.5) должен лечь на пол (верх пола y=0.5) =>
    // центр ≈ 1.0. Допускаем разброс от разных бэкендов/оседания.
    CHECK_TRUE(pos.y > 0.5f);   // не провалилось сквозь пол
    CHECK_TRUE(pos.y < 1.6f);   // и осело, а не зависло
}

TEST(Physics_backend_reports_joint_support) {
    auto simple = PhysicsWorld::Create(Backend::Simple);
    CHECK_FALSE(simple->SupportsJoints()); // аркадный бэкенд — без соединений
    if (PhysicsWorld::HasJolt()) {
        auto jolt = PhysicsWorld::Create(Backend::Jolt);
        CHECK_TRUE(jolt->SupportsJoints());
    }
}

TEST(Physics_point_joint_pins_body_in_place) {
    auto w = PhysicsWorld::Create(PhysicsWorld::DefaultBackend());
    if (!w->SupportsJoints()) return; // Simple — нечего проверять
    w->SetGravity({0, -9.81f, 0});

    BodyDesc d; d.Type = BodyType::Dynamic; d.Position = {0, 5, 0}; d.Mass = 1.0f;
    d.HalfExtents = {0.2f, 0.2f, 0.2f};
    BodyHandle body = w->CreateBody(d);

    JointDesc j; j.Type = JointType::Point; j.BodyA = body; j.BodyB = kInvalidBody;
    j.Anchor = {0, 5, 0}; // прикрепляем центр тела к точке в мире
    JointHandle joint = w->CreateJoint(j);
    CHECK_TRUE(joint != kInvalidJoint);

    StepFor(*w, 2.0f);
    glm::vec3 pos; glm::quat rot;
    w->GetBodyTransform(body, pos, rot);
    // Точечный сустав держит центр у якоря — тело не падает.
    CHECK_NEAR(pos.y, 5.0f, 0.5f);
}

TEST(Physics_distance_joint_limits_fall) {
    auto w = PhysicsWorld::Create(PhysicsWorld::DefaultBackend());
    if (!w->SupportsJoints()) return;
    w->SetGravity({0, -9.81f, 0});

    BodyDesc d; d.Type = BodyType::Dynamic; d.Position = {0, 5, 0}; d.Mass = 1.0f;
    d.HalfExtents = {0.2f, 0.2f, 0.2f};
    BodyHandle body = w->CreateBody(d);

    JointDesc j; j.Type = JointType::Distance; j.BodyA = body; j.BodyB = kInvalidBody;
    j.Anchor = {0, 5, 0}; j.MinDistance = 0.0f; j.MaxDistance = 1.0f;
    CHECK_TRUE(w->CreateJoint(j) != kInvalidJoint);

    StepFor(*w, 3.0f);
    glm::vec3 pos; glm::quat rot;
    w->GetBodyTransform(body, pos, rot);
    // Трос длиной 1: тело провисает, но не дальше 1 м от якоря (y ≈ 4, не ниже).
    CHECK_TRUE(pos.y > 3.5f);
    CHECK_TRUE(pos.y < 5.1f);
}

TEST(Physics_hinge_swings_but_keeps_pivot) {
    auto w = PhysicsWorld::Create(PhysicsWorld::DefaultBackend());
    if (!w->SupportsJoints()) return;
    w->SetGravity({0, -9.81f, 0});

    // Тело на «руке» длиной 1 от якоря, петля вокруг оси Z в точке (0,5,0).
    BodyDesc d; d.Type = BodyType::Dynamic; d.Position = {1, 5, 0}; d.Mass = 1.0f;
    d.HalfExtents = {0.2f, 0.2f, 0.2f};
    BodyHandle body = w->CreateBody(d);

    JointDesc j; j.Type = JointType::Hinge; j.BodyA = body; j.BodyB = kInvalidBody;
    j.Anchor = {0, 5, 0}; j.Axis = {0, 0, 1};
    CHECK_TRUE(w->CreateJoint(j) != kInvalidJoint);

    StepFor(*w, 2.0f);
    glm::vec3 pos; glm::quat rot;
    w->GetBodyTransform(body, pos, rot);
    // «Рука» жёсткая: расстояние до якоря сохраняется ≈1 (маятник качается вниз).
    float dist = glm::length(pos - glm::vec3(0, 5, 0));
    CHECK_NEAR(dist, 1.0f, 0.3f);
    CHECK_TRUE(pos.y < 5.0f); // качнулось вниз под гравитацией
}

TEST(Physics_scene_builds_joints_from_components) {
    // ECS-путь: две сущности с телами, у второй JointComponent на первую.
    // PhysicsScene вторым проходом должен построить соединение.
    Scene scene("P");
    GameObject anchor = scene.CreateObject("Anchor");
    anchor.GetTransform().Position = {0, 5, 0};
    scene.Registry().emplace<RigidBodyComponent>(
        anchor.Entity(), RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(anchor.Entity());

    GameObject hang = scene.CreateObject("Hanging");
    hang.GetTransform().Position = {0, 4, 0};
    scene.Registry().emplace<RigidBodyComponent>(
        hang.Entity(), RigidBodyComponent{BodyType::Dynamic, 1.0f});
    scene.Registry().emplace<ColliderComponent>(hang.Entity());
    JointComponent jc;
    jc.Type = JointType::Distance;
    jc.TargetId = anchor.Id();
    jc.Anchor = {0, 0, 0};
    jc.MinDistance = 0.0f; jc.MaxDistance = 1.2f;
    scene.Registry().emplace<JointComponent>(hang.Entity(), jc);

    PhysicsScene phys(PhysicsWorld::DefaultBackend(), scene);
    if (!phys.SupportsJoints()) return; // Simple — соединения не строятся

    CHECK_EQ(phys.JointCount(), 1);
    // Прогоняем симуляцию: висящее тело держится тросом у якоря. Точка крепления
    // — центр тела (world 0,4,0), maxDistance=1.2, поэтому тело провисает не
    // ниже ≈4−1.2=2.8. Без соединения за 3 с оно улетело бы на десятки метров.
    for (int i = 0; i < 180; ++i) phys.Step(scene, 1.0f / 60.0f);
    float y = scene.Get(hang.Id()).GetTransform().Position.y;
    CHECK_TRUE(y > 2.6f);  // удержано тросом (иначе упало бы намного ниже)
    CHECK_TRUE(y < 4.1f);
}
