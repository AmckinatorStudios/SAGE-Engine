// Собственный физический движок SAGE (Backend::Builtin) — проверяется ИМЕННО
// он, а не «бэкенд по умолчанию»: смысл этих тестов в том, что сборка без Jolt
// получает полноценную физику, а не её видимость.
//
// Каждый тест здесь соответствует тому, чего встроенная физика раньше не умела
// вовсе: вращаться, лежать под углом, стоять стопкой, тереться, отскакивать,
// засыпать и держать соединения.
#include "TestFramework.h"

#include "sage/physics/PhysicsWorld.h"

#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace sage::physics;

namespace {

std::unique_ptr<PhysicsWorld> MakeWorld() {
    auto w = PhysicsWorld::Create(Backend::Builtin);
    w->SetGravity({0.0f, -9.81f, 0.0f});
    return w;
}

BodyHandle Floor(PhysicsWorld& w, float friction = 0.6f) {
    BodyDesc d;
    d.Type = BodyType::Static;
    d.Shape = ShapeType::Box;
    d.HalfExtents = {50.0f, 0.5f, 50.0f};
    d.Position = {0.0f, -0.5f, 0.0f};
    d.Friction = friction;
    d.Restitution = 0.0f;
    return w.CreateBody(d);
}

BodyHandle Box(PhysicsWorld& w, glm::vec3 pos, glm::vec3 half, float mass = 1.0f,
               glm::quat rot = glm::quat(1, 0, 0, 0), float friction = 0.6f,
               float restitution = 0.0f) {
    BodyDesc d;
    d.Type = BodyType::Dynamic;
    d.Shape = ShapeType::Box;
    d.HalfExtents = half;
    d.Position = pos;
    d.Rotation = rot;
    d.Mass = mass;
    d.Friction = friction;
    d.Restitution = restitution;
    return w.CreateBody(d);
}

void Run(PhysicsWorld& w, float seconds) {
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) w.Step(dt);
}

glm::vec3 PosOf(PhysicsWorld& w, BodyHandle h) {
    glm::vec3 p; glm::quat r;
    w.GetBodyTransform(h, p, r);
    return p;
}
glm::quat RotOf(PhysicsWorld& w, BodyHandle h) {
    glm::vec3 p; glm::quat r;
    w.GetBodyTransform(h, p, r);
    return r;
}

// Насколько тело «не лежит гранью»: угол между вертикалью и БЛИЖАЙШЕЙ к ней
// собственной осью тела. Именно ближайшей, а не осью Y: куб, легший на бок,
// повёрнут на 90° и при этом лежит ровно — мерить у него отклонение оси Y
// значило бы называть правильную посадку падением набок.
float TiltDegrees(const glm::quat& q) {
    float best = 0.0f;
    for (int i = 0; i < 3; ++i) {
        glm::vec3 e(0.0f); e[i] = 1.0f;
        best = std::max(best, std::abs((q * e).y));
    }
    return glm::degrees(std::acos(glm::clamp(best, -1.0f, 1.0f)));
}

} // namespace

// --- вращение -----------------------------------------------------------------

TEST(Builtin_box_dropped_on_a_corner_tumbles_and_lands_flat) {
    auto w = MakeWorld();
    Floor(*w);
    // Куб, повёрнутый на 40° по двум осям: он обязан УПАСТЬ НА ГРАНЬ, а не
    // остаться висеть углом вниз. Физика без вращения оставила бы его в
    // исходном повороте навсегда — и это самое заметное, чего не было у
    // прежнего встроенного бэкенда.
    const glm::quat tilted = glm::angleAxis(glm::radians(40.0f), glm::vec3(1, 0, 0)) *
                             glm::angleAxis(glm::radians(35.0f), glm::vec3(0, 0, 1));
    const BodyHandle b = Box(*w, {0.0f, 3.0f, 0.0f}, glm::vec3(0.5f), 1.0f, tilted);

    std::printf("       наклон до падения: %.1f°\n", TiltDegrees(tilted));
    Run(*w, 5.0f);
    const glm::quat r = RotOf(*w, b);
    const glm::vec3 p = PosOf(*w, b);
    std::printf("       после падения: наклон %.1f°, y=%.3f\n", TiltDegrees(r), p.y);

    CHECK_TRUE(TiltDegrees(r) < 8.0f);          // легло гранью на пол
    CHECK_TRUE(p.y > 0.45f && p.y < 0.56f);     // ровно на полу, не в нём
}

