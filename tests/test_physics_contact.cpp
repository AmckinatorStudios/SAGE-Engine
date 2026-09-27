// Точность контакта, вращение и коллайдеры по мешу — БЕЗ GL.
//
// Главная проверка этого файла — «тело не тонет в полу». Раньше Jolt искал
// контакты только в конечной точке шага с запасом в 2 см, и ящик, упавший с
// пяти метров (10 м/с, 17 см за шаг), оказывался в полу на 15 см, после чего
// выталкивание поднимало его обратно несколько кадров подряд — «провалился и
// всплыл». Здесь это меряется числом: насколько глубоко нижняя точка тела
// уходила под поверхность за всё падение и отскакивало ли тело обратно.
#include "TestFramework.h"

#include "sage/physics/PhysicsWorld.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

#include <cmath>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace sage::physics;

namespace {

constexpr float kG = 9.81f;
// Сколько проникновения не видно глазом и не выталкивается решателем: зазор
// Jolt (PhysicsSettings::mPenetrationSlop) плюс запас на подшаг.
constexpr float kMaxSink = 0.012f;

std::unique_ptr<PhysicsWorld> MakeWorld() {
    auto w = PhysicsWorld::Create(PhysicsWorld::DefaultBackend());
    w->SetGravity({0.0f, -kG, 0.0f});
    return w;
}

// Пол: статическая коробка с верхом ровно на y = 0.
BodyHandle Floor(PhysicsWorld& w, float friction = 0.5f) {
    BodyDesc f;
    f.Type = BodyType::Static;
    // Без упругости: Jolt берёт большую из двух, и отскок от пола с 0.1 по
    // умолчанию выглядел бы в замере как «всплывание».
    f.Restitution = 0.0f;
    f.HalfExtents = {50.0f, 1.0f, 50.0f};
    f.Position = {0.0f, -1.0f, 0.0f};
    f.Friction = friction;
    return w.CreateBody(f);
}

// Нижняя точка тела по его форме и позе (без поворота — тела в тесте падают ровно).
float Bottom(const BodyDesc& d, const glm::vec3& pos) {
    switch (d.Shape) {
        case ShapeType::Sphere: return pos.y - d.Radius;
        case ShapeType::Capsule: return pos.y - d.HalfHeight - d.Radius;
        default: return pos.y - d.HalfExtents.y;
    }
}

struct DropResult {
    float MaxSink = 0.0f;       // насколько глубоко низ уходил под пол (> 0 — ушёл)
    float PopUp = 0.0f;         // насколько поднимался над полом после касания
    float RestGap = 0.0f;       // зазор в покое (низ − пол)
    float RestSpeed = 0.0f;
    float TouchTime = -1.0f;    // когда впервые коснулся
    bool Touched = false;
};

DropResult Drop(BodyDesc d, float height, const std::function<float(int)>& dtAt, float seconds = 4.0f) {
    auto w = MakeWorld();
    Floor(*w);
    d.Type = BodyType::Dynamic;
    d.Restitution = 0.0f;
    d.Position = {0.0f, height + (d.Shape == ShapeType::Sphere ? d.Radius
                                  : d.Shape == ShapeType::Capsule ? d.HalfHeight + d.Radius
                                                                  : d.HalfExtents.y), 0.0f};
    const BodyHandle b = w->CreateBody(d);
    DropResult r;
    float t = 0.0f;
    for (int i = 0; t < seconds; ++i) {
        const float dt = dtAt(i);
        w->Step(dt);
        t += dt;
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        const float gap = Bottom(d, p);
        r.MaxSink = std::max(r.MaxSink, -gap);
        if (!r.Touched && gap < 0.03f) { r.Touched = true; r.TouchTime = t; }
        if (r.Touched) r.PopUp = std::max(r.PopUp, gap);
        r.RestGap = gap;
        r.RestSpeed = glm::length(w->GetLinearVelocity(b));
    }
    return r;
}

float Fixed60(int) { return 1.0f / 60.0f; }

BodyDesc BoxDesc(float half, float mass) {
    BodyDesc d;
    d.Shape = ShapeType::Box;
    d.HalfExtents = glm::vec3(half);
    d.Mass = mass;
    return d;
}
BodyDesc SphereDesc(float r, float mass) {
    BodyDesc d;
    d.Shape = ShapeType::Sphere;
    d.Radius = r;
    d.Mass = mass;
    return d;
}
BodyDesc CapsuleDesc(float r, float hh, float mass) {
    BodyDesc d;
    d.Shape = ShapeType::Capsule;
    d.Radius = r;
    d.HalfHeight = hh;
    d.Mass = mass;
    return d;
}

bool IsJolt() { return PhysicsWorld::DefaultBackend() == Backend::Jolt; }

} // namespace

// ===========================================================================
//  Падение: не тонет и не всплывает
// ===========================================================================

