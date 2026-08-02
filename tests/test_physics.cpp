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

// ===========================================================================
//  Живой состав мира: тела заводятся и убираются ПО ХОДУ симуляции
//
//  Игра порождает физические объекты в рантайме (скрипт спавнит плавающий
//  мусор, обломки, брошенные предметы) и удаляет их. Пока тела строились
//  только в конструкторе PhysicsScene, всё порождённое после старта оставалось
//  без физики — молча: компонент на месте, тела нет, объект висит в воздухе.
// ===========================================================================

TEST(Physics_body_created_for_entity_spawned_after_start) {
    Scene scene("Runtime");
    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.Available()) return; // Null-бэкенд: физики нет, проверять нечего
    CHECK_EQ(physics.BodyCount(), 0);

    // Сущность появляется ПОСЛЕ создания физического мира — как из скрипта.
    GameObject crate = scene.CreateObject("Crate");
    crate.GetTransform().Position = {0.0f, 10.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(crate.Entity(),
        RigidBodyComponent{BodyType::Dynamic});

    physics.Step(scene, 1.0f / 60.0f);
    CHECK_EQ(physics.BodyCount(), 1);
    CHECK_TRUE(scene.Registry().get<RigidBodyComponent>(crate.Entity()).RuntimeBody != kInvalidBody);

    // И тело действительно живёт: под гравитацией ящик падает.
    float startY = crate.GetTransform().Position.y;
    for (int i = 0; i < 60; ++i) physics.Step(scene, 1.0f / 60.0f);
    CHECK_TRUE(crate.GetTransform().Position.y < startY - 1.0f);
}

TEST(Physics_body_removed_when_entity_destroyed) {
    Scene scene("Runtime");
    GameObject a = scene.CreateObject("A");
    scene.Registry().emplace<RigidBodyComponent>(a.Entity(), RigidBodyComponent{BodyType::Dynamic});
    GameObject b = scene.CreateObject("B");
    scene.Registry().emplace<RigidBodyComponent>(b.Entity(), RigidBodyComponent{BodyType::Dynamic});

    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.Available()) return;
    CHECK_EQ(physics.BodyCount(), 2);

    scene.RemoveObject(b.Id());
    physics.Step(scene, 1.0f / 60.0f);
    CHECK_EQ(physics.BodyCount(), 1); // тело удалённой сущности отдано миру

    // Снятый компонент — то же самое: тела быть не должно.
    scene.Registry().remove<RigidBodyComponent>(a.Entity());
    physics.Step(scene, 1.0f / 60.0f);
    CHECK_EQ(physics.BodyCount(), 0);
}