TEST(Builtin_rotated_box_rests_on_its_slanted_face_not_on_a_bounding_box) {
    auto w = MakeWorld();
    Floor(*w);
    // Наклонная СТАТИКА (горка 20°) и кубик на ней. Осевой габарит поднял бы
    // кубик на высоту всей горки; честная повёрнутая коробка держит его на
    // самой поверхности.
    BodyDesc ramp;
    ramp.Type = BodyType::Static;
    ramp.Shape = ShapeType::Box;
    ramp.HalfExtents = {4.0f, 0.25f, 4.0f};
    ramp.Position = {0.0f, 1.0f, 0.0f};
    ramp.Rotation = glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1));
    ramp.Friction = 1.0f;
    w->CreateBody(ramp);

    const BodyHandle b = Box(*w, {0.0f, 2.2f, 0.0f}, glm::vec3(0.25f), 1.0f,
                             glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1)), 1.0f);
    Run(*w, 4.0f);

    const glm::vec3 p = PosOf(*w, b);
    std::printf("       кубик на горке: y=%.3f (поверхность около 1.25)\n", p.y);
    // Поверхность горки под кубиком — примерно y=1.25; кубик полукубом выше.
    CHECK_TRUE(p.y > 1.2f && p.y < 1.7f);
    // И повторил наклон горки, а не остался лежать плашмя.
    CHECK_TRUE(std::abs(TiltDegrees(RotOf(*w, b)) - 20.0f) < 8.0f);
}

// --- манифольды и стопка --------------------------------------------------------

TEST(Builtin_a_stack_of_boxes_stands_still) {
    auto w = MakeWorld();
    Floor(*w);
    // Пять ящиков друг на друге. Это главная проверка манифольдов и тёплого
    // старта разом: с одной точкой контакта стопка развалится за секунду, без
    // переноса импульса — просядет и уползёт.
    BodyHandle boxes[5];
    for (int i = 0; i < 5; ++i)
        boxes[i] = Box(*w, {0.0f, 0.5f + (float)i * 1.02f, 0.0f}, glm::vec3(0.5f));

    Run(*w, 6.0f);

    for (int i = 0; i < 5; ++i) {
        const glm::vec3 p = PosOf(*w, boxes[i]);
        std::printf("       ящик %d: y=%.3f смещение вбок=%.3f наклон=%.1f°\n", i, p.y,
                    std::sqrt(p.x * p.x + p.z * p.z), TiltDegrees(RotOf(*w, boxes[i])));
        // Стоит примерно там, где его положили: не провалился в соседа и не
        // уехал вбок.
        CHECK_TRUE(p.y > 0.35f + (float)i * 0.9f);
        CHECK_TRUE(std::sqrt(p.x * p.x + p.z * p.z) < 0.25f);
        CHECK_TRUE(TiltDegrees(RotOf(*w, boxes[i])) < 12.0f);
    }
}

// --- трение ----------------------------------------------------------------------