TEST(PhysicsContact_falling_bodies_do_not_sink_into_floor) {
    if (!IsJolt()) return;
    struct Case { const char* name; BodyDesc desc; };
    const Case cases[] = {
        {"box 1m", BoxDesc(0.5f, 1.0f)},
        {"box 10cm", BoxDesc(0.05f, 0.2f)},
        {"box 4m", BoxDesc(2.0f, 500.0f)},
        {"sphere", SphereDesc(0.5f, 1.0f)},
        {"small sphere", SphereDesc(0.08f, 0.1f)},
        {"capsule", CapsuleDesc(0.3f, 0.5f, 5.0f)},
    };
    const float heights[] = {0.5f, 5.0f, 20.0f};
    for (const Case& c : cases) {
        for (float h : heights) {
            const DropResult r = Drop(c.desc, h, Fixed60);
            if (r.MaxSink > kMaxSink || r.PopUp > 0.02f || std::abs(r.RestGap) > kMaxSink || r.RestSpeed > 0.05f)
                std::printf("    %s с %.1f м: ушёл в пол на %.3f, подскочил на %.3f, в покое %.4f, скорость %.3f\n",
                            c.name, h, r.MaxSink, r.PopUp, r.RestGap, r.RestSpeed);
            CHECK_TRUE(r.Touched);
            CHECK_TRUE(r.MaxSink <= kMaxSink);
            // «Всплывание» после касания: без упругости тело не должно
            // подниматься — раньше его выталкивало обратно на путь за шаг.
            CHECK_TRUE(r.PopUp <= 0.02f);
            CHECK_TRUE(std::abs(r.RestGap) <= kMaxSink);
            CHECK_TRUE(r.RestSpeed < 0.05f);
        }
    }
}

TEST(PhysicsContact_masses_from_grams_to_tons_land_the_same) {
    if (!IsJolt()) return;
    const float masses[] = {0.01f, 1.0f, 100.0f, 10000.0f};
    for (float m : masses) {
        const DropResult r = Drop(BoxDesc(0.5f, m), 8.0f, Fixed60);
        CHECK_TRUE(r.MaxSink <= kMaxSink);
        CHECK_TRUE(r.PopUp <= 0.02f);
        CHECK_NEAR(r.RestGap, 0.0f, kMaxSink);
    }
}

// Частота кадров не должна менять ни глубину, ни время падения. Раньше при
// частоте, не кратной 60, одни кадры получали шаг физики, а другие нет.
TEST(PhysicsContact_frame_rate_does_not_change_the_fall) {
    if (!IsJolt()) return;
    const float h = 5.0f;
    const float expectTouch = std::sqrt(2.0f * h / kG);
    const std::function<float(int)> rates[] = {
        [](int) { return 1.0f / 20.0f; },
        [](int) { return 1.0f / 30.0f; },
        [](int) { return 1.0f / 60.0f; },
        [](int) { return 1.0f / 144.0f; },
        [](int) { return 1.0f / 240.0f; },
        [](int) { return 1.0f / 1000.0f; },
        // Неровные кадры — как в живой игре с подгрузками.
        [](int i) { return (i % 7 == 0) ? 0.05f : (i % 3 == 0 ? 0.004f : 0.013f); },
    };
    for (const auto& rate : rates) {
        const DropResult r = Drop(SphereDesc(0.5f, 1.0f), h, rate);
        CHECK_TRUE(r.MaxSink <= kMaxSink);
        CHECK_TRUE(r.PopUp <= 0.02f);
        CHECK_NEAR(r.RestGap, 0.0f, kMaxSink);
        // Касание — в пределах двух кадров самой редкой частоты от теории
        // (с поправкой на сопротивление воздуха в 0.05 доли в секунду).
        CHECK_NEAR(r.TouchTime, expectTouch, 0.1f);
    }
}

// Каждый кадр продвигает физику ровно на своё время: на 144 Гц не бывает
// кадров «без шага», из-за которых падение шло рывками.
TEST(PhysicsContact_every_frame_advances_the_simulation) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    BodyDesc d = SphereDesc(0.5f, 1.0f);
    d.LinearDamping = 0.0f;
    d.Position = {0, 100, 0};
    const BodyHandle b = w->CreateBody(d);
    float prevY = 100.0f;
    for (int i = 0; i < 144; ++i) {
        w->Step(1.0f / 144.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        if (i > 0) CHECK_TRUE(p.y < prevY);
        prevY = p.y;
    }
    // За секунду свободного падения — g/2 метров (полуявный Эйлер даёт чуть больше).
    CHECK_NEAR(100.0f - prevY, 0.5f * kG, 0.05f);
}

// Быстрый снаряд не проходит сквозь тонкую стенку и не застревает в ней.
TEST(PhysicsContact_fast_projectile_stops_at_thin_wall) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    w->SetGravity({0, 0, 0});
    BodyDesc wall;
    wall.Type = BodyType::Static;
    wall.HalfExtents = {0.05f, 5.0f, 5.0f};
    wall.Position = {0, 0, 0};
    w->CreateBody(wall);
    BodyDesc ball = SphereDesc(0.1f, 0.05f);
    ball.Position = {-10, 0, 0};
    ball.Restitution = 0.0f;
    const BodyHandle b = w->CreateBody(ball);
    w->SetLinearVelocity(b, {200.0f, 0, 0});
    float maxX = -100.0f;
    for (int i = 0; i < 60; ++i) {
        w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        maxX = std::max(maxX, p.x);
    }
    // Правая грань шара не заходит за левую грань стены глубже зазора.
    CHECK_TRUE(maxX + 0.1f <= -0.05f + kMaxSink);
}