// Play -> Stop -> Play на одной и той же сцене: второй мир обязан построить
// тела заново, а не принять протухшие хэндлы первого за живые.
TEST(Physics_second_simulation_rebuilds_bodies) {
    Scene scene("Replay");
    GameObject box = scene.CreateObject("Box");
    box.GetTransform().Position = {0.0f, 5.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(box.Entity(), RigidBodyComponent{BodyType::Dynamic});

    {
        PhysicsScene first(PhysicsWorld::DefaultBackend(), scene);
        if (!first.Available()) return;
        for (int i = 0; i < 30; ++i) first.Step(scene, 1.0f / 60.0f);
    }
    // Первая симуляция мертва, но хэндл в компоненте остался.
    PhysicsScene second(PhysicsWorld::DefaultBackend(), scene);
    CHECK_EQ(second.BodyCount(), 1);

    float startY = box.GetTransform().Position.y;
    for (int i = 0; i < 60; ++i) second.Step(scene, 1.0f / 60.0f);
    CHECK_TRUE(box.GetTransform().Position.y < startY - 0.5f);
}

// --- Запросы к миру: луч и сфера ----------------------------------------------
//
// Луч проверяется на ОБОИХ бэкендах (Simple всегда доступен, Jolt — если
// собран): headless-прогоны и сборки без Jolt идут на Simple, и работающий
// только у Jolt луч оставил бы половину движка без проверки.

namespace {
// Гоняет один и тот же сценарий на каждом доступном бэкенде: разница между
// ними — предмет отдельного разговора, а контракт обязан быть один.
template <typename Fn>
void ForEachBackend(Fn&& fn) {
    fn(Backend::Simple, "Simple");
    if (PhysicsWorld::HasJolt()) fn(Backend::Jolt, "Jolt");
}

BodyHandle MakeStaticBox(PhysicsWorld& w, glm::vec3 pos, glm::vec3 half,
                         LayerMask layer = kLayerDefault, bool sensor = false) {
    BodyDesc d;
    d.Type = BodyType::Static;
    d.Shape = ShapeType::Box;
    d.HalfExtents = half;
    d.Position = pos;
    d.Layer = layer;
    d.Sensor = sensor;
    return w.CreateBody(d);
}
} // namespace

TEST(Physics_raycast_hits_the_nearest_body) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsQueries()) return;

        // Две стены на пути луча: попасть обязан в БЛИЖНЮЮ. Одна стена такой
        // проверкой не была бы — «нашёл хоть что-то» и «нашёл ближайшее» это
        // разные утверждения, и второе ломается чаще.
        const BodyHandle near_ = MakeStaticBox(*w, {0.0f, 0.0f, -3.0f}, {2.0f, 2.0f, 0.2f});
        MakeStaticBox(*w, {0.0f, 0.0f, -8.0f}, {2.0f, 2.0f, 0.2f});
        w->Step(1.0f / 60.0f);

        RayHit hit;
        const bool ok = w->Raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 50.0f, hit);
        std::printf("       [%s] попадание=%d дистанция=%.2f нормаль=(%.1f %.1f %.1f)\n", name,
                    (int)ok, hit.Distance, hit.Normal.x, hit.Normal.y, hit.Normal.z);
        CHECK_TRUE(ok);
        CHECK_EQ((int)hit.Body, (int)near_);
        CHECK_NEAR(hit.Distance, 2.8f, 0.15);
        // Нормаль смотрит НАВСТРЕЧУ лучу — иначе рикошет и следы от пуль
        // уходили бы внутрь стены.
        CHECK_TRUE(hit.Normal.z > 0.5f);
    });
}

TEST(Physics_raycast_misses_and_respects_distance) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsQueries()) return;
        MakeStaticBox(*w, {0.0f, 0.0f, -10.0f}, {1.0f, 1.0f, 0.5f});
        w->Step(1.0f / 60.0f);

        RayHit hit;
        // Мимо: луч уходит вбок.
        CHECK_FALSE(w->Raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 50.0f, hit));
        // По направлению, но КОРОЧЕ цели: дальность обязана ограничивать, иначе
        // «дострелить» можно было бы до чего угодно.
        CHECK_FALSE(w->Raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 5.0f, hit));
        CHECK_TRUE(w->Raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 20.0f, hit));
        std::printf("       [%s] ограничение дальности работает\n", name);
    });
}

TEST(Physics_raycast_layer_mask_skips_other_layers) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsQueries()) return;

        constexpr LayerMask kPlayer = 1u << 1;
        constexpr LayerMask kWorld = 1u << 2;
        // Ближе — «игрок», дальше — стена. Без масок луч из камеры первым делом
        // упирается в собственную капсулу, и выстрел не выходит за неё.
        MakeStaticBox(*w, {0.0f, 0.0f, -1.0f}, {0.5f, 0.5f, 0.5f}, kPlayer);
        const BodyHandle wall = MakeStaticBox(*w, {0.0f, 0.0f, -6.0f}, {2.0f, 2.0f, 0.3f}, kWorld);
        w->Step(1.0f / 60.0f);

        RayHit hit;
        CHECK_TRUE(w->Raycast({0, 0, 0}, {0, 0, -1}, 50.0f, hit, kWorld));
        CHECK_EQ((int)hit.Body, (int)wall);
        std::printf("       [%s] маска пропустила игрока и нашла стену на %.1f м\n", name,
                    hit.Distance);

        // А без маски — попадает в ближнее.
        CHECK_TRUE(w->Raycast({0, 0, 0}, {0, 0, -1}, 50.0f, hit, kAllLayers));
        CHECK_TRUE((int)hit.Body != (int)wall);
    });
}

