// ---------------------------------------------------------------------------
// Триггер-зоны: кто вошёл, кто внутри, кто вышел.
//
// ЧТО ЛОМАЛОСЬ. Зона сообщала только о ТЕЛАХ, пересёкших её по сообщениям
// бэкенда. Персонаж (контроллер) телом мира не является — и игрок проходил
// сквозь «кнопку», «воду», «финиш» молча; это самый частый случай триггера
// вообще. Удалённый изнутри объект уходил без «вышел», и счётчик «сколько на
// плите» у скрипта навсегда оставался на единицу больше. Маски слоёв у зоны не
// было — «дверь открылась от упавшего ящика». «Пока внутри» (Stay) не было
// вовсе.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include "sage/physics/PhysicsScene.h"
#include "sage/physics/PhysicsWorld.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>

using namespace sage::physics;

namespace {

constexpr float kDt = 1.0f / 60.0f;

GameObject MakeFloor(Scene& scene) {
    GameObject floor = scene.CreateObject("Floor");
    floor.GetTransform().Position = {0.0f, -0.5f, 0.0f};
    floor.GetTransform().Scale = {40.0f, 1.0f, 40.0f};
    scene.Registry().emplace<RigidBodyComponent>(floor.Entity(), RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(floor.Entity());
    return floor;
}

// Зона 2×2×2 с центром в (x, 1, 0).
GameObject MakeZone(Scene& scene, float x) {
    GameObject zone = scene.CreateObject("Zone");
    zone.GetTransform().Position = {x, 1.0f, 0.0f};
    RigidBodyComponent rb{BodyType::Static};
    rb.Sensor = true;
    scene.Registry().emplace<RigidBodyComponent>(zone.Entity(), rb);
    ColliderComponent col;
    col.HalfExtents = {1.0f, 1.0f, 1.0f};
    scene.Registry().emplace<ColliderComponent>(zone.Entity(), col);
    return zone;
}

int Count(const PhysicsScene& p, entt::entity zone, entt::entity guest, bool begin) {
    int n = 0;
    for (const PhysicsScene::EntityContact& c : p.Contacts())
        if (c.Sensor && c.Begin == begin && c.A == zone && c.B == guest) ++n;
    return n;
}

} // namespace

// --- 1. Геометрия: капсула персонажа против формы зоны -------------------------
TEST(Trigger_capsule_touches_box_sphere_capsule_and_rotated_box) {
    BodyDesc box;
    box.Shape = ShapeType::Box;
    box.HalfExtents = {1.0f, 1.0f, 1.0f};
    // Капсула вертикальная, радиус 0.3: рядом с гранью — касается, дальше — нет.
    CHECK_TRUE(CapsuleTouchesBodyForTest({1.2f, 0, 0}, {1.2f, 1, 0}, 0.3f, box));
    CHECK_FALSE(CapsuleTouchesBodyForTest({1.4f, 0, 0}, {1.4f, 1, 0}, 0.3f, box));
    // Касается только ВЕРХНИЙ конец оси — середина отрезка далеко. Проверка
    // «по концам» или «по центру» тут бы ошиблась.
    CHECK_TRUE(CapsuleTouchesBodyForTest({0, -3, 0}, {0, -1.1f, 0}, 0.3f, box));

    // Повёрнутая на 45° коробка достаёт углом дальше, чем гранью.
    BodyDesc rotated = box;
    rotated.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
    CHECK_TRUE(CapsuleTouchesBodyForTest({1.6f, 0, 0}, {1.6f, 1, 0}, 0.3f, rotated));
    CHECK_FALSE(CapsuleTouchesBodyForTest({1.6f, 0, 0}, {1.6f, 1, 0}, 0.3f, box));

    BodyDesc sphere;
    sphere.Shape = ShapeType::Sphere;
    sphere.Radius = 1.0f;
    CHECK_TRUE(CapsuleTouchesBodyForTest({1.25f, -1, 0}, {1.25f, 1, 0}, 0.3f, sphere));
    CHECK_FALSE(CapsuleTouchesBodyForTest({1.35f, -1, 0}, {1.35f, 1, 0}, 0.3f, sphere));

    BodyDesc capsule;
    capsule.Shape = ShapeType::Capsule;
    capsule.Radius = 0.5f;
    capsule.HalfHeight = 2.0f;
    CHECK_TRUE(CapsuleTouchesBodyForTest({0.7f, 1.5f, 0}, {0.7f, 1.8f, 0}, 0.3f, capsule));
    CHECK_FALSE(CapsuleTouchesBodyForTest({0.7f, 3.0f, 0}, {0.7f, 3.5f, 0}, 0.3f, capsule));

    // Составная зона: касание любой части — касание зоны.
    BodyDesc compound;
    ChildShape far;
    far.Shape = ShapeType::Box;
    far.HalfExtents = {0.5f, 0.5f, 0.5f};
    far.Position = {5.0f, 0.0f, 0.0f};
    compound.Children.push_back(far);
    CHECK_TRUE(CapsuleTouchesBodyForTest({5.6f, 0, 0}, {5.6f, 1, 0}, 0.3f, compound));
    CHECK_FALSE(CapsuleTouchesBodyForTest({0, 0, 0}, {0, 1, 0}, 0.3f, compound));
}

// --- 2. ГЛАВНОЕ: персонаж входит в зону, стоит в ней и выходит -----------------
TEST(Trigger_character_enters_stays_and_leaves) {
    Scene scene("TriggerChar");
    MakeFloor(scene);
    GameObject zone = MakeZone(scene, 5.0f);
    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());