// Медленно катящийся шар останавливается и не тонет. Раньше Jolt на скорости
// около миллиметра за шаг брал манифольд из кэша пар тел: точка контакта
// уезжала вместе с поверхностью шара, наклонённая нормаль толкала его вперёд
// и вдавливала в пол — шар катился вечно с одной и той же скоростью.
TEST(PhysicsContact_slowly_rolling_ball_stops_and_does_not_sink) {
    if (!IsJolt()) return;
    for (ShapeType shape : {ShapeType::Sphere, ShapeType::Capsule}) {
        auto w = MakeWorld();
        Floor(*w);
        BodyDesc d;
        d.Shape = shape;
        d.Radius = 0.15f;
        d.HalfHeight = 0.2f;
        d.Mass = 2.0f;
        d.AngularDamping = 1.0f;
        d.Position = {0.0f, 0.15f, 0.0f};
        d.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));  // капсула лёжа
        const BodyHandle b = w->CreateBody(d);
        w->SetLinearVelocity(b, {1.0f, 0.0f, 0.0f});
        float lowest = 1.0f;
        for (int i = 0; i < 1200; ++i) {
            w->Step(1.0f / 60.0f);
            glm::vec3 p; glm::quat q;
            w->GetBodyTransform(b, p, q);
            lowest = std::min(lowest, p.y - 0.15f);
        }
        CHECK_TRUE(lowest > -0.002f);
        CHECK_TRUE(glm::length(w->GetLinearVelocity(b)) < 0.01f);
        CHECK_TRUE(w->IsSleeping(b));
    }
}

// ===========================================================================
//  Стопки и толпа
// ===========================================================================

TEST(PhysicsContact_stack_with_mass_ratio_stands_without_sinking) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    Floor(*w);
    std::vector<BodyHandle> boxes;
    const float masses[] = {1.0f, 50.0f, 1.0f, 50.0f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        BodyDesc d = BoxDesc(0.5f, masses[i]);
        d.Restitution = 0.0f;
        d.Position = {0.0f, 0.5f + i * 1.0f + 0.01f * i, 0.0f};
        boxes.push_back(w->CreateBody(d));
    }
    for (int i = 0; i < 300; ++i) w->Step(1.0f / 60.0f);
    float prevTop = 0.0f;
    for (int i = 0; i < 5; ++i) {
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(boxes[i], p, q);
        // Нижняя грань стоит на верхней грани предыдущего — без продавливания.
        CHECK_NEAR(p.y - 0.5f, prevTop, kMaxSink);
        CHECK_NEAR(p.x, 0.0f, 0.02f);
        CHECK_NEAR(p.z, 0.0f, 0.02f);
        // Стоит ровно, а не завалился.
        CHECK_TRUE(std::abs(glm::dot(q * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0))) > 0.999f);
        prevTop = p.y + 0.5f;
    }
}

TEST(PhysicsContact_crowd_of_mixed_bodies_settles_above_floor) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    Floor(*w);
    struct Item { BodyHandle H; BodyDesc D; };
    std::vector<Item> items;
    for (int i = 0; i < 80; ++i) {
        BodyDesc d = (i % 3 == 0) ? SphereDesc(0.2f + 0.05f * (i % 4), 0.5f + i)
                   : (i % 3 == 1) ? BoxDesc(0.15f + 0.05f * (i % 5), 1.0f + 3.0f * (i % 7))
                                  : CapsuleDesc(0.15f, 0.2f, 2.0f);
        d.Position = {-2.0f + (i % 5), 1.0f + 0.9f * (i / 5), -2.0f + ((i * 3) % 5)};
        // Сопротивления качению у твёрдого тела нет (как и в PhysX): шар на
        // ровном полу катится, пока его не остановит угловое затухание, —
        // здесь его задают так, как задал бы игрок в инспекторе. Без правки
        // кэша контактов (см. следующий тест) даже так шары не
        // останавливались: контакт сам подталкивал их вперёд.
        if (d.Shape != ShapeType::Box) d.AngularDamping = 1.0f;
        d.Rotation = glm::angleAxis(0.3f * i, glm::normalize(glm::vec3(1, 2, 3)));
        items.push_back({w->CreateBody(d), d});
    }
    // Катящийся шар теряет энергию медленнее, чем крутящийся в воздухе:
    // затухание вращения делится между ω и v, которые трение держит вместе
    // (для шара — в I/(I + m·r²) = 2/7 раза). Поэтому 25 секунд.
    for (int i = 0; i < 1500; ++i) w->Step(1.0f / 60.0f);
    int moving = 0;
    for (const Item& it : items) {
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(it.H, p, q);
        // Ни одно тело не ушло под пол глубже зазора: центр выше пола хотя бы
        // на свою самую тонкую «половину» за вычетом зазора.
        const float minHalf = it.D.Shape == ShapeType::Box ? it.D.HalfExtents.x : it.D.Radius;
        CHECK_TRUE(p.y >= minHalf - kMaxSink);
        if (!w->IsSleeping(it.H)) {
            ++moving;
            std::printf("    не уснул: форма %d, y=%.3f v=%.3f w=%.3f\n", (int)it.D.Shape, p.y,
                        glm::length(w->GetLinearVelocity(it.H)), glm::length(w->GetAngularVelocity(it.H)));
        }
    }
    CHECK_EQ(moving, 0);
}

// ===========================================================================
//  Вращение
// ===========================================================================

TEST(PhysicsRotation_angular_velocity_round_trips_on_both_backends) {
    for (Backend be : {Backend::Builtin, PhysicsWorld::DefaultBackend()}) {
        auto w = PhysicsWorld::Create(be);
        w->SetGravity({0, 0, 0});
        BodyDesc d = BoxDesc(0.5f, 2.0f);
        d.AngularDamping = 0.0f;
        const BodyHandle b = w->CreateBody(d);
        w->SetAngularVelocity(b, {0.0f, 3.0f, 0.0f});
        CHECK_NEAR(w->GetAngularVelocity(b).y, 3.0f, 1e-4f);
        // Полсекунды вращения вокруг Y на 3 рад/с — поворот на 1.5 рад.
        for (int i = 0; i < 30; ++i) w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        const glm::vec3 x = q * glm::vec3(1, 0, 0);
        CHECK_NEAR(std::atan2(-x.z, x.x), 1.5f, 0.05f);
        CHECK_NEAR(w->GetAngularVelocity(b).y, 3.0f, 0.02f);
    }
}