TEST(Physics_overlap_sphere_finds_bodies_in_range) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsQueries()) return;
        MakeStaticBox(*w, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        MakeStaticBox(*w, {3.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        MakeStaticBox(*w, {20.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
        w->Step(1.0f / 60.0f);

        std::vector<BodyHandle> found;
        const int n = w->OverlapSphere({0.0f, 0.0f, 0.0f}, 4.0f, found);
        std::printf("       [%s] в сфере радиуса 4: %d тел\n", name, n);
        CHECK_EQ(n, 2);   // третье в двадцати метрах — не должно попасть
    });
}

// --- События столкновений -----------------------------------------------------

TEST(Physics_reports_contact_when_bodies_meet) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsContacts()) return;
        w->SetGravity({0.0f, -9.81f, 0.0f});

        MakeStaticBox(*w, {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f});
        BodyDesc falling;
        falling.Type = BodyType::Dynamic;
        falling.Shape = ShapeType::Box;
        falling.HalfExtents = {0.4f, 0.4f, 0.4f};
        falling.Position = {0.0f, 3.0f, 0.0f};
        const BodyHandle ball = w->CreateBody(falling);

        std::vector<ContactEvent> events;
        int begins = 0;
        for (int i = 0; i < 180; ++i) {
            w->Step(1.0f / 60.0f);
            w->PollContacts(events);
            for (const ContactEvent& e : events)
                if (e.When == ContactEvent::Phase::Begin && (e.A == ball || e.B == ball)) ++begins;
        }
        std::printf("       [%s] событий касания пола: %d\n", name, begins);
        CHECK_TRUE(begins >= 1);
        // Ключевое: «начал касаться» приходит НЕ каждый кадр, пока предмет
        // лежит. Иначе звук удара звучал бы непрерывно всё время лежания.
        CHECK_TRUE(begins < 30);
    });
}

TEST(Physics_sensor_detects_without_pushing) {
    ForEachBackend([](Backend backend, const char* name) {
        auto w = PhysicsWorld::Create(backend);
        if (!w->SupportsContacts()) return;
        w->SetGravity({0.0f, -9.81f, 0.0f});

        // Зона-сенсор на пути падения: обязана заметить и НЕ остановить.
        MakeStaticBox(*w, {0.0f, 1.0f, 0.0f}, {2.0f, 0.3f, 2.0f}, kLayerDefault, /*sensor=*/true);
        BodyDesc falling;
        falling.Type = BodyType::Dynamic;
        falling.Shape = ShapeType::Box;
        falling.HalfExtents = {0.3f, 0.3f, 0.3f};
        falling.Position = {0.0f, 4.0f, 0.0f};
        const BodyHandle ball = w->CreateBody(falling);

        std::vector<ContactEvent> events;
        bool sawSensor = false;
        for (int i = 0; i < 120; ++i) {
            w->Step(1.0f / 60.0f);
            w->PollContacts(events);
            for (const ContactEvent& e : events)
                if (e.Sensor && (e.A == ball || e.B == ball)) sawSensor = true;
        }
        glm::vec3 pos;
        glm::quat rot;
        w->GetBodyTransform(ball, pos, rot);
        std::printf("       [%s] сенсор заметил=%d, тело упало до y=%.2f\n", name, (int)sawSensor,
                    pos.y);
        CHECK_TRUE(sawSensor);
        // Проверка «не остановил»: тело обязано провалиться НИЖЕ зоны.
        CHECK_TRUE(pos.y < 0.5f);
    });
}