TEST(Builtin_friction_decides_whether_a_box_slides_down_a_ramp) {
    // Одна и та же горка в 25°, два ящика: липкий остаётся, скользкий съезжает.
    // Без конуса Кулона оба вели бы себя одинаково.
    auto run = [](float friction) {
        auto w = MakeWorld();
        BodyDesc ramp;
        ramp.Type = BodyType::Static;
        ramp.Shape = ShapeType::Box;
        ramp.HalfExtents = {6.0f, 0.25f, 4.0f};
        ramp.Position = {0.0f, 1.0f, 0.0f};
        ramp.Rotation = glm::angleAxis(glm::radians(25.0f), glm::vec3(0, 0, 1));
        ramp.Friction = friction;
        w->CreateBody(ramp);

        const glm::quat tilt = glm::angleAxis(glm::radians(25.0f), glm::vec3(0, 0, 1));
        const BodyHandle b = Box(*w, {0.0f, 1.6f, 0.0f}, glm::vec3(0.25f), 1.0f, tilt, friction);
        Run(*w, 3.0f);
        return PosOf(*w, b).x;
    };

    const float sticky = run(1.2f);
    const float slippery = run(0.02f);
    std::printf("       липкий сполз на %.2f, скользкий — на %.2f\n", sticky, slippery);
    CHECK_TRUE(std::abs(sticky) < 0.35f);       // почти не тронулся
    CHECK_TRUE(std::abs(slippery) > 1.5f);      // уехал вниз по склону
}

// --- упругость ---------------------------------------------------------------------

TEST(Builtin_restitution_makes_a_ball_bounce_and_a_dead_ball_stay) {
    auto bounceHeight = [](float restitution) {
        auto w = MakeWorld();
        Floor(*w, 0.4f);
        BodyDesc d;
        d.Type = BodyType::Dynamic;
        d.Shape = ShapeType::Sphere;
        d.Radius = 0.3f;
        d.Position = {0.0f, 4.0f, 0.0f};
        d.Mass = 1.0f;
        d.Restitution = restitution;
        d.Friction = 0.3f;
        const BodyHandle b = w->CreateBody(d);

        // Падаем, ударяемся — и ищем, как высоко подскочили после удара.
        Run(*w, 1.0f);
        float peak = 0.0f;
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; ++i) {
            w->Step(dt);
            peak = std::max(peak, PosOf(*w, b).y);
        }
        return peak;
    };

    const float bouncy = bounceHeight(0.8f);
    const float dead = bounceHeight(0.0f);
    std::printf("       упругий отскочил до %.2f, неупругий — до %.2f\n", bouncy, dead);
    CHECK_TRUE(bouncy > 1.0f);
    CHECK_TRUE(dead < 0.5f);
}

// --- сон -----------------------------------------------------------------------------

TEST(Builtin_a_settled_body_falls_asleep_and_wakes_on_impact) {
    auto w = MakeWorld();
    Floor(*w);
    const BodyHandle sleeper = Box(*w, {0.0f, 0.5f, 0.0f}, glm::vec3(0.5f));
    Run(*w, 3.0f);
    const glm::vec3 restPos = PosOf(*w, sleeper);

    // Спящее тело не должно ползти: без сна оно годами дрейфует на доли
    // миллиметра за кадр, и за долгую сессию сдвигается заметно.
    Run(*w, 5.0f);
    const glm::vec3 stillPos = PosOf(*w, sleeper);
    const float drift = glm::length(stillPos - restPos);
    std::printf("       дрейф во сне за 5 с: %.6f\n", drift);
    CHECK_TRUE(drift < 0.002f);

    // Роняем сверху второй ящик: спящий обязан проснуться, а не пропустить
    // гостя сквозь себя.
    Box(*w, {0.0f, 4.0f, 0.0f}, glm::vec3(0.5f), 3.0f);
    Run(*w, 3.0f);
    const glm::vec3 after = PosOf(*w, sleeper);
    std::printf("       после удара сверху: y=%.3f\n", after.y);
    CHECK_TRUE(after.y > 0.35f && after.y < 0.65f);   // остался на месте, не продавлен
}

// --- спекулятивные контакты ------------------------------------------------------