// Момент сил раскручивает тело по I·dω/dt = τ, и итог не зависит от кадров.
TEST(PhysicsRotation_torque_spins_up_by_inertia_at_any_frame_rate) {
    for (Backend be : {Backend::Builtin, PhysicsWorld::DefaultBackend()}) {
        for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
            auto w = PhysicsWorld::Create(be);
            w->SetGravity({0, 0, 0});
            BodyDesc d = SphereDesc(0.5f, 4.0f);
            d.AngularDamping = 0.0f;
            const BodyHandle b = w->CreateBody(d);
            // Шар: I = 2/5·m·r² = 0.4 кг·м². Момент 0.4 Н·м секунду -> 1 рад/с.
            int frames = (int)std::lround(1.0f / dt);
            for (int i = 0; i < frames; ++i) {
                w->AddTorque(b, {0.0f, 0.0f, 0.4f});
                w->Step(dt);
            }
            // Хвост накопителя встроенного бэкенда (кадр короче его шага)
            // доотдаётся следующим шагом без новой силы.
            w->Step(1.0f / 60.0f);
            CHECK_NEAR(w->GetAngularVelocity(b).z, 1.0f, 0.03f);
        }
    }
}

TEST(PhysicsRotation_force_accelerates_the_same_at_any_frame_rate) {
    if (!IsJolt()) return;
    for (float dt : {1.0f / 24.0f, 1.0f / 60.0f, 1.0f / 144.0f, 1.0f / 500.0f}) {
        auto w = MakeWorld();
        w->SetGravity({0, 0, 0});
        BodyDesc d = BoxDesc(0.5f, 2.0f);
        d.LinearDamping = 0.0f;
        const BodyHandle b = w->CreateBody(d);
        int frames = (int)std::lround(1.0f / dt);
        for (int i = 0; i < frames; ++i) {
            w->AddForce(b, {4.0f, 0.0f, 0.0f});   // 4 Н на 2 кг — 2 м/с за секунду
            w->Step(dt);
        }
        w->Step(1.0f / 60.0f);
        CHECK_NEAR(w->GetLinearVelocity(b).x, 2.0f, 0.02f);
        CHECK_NEAR(w->GetAngularVelocity(b).y, 0.0f, 1e-4f);
    }
}

TEST(PhysicsRotation_off_center_impulse_spins_center_impulse_does_not) {
    for (Backend be : {Backend::Builtin, PhysicsWorld::DefaultBackend()}) {
        auto w = PhysicsWorld::Create(be);
        w->SetGravity({0, 0, 0});
        BodyDesc d = BoxDesc(0.5f, 1.0f);
        d.AngularDamping = 0.0f;
        const BodyHandle a = w->CreateBody(d);
        d.Position = {5, 0, 0};
        const BodyHandle b = w->CreateBody(d);
        w->AddImpulseAtPoint(a, {0, 0, 1}, {0.5f, 0, 0});      // в край
        w->AddImpulseAtPoint(b, {0, 0, 1}, {5, 0, 0});         // в центр
        // Куб: I = m·(2h)²/6 = 1/6. Плечо 0.5 -> ω = 0.5/(1/6) = 3 рад/с вокруг −Y.
        CHECK_NEAR(w->GetAngularVelocity(a).y, -3.0f, 0.05f);
        CHECK_NEAR(glm::length(w->GetAngularVelocity(b)), 0.0f, 1e-4f);
        CHECK_NEAR(w->GetLinearVelocity(a).z, 1.0f, 1e-3f);
        CHECK_NEAR(w->GetLinearVelocity(b).z, 1.0f, 1e-3f);
    }
}

TEST(PhysicsRotation_locked_axes_hold_under_impacts) {
    for (Backend be : {Backend::Builtin, PhysicsWorld::DefaultBackend()}) {
        auto w = PhysicsWorld::Create(be);
        w->SetGravity({0, -kG, 0});
        Floor(*w);
        // Поворот заморожен целиком: удар в край не опрокидывает.
        BodyDesc d = BoxDesc(0.5f, 1.0f);
        d.Position = {0, 0.5f, 0};
        d.Locks = kLockRotAll;
        const BodyHandle upright = w->CreateBody(d);
        // Позиция заморожена по Z: игра в плоскости XY.
        BodyDesc s = SphereDesc(0.3f, 1.0f);
        s.Position = {3, 2, 0};
        s.Locks = kLockPosZ;
        const BodyHandle planar = w->CreateBody(s);
        w->AddImpulseAtPoint(upright, {3, 0, 3}, {0.5f, 0.5f, 0.5f});
        w->AddImpulse(planar, {2, 0, 5});
        for (int i = 0; i < 120; ++i) w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(upright, p, q);
        CHECK_TRUE(std::abs(glm::dot(q * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0))) > 0.9999f);
        CHECK_TRUE(std::abs(glm::dot(q * glm::vec3(1, 0, 0), glm::vec3(1, 0, 0))) > 0.9999f);
        CHECK_TRUE(p.x > 0.5f);   // сдвинуться удар всё равно смог
        w->GetBodyTransform(planar, p, q);
        CHECK_NEAR(p.z, 0.0f, 1e-3f);
        CHECK_TRUE(p.x > 3.5f);
    }
}