// --- Контроллер персонажа -----------------------------------------------------

TEST(Character_walks_stands_and_is_stopped_by_walls) {
    if (!PhysicsWorld::HasJolt()) return;   // контроллер есть только у Jolt
    auto w = PhysicsWorld::Create(Backend::Jolt);
    if (!w->SupportsCharacters()) return;
    w->SetGravity({0.0f, -9.81f, 0.0f});

    MakeStaticBox(*w, {0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});   // пол
    MakeStaticBox(*w, {4.0f, 1.5f, 0.0f}, {0.5f, 2.0f, 6.0f});      // стена справа

    CharacterDesc cd;
    cd.Position = {0.0f, 0.2f, 0.0f};
    const CharacterHandle ch = w->CreateCharacter(cd);
    CHECK_TRUE(ch != kInvalidCharacter);

    // 1. Стоит на полу и не проваливается.
    for (int i = 0; i < 60; ++i) {
        w->MoveCharacter(ch, {0.0f, -1.0f, 0.0f}, 1.0f / 60.0f);
        w->Step(1.0f / 60.0f);
    }
    CharacterState st = w->GetCharacterState(ch);
    std::printf("       стоит: y=%.2f опора=%d\n", st.Position.y, (int)st.Grounded);
    CHECK_TRUE(st.Grounded);
    CHECK_TRUE(st.Position.y > -0.1f && st.Position.y < 0.3f);

    // 2. Идёт вперёд — и доходит.
    for (int i = 0; i < 60; ++i) {
        w->MoveCharacter(ch, {2.0f, -1.0f, 0.0f}, 1.0f / 60.0f);
        w->Step(1.0f / 60.0f);
    }
    st = w->GetCharacterState(ch);
    std::printf("       прошёл до x=%.2f\n", st.Position.x);
    CHECK_TRUE(st.Position.x > 1.0f);

    // 3. Упирается в стену, а не проходит сквозь. Ради этого контроллер и
    //    существует: динамическое тело здесь бы либо застряло, либо опрокинулось.
    for (int i = 0; i < 240; ++i) {
        w->MoveCharacter(ch, {4.0f, -1.0f, 0.0f}, 1.0f / 60.0f);
        w->Step(1.0f / 60.0f);
    }
    st = w->GetCharacterState(ch);
    std::printf("       уперся в стену на x=%.2f (стена на 3.5)\n", st.Position.x);
    CHECK_TRUE(st.Position.x < 3.6f);
    CHECK_TRUE(st.Grounded);
}