TEST(Builtin_a_fast_body_does_not_tunnel_through_a_thin_wall) {
    auto w = MakeWorld();
    w->SetGravity({0.0f, 0.0f, 0.0f});
    BodyDesc wall;
    wall.Type = BodyType::Static;
    wall.Shape = ShapeType::Box;
    wall.HalfExtents = {0.05f, 5.0f, 5.0f};   // стенка в десять сантиметров
    wall.Position = {10.0f, 0.0f, 0.0f};
    w->CreateBody(wall);

    const BodyHandle bullet = Box(*w, {0.0f, 0.0f, 0.0f}, glm::vec3(0.1f), 1.0f);
    w->SetLinearVelocity(bullet, {160.0f, 0.0f, 0.0f});   // почти скорость звука
    Run(*w, 1.0f);

    const float x = PosOf(*w, bullet).x;
    std::printf("       снаряд остановился на x=%.2f (стенка на 9.95)\n", x);
    CHECK_TRUE(x < 10.0f);   // не пролетел насквозь
}

// --- соединения --------------------------------------------------------------------

TEST(Builtin_point_joint_holds_a_body_hanging_from_the_world) {
    auto w = MakeWorld();
    const BodyHandle b = Box(*w, {0.0f, 5.0f, 0.0f}, glm::vec3(0.2f));
    JointDesc j;
    j.Type = JointType::Point;
    j.BodyA = b;
    j.BodyB = kInvalidBody;
    j.Anchor = {0.0f, 5.2f, 0.0f};
    CHECK_TRUE(w->CreateJoint(j) != kInvalidJoint);

    Run(*w, 4.0f);
    const glm::vec3 p = PosOf(*w, b);
    std::printf("       висит в %.2f %.2f %.2f\n", p.x, p.y, p.z);
    // Точка крепления держит: тело качается под ней, но не падает.
    CHECK_TRUE(glm::length(p - glm::vec3(0.0f, 5.2f, 0.0f)) < 0.6f);
}

TEST(Builtin_distance_joint_behaves_like_a_rope) {
    auto w = MakeWorld();
    const BodyHandle b = Box(*w, {0.0f, 8.0f, 0.0f}, glm::vec3(0.2f));
    JointDesc j;
    j.Type = JointType::Distance;
    j.BodyA = b;
    j.BodyB = kInvalidBody;
    j.Anchor = {0.0f, 8.0f, 0.0f};
    j.MinDistance = 0.0f;
    j.MaxDistance = 2.0f;
    w->CreateJoint(j);
    // Якорь троса — там, где он был при создании; тело падает, пока трос не
    // натянется, и повисает на нём.
    Run(*w, 5.0f);

    const glm::vec3 p = PosOf(*w, b);
    const float dist = glm::length(p - glm::vec3(0.0f, 8.0f, 0.0f));
    std::printf("       трос натянулся на длину %.3f (предел 2.0)\n", dist);
    CHECK_TRUE(dist < 2.2f);
    CHECK_TRUE(dist > 1.7f);   // именно повис, а не остался наверху
}

TEST(Builtin_hinge_lets_a_door_swing_around_its_axis_only) {
    auto w = MakeWorld();
    // Дверь: плоская коробка, подвешенная на петле по вертикальной оси у
    // одного края. Должна поворачиваться вокруг петли и не заваливаться.
    const BodyHandle door = Box(*w, {1.0f, 2.0f, 0.0f}, {1.0f, 1.0f, 0.08f}, 8.0f);
    JointDesc j;
    j.Type = JointType::Hinge;
    j.BodyA = door;
    j.BodyB = kInvalidBody;
    j.Anchor = {0.0f, 2.0f, 0.0f};
    j.Axis = {0.0f, 1.0f, 0.0f};
    w->CreateJoint(j);

    // Толкаем дверь в сторону.
    w->AddImpulse(door, {0.0f, 0.0f, 40.0f});
    Run(*w, 2.0f);

    const glm::vec3 p = PosOf(*w, door);
    const glm::quat r = RotOf(*w, door);
    const glm::vec3 up = r * glm::vec3(0.0f, 1.0f, 0.0f);
    std::printf("       дверь: центр (%.2f %.2f %.2f), верх (%.2f %.2f %.2f)\n", p.x, p.y, p.z,
                up.x, up.y, up.z);
    // Провернулась (z заметно изменился)…
    CHECK_TRUE(std::abs(p.z) > 0.2f);
    // …осталась на своей высоте и вертикальной: петля держит остальные оси.
    CHECK_TRUE(std::abs(p.y - 2.0f) < 0.35f);
    CHECK_TRUE(up.y > 0.9f);
    // И не улетела от петли.
    CHECK_TRUE(std::abs(glm::length(glm::vec3(p.x, 0.0f, p.z)) - 1.0f) < 0.35f);
}