// Удар о пол разворачивает тело: наклонённый ящик ложится на грань.
TEST(PhysicsRotation_tilted_box_lands_and_settles_on_a_face) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    Floor(*w);
    BodyDesc d = BoxDesc(0.5f, 3.0f);
    d.Position = {0, 3, 0};
    d.Rotation = glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(0.3f, 0, 1)));
    const BodyHandle b = w->CreateBody(d);
    float maxSpin = 0.0f;
    for (int i = 0; i < 300; ++i) {
        w->Step(1.0f / 60.0f);
        maxSpin = std::max(maxSpin, glm::length(w->GetAngularVelocity(b)));
    }
    glm::vec3 p; glm::quat q;
    w->GetBodyTransform(b, p, q);
    CHECK_TRUE(maxSpin > 1.0f);             // касание углом закрутило его
    // Лёг на грань: какая-то ось тела смотрит строго вверх, центр на полметра.
    const float up = std::max({std::abs((q * glm::vec3(1, 0, 0)).y), std::abs((q * glm::vec3(0, 1, 0)).y),
                               std::abs((q * glm::vec3(0, 0, 1)).y)});
    CHECK_TRUE(up > 0.999f);
    CHECK_NEAR(p.y, 0.5f, kMaxSink);
    CHECK_TRUE(glm::length(w->GetAngularVelocity(b)) < 0.05f);
}

// Шар на склоне катится, а не скользит: ω = v / r.
TEST(PhysicsRotation_ball_rolls_down_a_slope) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    BodyDesc ramp;
    ramp.Type = BodyType::Static;
    ramp.HalfExtents = {20, 0.5f, 5};
    ramp.Rotation = glm::angleAxis(glm::radians(-15.0f), glm::vec3(0, 0, 1));
    ramp.Friction = 0.8f;
    w->CreateBody(ramp);
    BodyDesc s = SphereDesc(0.4f, 2.0f);
    s.Friction = 0.8f;
    s.AngularDamping = 0.0f;
    s.LinearDamping = 0.0f;
    s.Position = ramp.Rotation * glm::vec3(-8.0f, 0.5f + 0.4f + 0.01f, 0.0f);
    const BodyHandle b = w->CreateBody(s);
    for (int i = 0; i < 90; ++i) w->Step(1.0f / 60.0f);
    const glm::vec3 v = w->GetLinearVelocity(b);
    const glm::vec3 om = w->GetAngularVelocity(b);
    CHECK_TRUE(glm::length(v) > 1.0f);
    // Катится вниз по склону (+X) — вращение вокруг −Z.
    CHECK_NEAR(-om.z * 0.4f, glm::length(v), 0.05f * glm::length(v));
    // Ускорение катящегося шара — 5/7·g·sinθ, а не g·sinθ скользящего.
    CHECK_NEAR(glm::length(v), 5.0f / 7.0f * kG * std::sin(glm::radians(15.0f)) * 1.5f, 0.15f);
}

// ===========================================================================
//  Коллайдеры по мешу
// ===========================================================================

namespace {
// Долина из двух наклонных плоскостей (вогнутая форма — выпуклая оболочка её
// бы «залила»): шар, брошенный на склон, скатывается на дно по сетке.
MeshGeometryPtr Valley() {
    auto g = std::make_shared<MeshGeometry>();
    //    0 ---- 1 (x=-4, y=2)     дно по x=0, y=0
    g->Points = {{-4, 2, -4}, {-4, 2, 4}, {0, 0, -4}, {0, 0, 4}, {4, 2, -4}, {4, 2, 4}};
    // Против часовой при взгляде сверху — лицевая сторона вверх.
    g->Indices = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
    return g;
}

// Плоский пол из сетки 8×8 квадратов — много внутренних рёбер.
MeshGeometryPtr GridFloor(float half, int n) {
    auto g = std::make_shared<MeshGeometry>();
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            g->Points.push_back({-half + 2 * half * x / n, 0.0f, -half + 2 * half * z / n});
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const uint32_t i = z * (n + 1) + x;
            g->Indices.insert(g->Indices.end(), {i, i + (uint32_t)n + 1, i + 1, i + 1, i + (uint32_t)n + 1,
                                                 i + (uint32_t)n + 2});
        }
    return g;
}

// Конус: основание радиуса 0.5 на y=−0.5, вершина на y=+0.5.
MeshGeometryPtr Cone() {
    auto g = std::make_shared<MeshGeometry>();
    for (int i = 0; i < 24; ++i) {
        const float a = 6.2831853f * i / 24;
        g->Points.push_back({0.5f * std::cos(a), -0.5f, 0.5f * std::sin(a)});
    }
    g->Points.push_back({0, 0.5f, 0});
    return g;
}
} // namespace