TEST(Character_climbs_a_step_it_would_trip_over) {
    if (!PhysicsWorld::HasJolt()) return;
    auto w = PhysicsWorld::Create(Backend::Jolt);
    if (!w->SupportsCharacters()) return;
    w->SetGravity({0.0f, -9.81f, 0.0f});

    MakeStaticBox(*w, {0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
    // Ступенька 25 см — ниже StepHeight (35 см): на неё нужно ВЗОЙТИ.
    MakeStaticBox(*w, {3.0f, 0.125f, 0.0f}, {2.0f, 0.125f, 4.0f});

    CharacterDesc cd;
    cd.Position = {0.0f, 0.2f, 0.0f};
    cd.StepHeight = 0.35f;
    const CharacterHandle ch = w->CreateCharacter(cd);

    // Идём ровно до СЕРЕДИНЫ ступеньки (она занимает x от 1 до 5) и там
    // останавливаемся: пройти её насквозь и слезть обратно на пол — тоже успех
    // подъёма, но проверить по конечной высоте его уже нельзя.
    for (int i = 0; i < 110; ++i) {
        w->MoveCharacter(ch, {2.0f, -2.0f, 0.0f}, 1.0f / 60.0f);
        w->Step(1.0f / 60.0f);
    }
    for (int i = 0; i < 30; ++i) {   // дать опоре устояться на месте
        w->MoveCharacter(ch, {0.0f, -2.0f, 0.0f}, 1.0f / 60.0f);
        w->Step(1.0f / 60.0f);
    }
    const CharacterState st = w->GetCharacterState(ch);
    std::printf("       после ступеньки: x=%.2f y=%.2f опора=%d\n", st.Position.x, st.Position.y,
                (int)st.Grounded);
    // Взошёл: и продвинулся по x, и поднялся по y.
    CHECK_TRUE(st.Position.x > 1.5f && st.Position.x < 5.0f);
    CHECK_TRUE(st.Position.y > 0.15f);   // стоит НА ступеньке, а не перед ней
}

// --- Сквозной путь через ECS: сущности, а не дескрипторы ----------------------
//
// Мир физики отвечает дескрипторами тел, а игра думает сущностями. Перевод
// делается в PhysicsScene, и именно он ломается незаметнее всего: попадание в
// «никуда» выглядит как промах, а не как сбой.

TEST(PhysicsScene_raycast_returns_the_entity_that_was_hit) {
    Scene scene("RayScene");
    GameObject wall = scene.CreateObject("Wall");
    wall.GetTransform().Position = {0.0f, 0.0f, -5.0f};
    wall.GetTransform().Scale = {4.0f, 4.0f, 0.5f};
    scene.Registry().emplace<RigidBodyComponent>(wall.Entity(),
                                                 RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(wall.Entity());

    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.SupportsQueries()) return;
    physics.Step(scene, 1.0f / 60.0f);

    const PhysicsScene::EntityHit hit = physics.Raycast({0, 0, 0}, {0, 0, -1}, 50.0f);
    std::printf("       попал в сущность=%d на %.2f м\n", (int)hit.Hit, hit.Distance);
    CHECK_TRUE(hit.Hit);
    CHECK_TRUE(hit.Entity == wall.Entity());   // именно ТА сущность, а не «какая-то»

    // Удалённая сущность не должна отдаваться лучом: её тело снимается тем же
    // Step, и попадание в мёртвый объект — худший вид ответа.
    scene.RemoveObject(wall.Id());
    physics.Step(scene, 1.0f / 60.0f);
    CHECK_FALSE(physics.Raycast({0, 0, 0}, {0, 0, -1}, 50.0f).Hit);
}

TEST(PhysicsScene_reports_contacts_between_entities) {
    Scene scene("ContactScene");
    GameObject floor = scene.CreateObject("Floor");
    floor.GetTransform().Position = {0.0f, -0.5f, 0.0f};
    floor.GetTransform().Scale = {20.0f, 1.0f, 20.0f};
    scene.Registry().emplace<RigidBodyComponent>(floor.Entity(),
                                                 RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(floor.Entity());

    GameObject box = scene.CreateObject("Box");
    box.GetTransform().Position = {0.0f, 3.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(box.Entity());
    scene.Registry().emplace<ColliderComponent>(box.Entity());

    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.SupportsContacts()) return;

    bool sawPair = false;
    for (int i = 0; i < 240; ++i) {
        physics.Step(scene, 1.0f / 60.0f);
        for (const PhysicsScene::EntityContact& c : physics.Contacts()) {
            const bool pair = (c.A == box.Entity() && c.B == floor.Entity()) ||
                              (c.B == box.Entity() && c.A == floor.Entity());
            if (pair && c.Begin) sawPair = true;
        }
    }
    std::printf("       столкновение ящика с полом замечено=%d\n", (int)sawPair);
    CHECK_TRUE(sawPair);
}

TEST(PhysicsScene_drives_a_character_controller_from_components) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("CharScene");
    GameObject floor = scene.CreateObject("Floor");
    floor.GetTransform().Position = {0.0f, -0.5f, 0.0f};
    floor.GetTransform().Scale = {40.0f, 1.0f, 40.0f};
    scene.Registry().emplace<RigidBodyComponent>(floor.Entity(),
                                                 RigidBodyComponent{BodyType::Static});
    scene.Registry().emplace<ColliderComponent>(floor.Entity());

    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = {0.0f, 0.2f, 0.0f};
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());

    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.SupportsCharacters()) return;
    physics.Step(scene, 1.0f / 60.0f);

    auto& cc = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    CHECK_TRUE(cc.Runtime != kInvalidCharacter);   // контроллер завёлся сам

    for (int i = 0; i < 120; ++i) {
        physics.MoveCharacter(cc.Runtime, {1.5f, -2.0f, 0.0f}, 1.0f / 60.0f);
        physics.Step(scene, 1.0f / 60.0f);
    }
    // Положение обязано вернуться В TRANSFORM сущности: игра работает с ним, а
    // не с дескриптором внутри физики.
    const glm::vec3 pos = hero.GetTransform().Position;
    std::printf("       персонаж дошёл до x=%.2f, опора=%d\n", pos.x, (int)cc.Grounded);
    CHECK_TRUE(pos.x > 1.0f);
    CHECK_TRUE(cc.Grounded);
}