TEST(Builtin_fixed_joint_welds_two_bodies_together) {
    auto w = MakeWorld();
    Floor(*w);
    const BodyHandle a = Box(*w, {0.0f, 5.0f, 0.0f}, glm::vec3(0.3f), 1.0f);
    const BodyHandle b = Box(*w, {0.8f, 5.0f, 0.0f}, glm::vec3(0.3f), 1.0f);
    JointDesc j;
    j.Type = JointType::Fixed;
    j.BodyA = a;
    j.BodyB = b;
    j.Anchor = {0.4f, 5.0f, 0.0f};
    w->CreateJoint(j);

    Run(*w, 4.0f);
    const glm::vec3 pa = PosOf(*w, a), pb = PosOf(*w, b);
    const float gap = glm::length(pb - pa);
    std::printf("       сварка: расстояние %.3f (было 0.800)\n", gap);
    CHECK_TRUE(std::abs(gap - 0.8f) < 0.15f);
}

TEST(Builtin_slider_moves_along_its_axis_and_is_held_across_it) {
    auto w = MakeWorld();
    // Ползун вдоль X: тело обязано ездить по оси и не падать поперёк неё.
    const BodyHandle b = Box(*w, {0.0f, 3.0f, 0.0f}, glm::vec3(0.25f), 1.0f);
    JointDesc j;
    j.Type = JointType::Slider;
    j.BodyA = b;
    j.BodyB = kInvalidBody;
    j.Anchor = {0.0f, 3.0f, 0.0f};
    j.Axis = {1.0f, 0.0f, 0.0f};
    w->CreateJoint(j);

    w->AddImpulse(b, {6.0f, 0.0f, 0.0f});
    Run(*w, 2.0f);

    const glm::vec3 p = PosOf(*w, b);
    std::printf("       ползун: (%.2f %.2f %.2f)\n", p.x, p.y, p.z);
    CHECK_TRUE(p.x > 1.0f);                  // поехал вдоль оси
    CHECK_TRUE(std::abs(p.y - 3.0f) < 0.2f); // и не упал, хотя тяготение есть
    CHECK_TRUE(std::abs(p.z) < 0.2f);
}

// --- инерция формы ---------------------------------------------------------------