TEST(PhysicsMesh_ball_rolls_to_the_bottom_of_a_concave_mesh) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    BodyDesc valley;
    valley.Type = BodyType::Static;
    valley.Shape = ShapeType::Mesh;
    valley.Geometry = Valley();
    w->CreateBody(valley);
    BodyDesc s = SphereDesc(0.3f, 1.0f);
    s.Position = {-2.5f, 3.0f, 0.0f};
    s.AngularDamping = 0.5f;
    s.LinearDamping = 0.3f;
    const BodyHandle b = w->CreateBody(s);
    float lowest = 100.0f;
    for (int i = 0; i < 900; ++i) {
        w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        // Расстояние от центра до ближайшего склона (нормали наружу вверх).
        const glm::vec3 nL = glm::normalize(glm::vec3(0.5f, 1.0f, 0.0f));  // левый склон
        const glm::vec3 nR = glm::normalize(glm::vec3(-0.5f, 1.0f, 0.0f)); // правый
        const float d = std::min(glm::dot(p, nL), glm::dot(p, nR));
        lowest = std::min(lowest, d - 0.3f);
    }
    glm::vec3 p; glm::quat q;
    w->GetBodyTransform(b, p, q);
    CHECK_NEAR(p.x, 0.0f, 0.1f);          // на дне, а не где остановила оболочка
    CHECK_TRUE(p.y < 0.5f);
    CHECK_TRUE(lowest > -kMaxSink);       // по сетке — без проваливания
}

TEST(PhysicsMesh_boxes_slide_and_rest_on_triangle_grid_without_sinking) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    BodyDesc floor;
    floor.Type = BodyType::Static;
    floor.Shape = ShapeType::Mesh;
    floor.Geometry = GridFloor(10.0f, 8);
    w->CreateBody(floor);

    // Падает на сетку с высоты.
    BodyDesc d = BoxDesc(0.4f, 5.0f);
    d.Position = {0.3f, 6.0f, 0.2f};
    d.Restitution = 0.0f;
    const BodyHandle faller = w->CreateBody(d);
    // Скользит поперёк стыков треугольников.
    d.Position = {-6.0f, 0.4f, 1.0f};
    d.Friction = 0.05f;
    const BodyHandle slider = w->CreateBody(d);
    w->SetLinearVelocity(slider, {6.0f, 0.0f, 0.0f});

    float sink = 0.0f, hop = 0.0f;
    for (int i = 0; i < 300; ++i) {
        w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(faller, p, q);
        sink = std::max(sink, 0.4f - p.y);
        w->GetBodyTransform(slider, p, q);
        sink = std::max(sink, 0.4f - p.y);
        hop = std::max(hop, p.y - 0.4f);   // подскок на внутреннем ребре
    }
    CHECK_TRUE(sink <= kMaxSink);
    CHECK_TRUE(hop < 0.01f);
    glm::vec3 p; glm::quat q;
    w->GetBodyTransform(slider, p, q);
    CHECK_TRUE(p.x > -3.0f);   // проехал через стыки, не зацепившись
}

TEST(PhysicsMesh_fast_ball_does_not_tunnel_through_mesh_floor) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    BodyDesc floor;
    floor.Type = BodyType::Static;
    floor.Shape = ShapeType::Mesh;
    floor.Geometry = GridFloor(10.0f, 4);
    w->CreateBody(floor);
    BodyDesc s = SphereDesc(0.1f, 0.2f);
    s.Position = {0.0f, 10.0f, 0.0f};
    s.Restitution = 0.0f;
    const BodyHandle b = w->CreateBody(s);
    w->SetLinearVelocity(b, {0.0f, -80.0f, 0.0f});
    float lowest = 100.0f;
    for (int i = 0; i < 120; ++i) {
        w->Step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        lowest = std::min(lowest, p.y - 0.1f);
    }
    CHECK_TRUE(lowest > -kMaxSink);
}

TEST(PhysicsMesh_dynamic_convex_hull_rests_on_its_base) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    Floor(*w);
    BodyDesc d;
    d.Type = BodyType::Dynamic;
    d.Shape = ShapeType::ConvexHull;
    d.Geometry = Cone();
    d.Mass = 2.0f;
    d.Position = {0, 2, 0};
    const BodyHandle hull = w->CreateBody(d);
    // «Mesh» у падающего тела строится оболочкой, а не проваливается.
    d.Shape = ShapeType::Mesh;
    d.Position = {3, 2, 0};
    const BodyHandle meshDyn = w->CreateBody(d);
    CHECK_TRUE(hull != kInvalidBody);
    CHECK_TRUE(meshDyn != kInvalidBody);
    for (int i = 0; i < 240; ++i) w->Step(1.0f / 60.0f);
    for (BodyHandle b : {hull, meshDyn}) {
        glm::vec3 p; glm::quat q;
        w->GetBodyTransform(b, p, q);
        CHECK_NEAR(p.y, 0.5f, kMaxSink);            // основание на полу
        CHECK_TRUE((q * glm::vec3(0, 1, 0)).y > 0.999f);
    }
}

// Сетка без треугольников (все точки на прямой) и пустая геометрия — не
// падение и не пропавшее тело, а коробка по габариту.
TEST(PhysicsMesh_degenerate_geometry_falls_back_to_a_box) {
    if (!IsJolt()) return;
    auto w = MakeWorld();
    Floor(*w);
    auto line = std::make_shared<MeshGeometry>();
    line->Points = {{-1, 0, 0}, {0, 0, 0}, {1, 0, 0}};
    line->Indices = {0, 1, 2};
    BodyDesc d;
    d.Type = BodyType::Static;
    d.Shape = ShapeType::Mesh;
    d.Geometry = line;
    CHECK_TRUE(w->CreateBody(d) != kInvalidBody);
    d.Type = BodyType::Dynamic;
    d.Shape = ShapeType::ConvexHull;
    d.Geometry = nullptr;
    d.Position = {0, 3, 0};
    const BodyHandle b = w->CreateBody(d);
    CHECK_TRUE(b != kInvalidBody);
    for (int i = 0; i < 180; ++i) w->Step(1.0f / 60.0f);
    glm::vec3 p; glm::quat q;
    w->GetBodyTransform(b, p, q);
    CHECK_TRUE(p.y > 0.4f);
}