// --- Физика на ДОЧЕРНЕЙ сущности ----------------------------------------------
//
// Тело создавалось из ЛОКАЛЬНОГО Transform: d.Position = tr.Position. Для
// корневой сущности локальное и мировое совпадают, и потому всё выглядело
// исправным — ровно до первой сущности с родителем. Тогда тело оказывалось не
// там, где объект: коллизия «не следует за объектом», хотя на экране объект
// стоит правильно, потому что рендер-то мировую матрицу учитывает.
TEST(Physics_body_of_a_child_entity_starts_at_its_world_position) {
    Scene scene("child");
    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.Available() || !physics.SupportsQueries()) return;

    GameObject parent = scene.CreateObject("Parent");
    parent.GetTransform().Position = {10.0f, 0.0f, -5.0f};

    GameObject child = scene.CreateObject("Child");
    child.GetTransform().Position = {0.0f, 3.0f, 0.0f}; // локально — над родителем
    scene.SetParent(child.Entity(), parent.Entity());

    auto& rb = scene.Registry().emplace<RigidBodyComponent>(child.Entity());
    rb.Type = sage::physics::BodyType::Static;   // статика не падает — позиция не «уедет»

    physics.Step(scene, 1.0f / 60.0f);

    // Мировая позиция ребёнка — (10, 3, -5). Именно там обязано стоять тело.
    PhysicsScene::EntityHit hit =
        physics.Raycast({10.0f, 10.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK_TRUE(hit.Hit);
    if (hit.Hit) CHECK_TRUE(hit.Entity == child.Entity());
}

// Поворот родителя обязан поворачивать и тело ребёнка. Проверяем длинным
// коллайдером: осевой луч попадает в него только при верном повороте.
TEST(Physics_child_body_follows_parent_rotation) {
    Scene scene("childrot");
    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.Available() || !physics.SupportsQueries()) return;

    GameObject parent = scene.CreateObject("Parent");
    parent.GetTransform().Rotation = {0.0f, 90.0f, 0.0f}; // разворот вокруг Y

    GameObject child = scene.CreateObject("Child");
    child.GetTransform().Position = {4.0f, 0.0f, 0.0f};   // локально — по +X
    scene.SetParent(child.Entity(), parent.Entity());

    auto& rb = scene.Registry().emplace<RigidBodyComponent>(child.Entity());
    rb.Type = sage::physics::BodyType::Static;
    auto& col = scene.Registry().emplace<ColliderComponent>(child.Entity());
    col.HalfExtents = {0.5f, 0.5f, 0.5f};

    physics.Step(scene, 1.0f / 60.0f);

    // Поворот на 90° вокруг Y переносит локальный +X в мировой -Z.
    PhysicsScene::EntityHit hit =
        physics.Raycast({0.0f, 5.0f, -4.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK_TRUE(hit.Hit);
    if (hit.Hit) CHECK_TRUE(hit.Entity == child.Entity());
}