    PhysicsScene physics(Backend::Builtin, scene);
    if (!physics.Available() || !physics.SupportsCharacters()) return;
    physics.Step(scene, kDt);
    auto& cc = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    CHECK_TRUE(cc.Runtime != kInvalidCharacter);

    std::vector<entt::entity> inside;
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 0);

    // Переносим в зону.
    physics.SetCharacterPosition(cc.Runtime, {5.0f, 0.0f, 0.0f});
    physics.Step(scene, kDt);
    CHECK_EQ(Count(physics, zone.Entity(), hero.Entity(), true), 1);
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 1);
    CHECK_TRUE(inside[0] == hero.Entity());
    CHECK_TRUE(physics.TriggerOverlaps()[0].Entered);

    // Стоит — вход не повторяется, а «пока внутри» уже не первый шаг.
    physics.Step(scene, kDt);
    CHECK_EQ(Count(physics, zone.Entity(), hero.Entity(), true), 0);
    CHECK_EQ((int)physics.TriggerOverlaps().size(), 1);
    CHECK_FALSE(physics.TriggerOverlaps()[0].Entered);
    physics.TriggersOf(hero.Entity(), inside);
    CHECK_EQ((int)inside.size(), 1);

    // Уходит.
    physics.SetCharacterPosition(cc.Runtime, {-5.0f, 0.0f, 0.0f});
    physics.Step(scene, kDt);
    CHECK_EQ(Count(physics, zone.Entity(), hero.Entity(), false), 1);
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 0);
}

// --- 3. Удалённый изнутри гость — всё равно «вышел» ------------------------------
TEST(Trigger_guest_destroyed_inside_still_leaves) {
    Scene scene("TriggerDestroy");
    GameObject zone = MakeZone(scene, 0.0f);
    GameObject box = scene.CreateObject("Box");
    box.GetTransform().Position = {0.0f, 1.0f, 0.0f};   // создан уже внутри
    box.GetTransform().Scale = {0.4f, 0.4f, 0.4f};
    scene.Registry().emplace<RigidBodyComponent>(box.Entity());   // падает, но первый шаг — внутри
    scene.Registry().emplace<ColliderComponent>(box.Entity());

    PhysicsScene physics(Backend::Builtin, scene);
    if (!physics.Available() || !physics.SupportsContacts()) return;
    bool entered = false;
    for (int i = 0; i < 10 && !entered; ++i) {
        physics.Step(scene, kDt);
        entered = Count(physics, zone.Entity(), box.Entity(), true) > 0;
    }
    CHECK_TRUE(entered);

    const entt::entity gone = box.Entity();
    scene.Registry().destroy(gone);
    physics.Step(scene, kDt);
    CHECK_EQ(Count(physics, zone.Entity(), gone, false), 1);
    std::vector<entt::entity> inside;
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 0);
}

// --- 4. Маска слоёв: зона не замечает чужие слои --------------------------------
TEST(Trigger_mask_ignores_other_layers) {
    Scene scene("TriggerMask");
    MakeFloor(scene);
    GameObject zone = MakeZone(scene, 0.0f);
    scene.Registry().get<RigidBodyComponent>(zone.Entity()).TriggerMask = 1u << 2;   // только слой 3

    GameObject hero = scene.CreateObject("Hero");
    auto& cc = scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    cc.Layer = 1u << 1;                                                            // слой 2

    PhysicsScene physics(Backend::Builtin, scene);
    if (!physics.Available() || !physics.SupportsCharacters()) return;
    physics.Step(scene, kDt);
    physics.Step(scene, kDt);
    std::vector<entt::entity> inside;
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 0);

    // Тот же персонаж на нужном слое — замечен.
    cc.Layer = 1u << 2;
    physics.Step(scene, kDt);
    CHECK_EQ(physics.ObjectsInTrigger(zone.Entity(), inside), 1);
}

// --- 5. Маска зоны переживает сохранение сцены ---------------------------------
TEST(Trigger_mask_survives_save_and_load) {
    Scene scene("TriggerSave");
    GameObject zone = MakeZone(scene, 0.0f);
    scene.Registry().get<RigidBodyComponent>(zone.Entity()).TriggerMask = 0x5u;
    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    Scene& back = *loaded;
    bool found = false;
    for (auto e : back.Registry().view<RigidBodyComponent>()) {
        const auto& rb = back.Registry().get<RigidBodyComponent>(e);
        if (!rb.Sensor) continue;
        found = true;
        CHECK_EQ((unsigned)rb.TriggerMask, 0x5u);
    }
    CHECK_TRUE(found);
}