// ===========================================================================
//  Кинематика
// ===========================================================================

// Платформа, поднимающая ящик, не продавливает его и не дрожит при любой
// частоте кадров: цель кадра раскладывается по подшагам.
TEST(PhysicsKinematic_lift_carries_box_without_sinking_at_any_frame_rate) {
    if (!IsJolt()) return;
    for (float dt : {1.0f / 20.0f, 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
        auto w = MakeWorld();
        BodyDesc lift;
        lift.Type = BodyType::Kinematic;
        lift.HalfExtents = {1.5f, 0.1f, 1.5f};
        lift.Position = {0, 0, 0};
        const BodyHandle platform = w->CreateBody(lift);
        BodyDesc d = BoxDesc(0.3f, 10.0f);
        d.Position = {0, 0.1f + 0.3f, 0};
        const BodyHandle box = w->CreateBody(d);
        for (int i = 0; i < 20; ++i) {
            w->SetBodyTransform(platform, {0, 0, 0}, glm::quat(1, 0, 0, 0));
            w->Step(1.0f / 60.0f);
        }
        float t = 0.0f, worst = 0.0f;
        while (t < 2.0f) {
            t += dt;
            const float y = 2.0f * t;   // 2 м/с вверх
            w->SetBodyTransform(platform, {0, y, 0}, glm::quat(1, 0, 0, 0));
            w->Step(dt);
            glm::vec3 pp, pb; glm::quat q;
            w->GetBodyTransform(platform, pp, q);
            CHECK_NEAR(pp.y, y, 1e-3f);       // платформа ровно там, куда вели
            w->GetBodyTransform(box, pb, q);
            worst = std::max(worst, (pp.y + 0.1f) - (pb.y - 0.3f));
        }
        CHECK_TRUE(worst <= kMaxSink);
        // Остановили платформу — она стоит, а не уезжает по инерции.
        glm::vec3 before, after; glm::quat q;
        w->GetBodyTransform(platform, before, q);
        for (int i = 0; i < 30; ++i) w->Step(1.0f / 60.0f);
        w->GetBodyTransform(platform, after, q);
        CHECK_NEAR(after.y, before.y, 1e-4f);
    }
}

// ===========================================================================
//  Через сцену: компонент -> тело
// ===========================================================================

TEST(PhysicsScene_mesh_collider_from_the_objects_own_mesh) {
    if (!IsJolt()) return;
    Scene scene("MeshCollider");
    // Пол — примитив «плоскость» 20×20, коллайдер «по мешу»: форму задаёт то,
    // что нарисовано, без единого размера в коллайдере.
    GameObject floor = scene.CreateObject("Floor");
    floor.EnsureRenderer().Ref = MeshRef{MeshRef::Type::Plane, ""};
    floor.GetTransform().Scale = {20.0f, 1.0f, 20.0f};
    scene.Registry().emplace<RigidBodyComponent>(floor.Entity(), RigidBodyComponent{BodyType::Static});
    ColliderComponent mesh;
    mesh.Shape = ShapeType::Mesh;
    scene.Registry().emplace<ColliderComponent>(floor.Entity(), mesh);

    // Цилиндр-«бочка» с выпуклой оболочкой по своему мешу.
    GameObject barrel = scene.CreateObject("Barrel");
    barrel.EnsureRenderer().Ref = MeshRef{MeshRef::Type::Cylinder, ""};
    barrel.GetTransform().Position = {0.0f, 3.0f, 0.0f};
    barrel.GetTransform().Scale = {1.0f, 2.0f, 1.0f};   // высота 2
    scene.Registry().emplace<RigidBodyComponent>(barrel.Entity(), RigidBodyComponent{BodyType::Dynamic, 5.0f});
    ColliderComponent hull;
    hull.Shape = ShapeType::ConvexHull;
    scene.Registry().emplace<ColliderComponent>(barrel.Entity(), hull);

    PhysicsScene physics(Backend::Jolt, scene);
    CHECK_EQ(physics.BodyCount(), 2);
    // Геометрия сварена: у плоскости 4 вершины, а не по вершине на угол треугольника.
    const MeshGeometryPtr g = physics.ColliderGeometry(scene, floor.Entity(), {20.0f, 1.0f, 20.0f});
    CHECK_TRUE(g && g->Points.size() == 4 && g->Indices.size() == 6);

    for (int i = 0; i < 240; ++i) physics.Step(scene, 1.0f / 60.0f);
    // Бочка стоит на плоскости (y = 0) нижним торцом: центр на высоте 1.
    CHECK_NEAR(barrel.GetTransform().Position.y, 1.0f, kMaxSink);
}

TEST(PhysicsScene_inspector_edits_apply_during_play) {
    if (!IsJolt()) return;
    Scene scene("LiveEdit");
    GameObject box = scene.CreateObject("Box");
    box.GetTransform().Position = {0.0f, 50.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(box.Entity(), RigidBodyComponent{BodyType::Dynamic, 1.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    for (int i = 0; i < 30; ++i) physics.Step(scene, 1.0f / 60.0f);
    RigidBodyComponent& rb = scene.Registry().get<RigidBodyComponent>(box.Entity());
    const BodyHandle before = rb.RuntimeBody;
    const float vBefore = physics.GetLinearVelocity(before).y;
    CHECK_TRUE(vBefore < -3.0f);

    // Правка «из инспектора»: масса и отключённое тяготение.
    rb.Mass = 20.0f;
    rb.GravityScale = 0.0f;
    physics.Step(scene, 1.0f / 60.0f);
    CHECK_TRUE(rb.RuntimeBody != before);                   // тело пересобрано
    CHECK_EQ(physics.BodyCount(), 1);                       // и не задвоено
    // Летит с той же скоростью, но больше не ускоряется (тяготение выключено).
    const float v1 = physics.GetLinearVelocity(rb.RuntimeBody).y;
    CHECK_NEAR(v1, vBefore, 0.5f);
    for (int i = 0; i < 30; ++i) physics.Step(scene, 1.0f / 60.0f);
    CHECK_NEAR(physics.GetLinearVelocity(rb.RuntimeBody).y, v1, 0.3f);

    // Неизменённое тело не пересобирается каждый кадр.
    const BodyHandle stable = rb.RuntimeBody;
    physics.Step(scene, 1.0f / 60.0f);
    CHECK_EQ(rb.RuntimeBody, stable);

    // Заморозка поворота из компонента доходит до тела.
    rb.Locks = kLockRotAll;
    physics.Step(scene, 1.0f / 60.0f);
    physics.SetAngularVelocity(rb.RuntimeBody, {5, 5, 5});
    CHECK_NEAR(glm::length(physics.GetAngularVelocity(rb.RuntimeBody)), 0.0f, 1e-4f);
}

// Соединение держится за тело. Пересобрали тело на ходу — соединение
// пересобрано с ним, а не осталось висеть на удалённом (в Jolt это чтение
// освобождённой памяти на следующем шаге).
TEST(PhysicsScene_joint_follows_rebuilt_body_and_deleted_partner) {
    Scene scene("Joints");
    GameObject anchor = scene.CreateObject("Anchor");
    anchor.GetTransform().Position = {0.0f, 5.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(anchor.Entity(), RigidBodyComponent{BodyType::Static});
    GameObject bob = scene.CreateObject("Bob");
    bob.GetTransform().Position = {0.0f, 3.0f, 0.0f};
    scene.Registry().emplace<RigidBodyComponent>(bob.Entity(), RigidBodyComponent{BodyType::Dynamic, 1.0f});
    JointComponent jc;
    jc.Type = JointType::Point;
    jc.TargetId = anchor.Id();
    jc.Anchor = {0.0f, 2.0f, 0.0f};
    scene.Registry().emplace<JointComponent>(bob.Entity(), jc);

    PhysicsScene physics(PhysicsWorld::DefaultBackend(), scene);
    if (!physics.SupportsJoints()) return;
    CHECK_EQ(physics.JointCount(), 1);
    for (int i = 0; i < 30; ++i) physics.Step(scene, 1.0f / 60.0f);

    scene.Registry().get<RigidBodyComponent>(bob.Entity()).Mass = 3.0f;   // пересборка
    for (int i = 0; i < 120; ++i) physics.Step(scene, 1.0f / 60.0f);
    CHECK_EQ(physics.JointCount(), 1);
    // Всё ещё висит на подвесе, а не упал.
    CHECK_TRUE(bob.GetTransform().Position.y > 2.5f);

    // Подвес удалили — соединение снято, шаги не падают.
    scene.RemoveObject(anchor.Id());
    for (int i = 0; i < 60; ++i) physics.Step(scene, 1.0f / 60.0f);
    std::printf("    после удаления подвеса: соединений %d, y=%.3f\n", physics.JointCount(),
                bob.GetTransform().Position.y);
    CHECK_EQ(physics.JointCount(), 0);
    CHECK_TRUE(bob.GetTransform().Position.y < 2.5f);
}

TEST(PhysicsScene_rigid_body_and_mesh_collider_settings_survive_save) {
    Scene scene("Save");
    GameObject obj = scene.CreateObject("Rock");
    RigidBodyComponent rb{BodyType::Dynamic, 7.0f};
    rb.LinearDamping = 0.4f;
    rb.AngularDamping = 0.9f;
    rb.GravityScale = 0.25f;
    rb.Continuous = false;
    rb.Locks = kLockRotX | kLockRotZ | kLockPosY;
    scene.Registry().emplace<RigidBodyComponent>(obj.Entity(), rb);
    ColliderComponent col;
    col.Shape = ShapeType::Mesh;
    col.MeshPath = "models/rock_low.obj";
    scene.Registry().emplace<ColliderComponent>(obj.Entity(), col);

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;
    bool found = false;
    for (auto e : loaded->Registry().view<RigidBodyComponent, ColliderComponent>()) {
        const auto& r = loaded->Registry().get<RigidBodyComponent>(e);
        const auto& c = loaded->Registry().get<ColliderComponent>(e);
        CHECK_NEAR(r.Mass, 7.0f, 1e-5f);
        CHECK_NEAR(r.LinearDamping, 0.4f, 1e-5f);
        CHECK_NEAR(r.AngularDamping, 0.9f, 1e-5f);
        CHECK_NEAR(r.GravityScale, 0.25f, 1e-5f);
        CHECK_FALSE(r.Continuous);
        CHECK_EQ((int)r.Locks, (int)(kLockRotX | kLockRotZ | kLockPosY));
        CHECK_TRUE(c.Shape == ShapeType::Mesh);
        CHECK_EQ(c.MeshPath, std::string("models/rock_low.obj"));
        found = true;
    }
    CHECK_TRUE(found);
}