TEST(Builtin_inertia_comes_from_the_shape_not_from_a_point_mass) {
    // Физический маятник: длинный брусок, подвешенный на петле за КОНЕЦ и
    // отпущенный из горизонтали.
    //
    // Это прямая проверка тензора инерции, и обмануть её нельзя. Угловое
    // ускорение маятника — m·g·d / I, где I считается от ФОРМЫ тела
    // (для стержня длиной L, подвешенного за конец, это m·L²/3) плюс перенос
    // оси. Если бы движок, как прежний встроенный, считал тело точкой массы,
    // I равнялось бы m·d², то есть на треть меньше, и брусок падал бы заметно
    // быстрее. Разница между двумя моделями — треть скорости, и она видна на
    // глаз.
    auto w = MakeWorld();

    // Брусок 0.2 x 4.0 x 0.2, положенный длинной стороной вдоль X.
    const float mass = 4.0f, length = 4.0f, d = length * 0.5f;
    BodyDesc rod;
    rod.Type = BodyType::Dynamic;
    rod.Shape = ShapeType::Box;
    rod.HalfExtents = {0.1f, 2.0f, 0.1f};
    rod.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    rod.Position = {d, 5.0f, 0.0f};
    rod.Mass = mass;
    const BodyHandle b = w->CreateBody(rod);

    JointDesc j;
    j.Type = JointType::Hinge;
    j.BodyA = b;
    j.BodyB = kInvalidBody;
    j.Anchor = {0.0f, 5.0f, 0.0f};
    j.Axis = {0.0f, 0.0f, 1.0f};
    w->CreateJoint(j);

    const float t = 0.3f;
    Run(*w, t);

    // Насколько провернулся вокруг петли.
    const glm::vec3 p = PosOf(*w, b);
    const float angle = std::atan2(5.0f - p.y, p.x);

    const float inertiaShape = mass * length * length / 3.0f;   // честная: 21.33
    const float inertiaPoint = mass * d * d;                    // точечная: 16.0
    const float expectShape = 0.5f * (mass * 9.81f * d / inertiaShape) * t * t;
    const float expectPoint = 0.5f * (mass * 9.81f * d / inertiaPoint) * t * t;
    std::printf("       за %.1f с провернулся на %.4f рад; по форме ожидается %.4f, "
                "как точка массы — %.4f\n", t, angle, expectShape, expectPoint);

    // Попали в предсказание по ФОРМЕ и ясно разошлись с точечным.
    CHECK_TRUE(std::abs(angle - expectShape) < expectShape * 0.25f);
    CHECK_TRUE(angle < (expectShape + expectPoint) * 0.5f);
}

// --- запросы по настоящим формам ----------------------------------------------------

TEST(Builtin_raycast_hits_the_real_face_of_a_rotated_box) {
    auto w = MakeWorld();
    BodyDesc d;
    d.Type = BodyType::Static;
    d.Shape = ShapeType::Box;
    d.HalfExtents = {1.0f, 1.0f, 1.0f};
    d.Position = {0.0f, 0.0f, 0.0f};
    d.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
    w->CreateBody(d);

    RayHit hit;
    CHECK_TRUE(w->Raycast({-10.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, hit, kAllLayers));
    // Куб, повёрнутый на 45°, встречает луч УГЛОМ на расстоянии √2 от центра,
    // а не гранью на расстоянии 1. Осевой габарит дал бы 1.0 и промахнулся бы
    // мимо истины на сорок процентов.
    const float dist = hit.Distance;
    std::printf("       луч попал на расстоянии %.3f (грань 9.0, угол %.3f)\n", dist,
                10.0f - std::sqrt(2.0f));
    CHECK_TRUE(std::abs(dist - (10.0f - std::sqrt(2.0f))) < 0.05f);
}

TEST(Builtin_sensor_detects_without_pushing) {
    auto w = MakeWorld();
    BodyDesc zone;
    zone.Type = BodyType::Static;
    zone.Shape = ShapeType::Box;
    zone.HalfExtents = {2.0f, 2.0f, 2.0f};
    zone.Position = {0.0f, 0.0f, 0.0f};
    zone.Sensor = true;
    w->CreateBody(zone);

    const BodyHandle faller = Box(*w, {0.0f, 6.0f, 0.0f}, glm::vec3(0.3f));

    bool sawSensor = false;
    const float dt = 1.0f / 60.0f;
    std::vector<ContactEvent> events;
    for (int i = 0; i < 240; ++i) {
        w->Step(dt);
        w->PollContacts(events);
        for (const ContactEvent& e : events)
            if (e.Sensor && e.When == ContactEvent::Phase::Begin) sawSensor = true;
    }
    const float y = PosOf(*w, faller).y;
    std::printf("       сенсор сработал=%d, тело провалилось до y=%.2f\n", (int)sawSensor, y);
    CHECK_TRUE(sawSensor);
    CHECK_TRUE(y < -2.0f);   // сенсор НЕ остановил падение
}