// --- Столкновения ПЕРСОНАЖА: события и маска слоёв ----------------------------
//
// Контроллер персонажа — не тело мира, и очередь контактов бэкенда о нём
// молчала: персонаж вставал на пол и упирался в стену, а скрипт не получал ни
// OnCollisionEnter, ни Exit. Маска CollidesWith при этом хранилась, но в шаг
// Jolt не передавалась — персонаж сталкивался со всеми слоями подряд.
namespace {
int Touches(const PhysicsScene& p, entt::entity hero, entt::entity other, bool begin) {
    int n = 0;
    for (const PhysicsScene::EntityContact& c : p.Contacts())
        if (!c.Sensor && c.Begin == begin && c.A == hero && c.B == other) ++n;
    return n;
}

GameObject MakeWall(Scene& scene, unsigned layer) {
    GameObject wall = scene.CreateObject("Wall");
    wall.GetTransform().Position = {2.0f, 1.0f, 0.0f};
    RigidBodyComponent rb{BodyType::Static};
    rb.Layer = layer;
    scene.Registry().emplace<RigidBodyComponent>(wall.Entity(), rb);
    ColliderComponent col;
    col.HalfExtents = {0.25f, 1.0f, 3.0f};
    scene.Registry().emplace<ColliderComponent>(wall.Entity(), col);
    return wall;
}

// Идёт на стену (+X) полторы секунды; возвращает, докуда дошёл.
float WalkIntoWall(Backend backend, LayerMask collidesWith, bool* touchedWall = nullptr) {
    Scene scene("CharWall");
    MakeFloor(scene);
    GameObject wall = MakeWall(scene, 1u << 2);
    GameObject hero = scene.CreateObject("Hero");
    auto& cc = scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    cc.CollidesWith = collidesWith;
    PhysicsScene physics(backend, scene);
    if (!physics.Available() || !physics.SupportsCharacters()) return -1.0f;
    bool touched = false;
    for (int i = 0; i < 90; ++i) {
        auto& c = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
        c.DesiredVelocity = {3.0f, 0.0f, 0.0f};
        c.HasRequest = true;
        physics.Step(scene, kDt);
        touched = touched || Touches(physics, hero.Entity(), wall.Entity(), true) > 0;
    }
    if (touchedWall) *touchedWall = touched;
    return hero.GetTransform().Position.x;
}
} // namespace

TEST(Character_collision_events_for_floor_and_wall) {
    for (Backend backend : {Backend::Builtin, Backend::Jolt}) {
        Scene scene("CharEvents");
        GameObject floor = MakeFloor(scene);
        GameObject hero = scene.CreateObject("Hero");
        hero.GetTransform().Position = {0.0f, 0.5f, 0.0f};   // падает на пол
        scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
        PhysicsScene physics(backend, scene);
        if (!physics.Available() || !physics.SupportsCharacters()) continue;

        int landed = 0;
        for (int i = 0; i < 60; ++i) {
            physics.Step(scene, kDt);
            landed += Touches(physics, hero.Entity(), floor.Entity(), true);
        }
        // Пол — ровно один вход, а не по событию на каждый шаг стояния.
        CHECK_EQ(landed, 1);
        const auto& cc = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
        CHECK_EQ((int)cc.Touching.size(), 1);

        // Подняли над полом — выход.
        physics.SetCharacterPosition(cc.Runtime, {0.0f, 5.0f, 0.0f});
        physics.Step(scene, kDt);
        CHECK_EQ(Touches(physics, hero.Entity(), floor.Entity(), false), 1);

        bool touchedWall = false;
        const float stopped = WalkIntoWall(backend, kAllLayers, &touchedWall);
        CHECK_TRUE(touchedWall);
        CHECK_TRUE(stopped < 1.8f);   // стена с x = 1.75 остановила
    }
}

TEST(Character_collides_with_mask_lets_it_walk_through_other_layers) {
    for (Backend backend : {Backend::Builtin, Backend::Jolt}) {
        const float blocked = WalkIntoWall(backend, kAllLayers);
        if (blocked < -0.5f) continue;   // бэкенд без персонажей
        bool touched = true;
        const float through = WalkIntoWall(backend, kAllLayers & ~(1u << 2), &touched);
        CHECK_TRUE(blocked < 1.8f);
        CHECK_TRUE(through > 3.0f);   // слой стены исключён — прошёл насквозь
        CHECK_FALSE(touched);         // и не «ударился» о то, сквозь что прошёл
    }
}

TEST(Character_collision_settings_survive_save_and_load) {
    Scene scene("CharSave");
    GameObject hero = scene.CreateObject("Hero");
    auto& cc = scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    cc.Layer = 1u << 4;
    cc.CollidesWith = 0x0Fu;
    cc.CollisionEvents = false;
    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    bool found = false;
    loaded->Registry().view<CharacterControllerComponent>().each(
        [&](entt::entity, const CharacterControllerComponent& c) {
            found = true;
            CHECK_EQ((unsigned)c.Layer, 1u << 4);
            CHECK_EQ((unsigned)c.CollidesWith, 0x0Fu);
            CHECK_FALSE(c.CollisionEvents);
        });
    CHECK_TRUE(found);
}
