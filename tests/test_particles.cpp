// ===========================================================================
//  Частицы: эффект данными, модули и их сочетания — без видеокарты.
//
//  ЧТО ЛОМАЛОСЬ И ЗАЧЕМ ЭТИ ПРОВЕРКИ. Частица знала две точки («в начале» и
//  «в конце») и тяготение по Y; всё прочее — огонь, дым, искры — было кодом
//  движка (пресетами). Дождь со снегом, ветер, турбулентность, столкновения,
//  текстура с раскадровкой, след — не выражались вовсе. Здесь проверяется, что
//  каждый модуль делает то, что обещает, и что они складываются: эффект
//  собирается данными, а не кодом.
// ===========================================================================
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h"
#include "ParticleUi.h"
#include "sage/render/ParticleCurves.h"
#include "sage/render/ParticleEffect.h"
#include "sage/render/ParticleEffectIO.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/ParticleSim.h"
#include "sage/render/ParticleSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

using namespace sage::fx;

namespace {

constexpr float kDt = 1.0f / 60.0f;
const glm::mat4 kIdentity(1.0f);

// Эффект без разброса и без самопроизвольного рождения: частицы — только те,
// что проверка родила сама.
ParticleEffect Quiet() {
    ParticleEffect f;
    f.RateOverTime = 0.0f;
    f.StartLifetime = {10.0f, 10.0f};
    f.StartSpeed = {0.0f, 0.0f};
    f.StartSize = {1.0f, 1.0f};
    f.Shape = EmitShape::Point;
    return f;
}

void Run(ParticleEmitter& e, const ParticleEffect& f, float seconds, const glm::mat4& w = kIdentity) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += kDt) e.Step(f, w, kDt);
}

} // namespace

// --- 1. Кривая и градиент ---------------------------------------------------------
TEST(Particles_curve_and_gradient_evaluate_between_keys) {
    Curve c{{{0.0f, 0.0f}, {0.5f, 2.0f}, {1.0f, 1.0f}}};
    CHECK_NEAR(c.Evaluate(0.25f), 1.0f, 1e-5f);
    CHECK_NEAR(c.Evaluate(0.75f), 1.5f, 1e-5f);
    CHECK_NEAR(c.Evaluate(-1.0f), 0.0f, 1e-5f);   // за краем — крайнее значение
    CHECK_NEAR(c.Evaluate(2.0f), 1.0f, 1e-5f);
    // Пустая кривая — единица: «модуль ничего не меняет», а не ноль.
    CHECK_NEAR(Curve{}.Evaluate(0.3f), 1.0f, 1e-6f);

    // Цвет и прозрачность — разными ключами.
    Gradient g;
    g.Colors = {{0.0f, {1.0f, 0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f, 1.0f}}};
    g.Alphas = {{0.0f, 1.0f}, {0.8f, 1.0f}, {1.0f, 0.0f}};
    const glm::vec4 mid = g.Evaluate(0.5f);
    CHECK_NEAR(mid.r, 0.5f, 1e-5f);
    CHECK_NEAR(mid.b, 0.5f, 1e-5f);
    CHECK_NEAR(mid.a, 1.0f, 1e-5f);
    CHECK_NEAR(g.Evaluate(0.9f).a, 0.5f, 1e-5f);
}

// --- 2. Форма испускания --------------------------------------------------------------
TEST(Particles_emit_shapes_keep_their_bounds) {
    ParticleEmitter e;
    ParticleEffect f = Quiet();
    glm::vec3 pos, dir;

    f.Shape = EmitShape::Cone;
    f.Radius = 0.5f;
    f.ConeAngle = 20.0f;
    for (int i = 0; i < 500; ++i) {
        e.SampleShape(f, pos, dir);
        CHECK_TRUE(std::abs(pos.y) < 1e-5f);
        CHECK_TRUE(glm::length(glm::vec2(pos.x, pos.z)) <= 0.5f + 1e-4f);
        // Направление не выходит из конуса.
        CHECK_TRUE(glm::degrees(std::acos(glm::clamp(dir.y, -1.0f, 1.0f))) <= 20.0f + 0.01f);
    }

    f.Shape = EmitShape::Box;
    f.BoxSize = {2.0f, 1.0f, 4.0f};
    for (int i = 0; i < 500; ++i) {
        e.SampleShape(f, pos, dir);
        CHECK_TRUE(std::abs(pos.x) <= 1.0f + 1e-5f && std::abs(pos.y) <= 0.5f + 1e-5f &&
                   std::abs(pos.z) <= 2.0f + 1e-5f);
    }

    f.Shape = EmitShape::Sphere;
    f.Radius = 3.0f;
    f.FromShell = true;
    for (int i = 0; i < 200; ++i) {
        e.SampleShape(f, pos, dir);
        CHECK_NEAR(glm::length(pos), 3.0f, 1e-3f);
    }

    f.Shape = EmitShape::Hemisphere;
    for (int i = 0; i < 200; ++i) {
        e.SampleShape(f, pos, dir);
        CHECK_TRUE(dir.y >= -1e-5f);   // только вверх
    }
}

// --- 3. Испускание: по времени, залпы, конец цикла ------------------------------------
TEST(Particles_rate_bursts_and_non_looping_cycle) {
    ParticleEffect f = Quiet();
    f.RateOverTime = 30.0f;
    ParticleEmitter e;
    Run(e, f, 1.0f);
    CHECK_TRUE(std::abs((int)e.Particles().size() - 30) <= 1);

    // Залп: 25 частиц на 0.5 с, дважды через 0.2 с.
    ParticleEffect b = Quiet();
    b.Duration = 2.0f;
    b.Bursts.push_back({0.5f, 25, 25, 2, 0.2f});
    ParticleEmitter eb;
    Run(eb, b, 0.45f);
    CHECK_EQ((int)eb.Particles().size(), 0);
    Run(eb, b, 0.1f);
    CHECK_EQ((int)eb.Particles().size(), 25);
    Run(eb, b, 0.5f);
    CHECK_EQ((int)eb.Particles().size(), 50);

    // Неповторяющийся цикл кончился — рождения нет, эмиттер доживает.
    ParticleEffect once = Quiet();
    once.RateOverTime = 10.0f;
    once.Loop = false;
    once.Duration = 1.0f;
    once.StartLifetime = {0.5f, 0.5f};
    ParticleEmitter eo;
    Run(eo, once, 3.0f);
    CHECK_EQ((int)eo.Particles().size(), 0);
    CHECK_FALSE(eo.Alive());
}

// --- 4. Разогрев и испускание по пути -------------------------------------------------
TEST(Particles_prewarm_and_rate_over_distance) {
    ParticleEffect f = Quiet();
    f.RateOverTime = 20.0f;
    f.Duration = 2.0f;
    f.Prewarm = true;
    ParticleEmitter e;
    e.Step(f, kIdentity, kDt);
    // Костёр в начале уровня уже горит: один цикл прокручен заранее.
    CHECK_TRUE(e.Particles().size() >= 35);

    ParticleEffect d = Quiet();
    d.RateOverDistance = 10.0f;
    ParticleEmitter ed;
    for (int i = 0; i <= 60; ++i)
        ed.Step(d, glm::translate(kIdentity, glm::vec3(i * 0.05f, 0.0f, 0.0f)), kDt);   // 3 м
    CHECK_TRUE(std::abs((int)ed.Particles().size() - 30) <= 1);
    // Рождены ВДОЛЬ пути, а не кучкой в конце.
    float minX = 1e9f, maxX = -1e9f;
    for (const Particle& p : ed.Particles()) { minX = std::min(minX, p.Position.x); maxX = std::max(maxX, p.Position.x); }
    CHECK_TRUE(maxX - minX > 2.5f);
}

// --- 5. Силы: тяготение, ветер, сопротивление, потолок скорости ------------------------
TEST(Particles_forces_gravity_wind_drag_and_speed_limit) {
    ParticleEffect f = Quiet();
    f.GravityScale = 1.0f;
    ParticleEmitter e;
    e.Emit(f, kIdentity, 1);
    Run(e, f, 1.0f);
    CHECK_NEAR(e.Particles()[0].Velocity.y, -9.81f, 0.2f);

    // Ветер: неподвижная частица набирает скорость ветра.
    ParticleEffect w = Quiet();
    w.UseForces = true;
    w.Wind = {3.0f, 0.0f, 0.0f};
    w.WindInfluence = 2.0f;
    ParticleEmitter ew;
    ew.Emit(w, kIdentity, 1);
    Run(ew, w, 4.0f);
    CHECK_NEAR(ew.Particles()[0].Velocity.x, 3.0f, 0.1f);

    // Сопротивление гасит скорость, потолок её режет.
    ParticleEffect d = Quiet();
    d.StartSpeed = {10.0f, 10.0f};
    d.UseForces = true;
    d.Drag = 2.0f;
    ParticleEmitter ed;
    ed.Emit(d, kIdentity, 1);
    Run(ed, d, 1.0f);
    CHECK_TRUE(glm::length(ed.Particles()[0].Velocity) < 2.0f);

    ParticleEffect m = Quiet();
    m.StartSpeed = {10.0f, 10.0f};
    m.UseForces = true;
    m.MaxSpeed = 3.0f;
    ParticleEmitter em;
    em.Emit(m, kIdentity, 1);
    Run(em, m, kDt);
    CHECK_TRUE(glm::length(em.Particles()[0].Velocity) <= 3.0f + 1e-4f);
}

// --- 6. Турбулентность: поле гладкое и действительно толкает ------------------------------
TEST(Particles_turbulence_is_smooth_and_moves_particles) {
    const glm::vec3 a = NoiseField({1.0f, 2.0f, 3.0f}, 0.5f, 2);
    const glm::vec3 b = NoiseField({1.001f, 2.0f, 3.0f}, 0.5f, 2);
    CHECK_TRUE(glm::length(a - b) < 0.02f);   // соседние точки — почти одна сила

    ParticleEffect f = Quiet();
    f.UseNoise = true;
    f.NoiseStrength = 3.0f;
    ParticleEmitter e;
    e.Emit(f, kIdentity, 20);
    Run(e, f, 1.0f);
    // Частицы из одной точки разошлись — каждая в своё завихрение.
    CHECK_TRUE(glm::length(e.Particles()[0].Position) > 0.05f);
}

// --- 7. Столкновение с плоскостью: не проваливается, отскакивает -------------------------
TEST(Particles_collide_with_plane_and_bounce) {
    ParticleEffect f = Quiet();
    f.GravityScale = 1.0f;
    f.UseCollision = true;
    f.Collision = CollisionMode::Plane;
    f.PlaneHeight = 0.0f;
    f.Bounce = 0.5f;
    ParticleEmitter e;
    e.Emit(f, glm::translate(kIdentity, glm::vec3(0.0f, 2.0f, 0.0f)), 1);
    bool bounced = false;
    float lowest = 1e9f;
    for (int i = 0; i < 180; ++i) {
        e.Step(f, glm::translate(kIdentity, glm::vec3(0.0f, 2.0f, 0.0f)), kDt);
        lowest = std::min(lowest, e.Particles()[0].Position.y);
        if (e.Particles()[0].Velocity.y > 0.5f) bounced = true;
    }
    CHECK_TRUE(lowest >= -1e-3f);
    CHECK_TRUE(bounced);

    // Столкновение с миром — лучом, который даёт вызывающий.
    ParticleEffect w = f;
    w.Collision = CollisionMode::World;
    CollisionQuery wall = [](const glm::vec3& from, const glm::vec3& to, CollisionHit& hit) {
        if (from.y >= 1.0f && to.y < 1.0f) {
            hit.Point = glm::vec3(from.x, 1.0f, from.z);
            hit.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
            return true;
        }
        return false;
    };
    ParticleEmitter ew;
    ew.Emit(w, glm::translate(kIdentity, glm::vec3(0.0f, 3.0f, 0.0f)), 1);
    for (int i = 0; i < 120; ++i) ew.Step(w, kIdentity, kDt, &wall);
    CHECK_TRUE(ew.Particles()[0].Position.y >= 1.0f - 1e-3f);
}

// --- 8. Вид по времени жизни: размер, цвет, кадры ------------------------------------------
TEST(Particles_size_colour_and_flipbook_over_lifetime) {
    ParticleEffect f = Quiet();
    f.StartLifetime = {1.0f, 1.0f};
    f.UseSizeOverLifetime = true;
    f.SizeOverLifetime = Curve{{{0.0f, 0.0f}, {0.5f, 2.0f}, {1.0f, 0.0f}}};
    f.UseColorOverLifetime = true;
    f.ColorOverLifetime = Gradient::Fade({1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
    f.Texture = "sheet.png";
    f.TilesX = 2;
    f.TilesY = 2;
    Particle p;
    p.Size = 1.0f;
    p.Lifetime = 1.0f;
    p.Age = 0.5f;
    CHECK_NEAR(ParticleEmitter::SizeOf(f, p), 2.0f, 1e-4f);
    CHECK_NEAR(ParticleEmitter::ColorOf(f, p).g, 0.5f, 1e-4f);
    CHECK_NEAR(ParticleEmitter::ColorOf(f, p).a, 0.5f, 1e-4f);
    CHECK_EQ(ParticleEmitter::FrameCount(f), 4);
    CHECK_EQ(ParticleEmitter::FrameOf(f, p, 4), 2);
    p.Age = 1.0f;   // последнее мгновение — последний кадр, а не снова первый
    CHECK_EQ(ParticleEmitter::FrameOf(f, p, 4), 3);
    f.Flipbook = FlipbookMode::Speed;
    f.FrameRate = 10.0f;
    p.Age = 0.55f;
    CHECK_EQ(ParticleEmitter::FrameOf(f, p, 4), 1);   // 5-й кадр по кругу
    // Кадры отдельными файлами: их число — число кадров.
    f.Frames = {"a.png", "b.png", "c.png"};
    CHECK_EQ(ParticleEmitter::FrameCount(f), 3);
}

// --- 9. След: точки копятся и стареют ------------------------------------------------------
TEST(Particles_trail_records_points_and_forgets_old_ones) {
    ParticleEffect f = Quiet();
    f.StartSpeed = {5.0f, 5.0f};
    f.Shape = EmitShape::Edge;
    f.EdgeLength = 0.0f;
    f.Render = RenderMode::Trail;
    f.TrailLifetime = 0.2f;
    f.TrailMinDistance = 0.01f;
    ParticleEmitter e;
    e.Emit(f, kIdentity, 1);
    Run(e, f, 1.0f);
    const Particle& p = e.Particles()[0];
    CHECK_TRUE(p.Trail.size() >= 8);
    // Самая старая точка — не старше времени жизни следа.
    CHECK_TRUE(e.TotalTime() - p.Trail.front().w <= 0.2f + kDt);
    // Точки идут за частицей (вверх по Y): хвост ниже головы.
    CHECK_TRUE(p.Trail.front().y < p.Trail.back().y);
}

// --- 10. Локальное пространство: частицы едут с объектом ----------------------------------
TEST(Particles_local_space_follows_the_emitter) {
    ParticleEffect f = Quiet();
    f.Space = SimulationSpace::Local;
    ParticleEmitter e;
    e.Emit(f, kIdentity, 1);
    e.Step(f, glm::translate(kIdentity, glm::vec3(5.0f, 0.0f, 0.0f)), kDt);
    // В осях эмиттера частица стоит на месте — в мире она уехала с ним.
    CHECK_NEAR(glm::length(e.Particles()[0].Position), 0.0f, 1e-4f);

    ParticleEffect w = Quiet();
    ParticleEmitter ew;
    ew.Emit(w, kIdentity, 1);
    ew.Step(w, glm::translate(kIdentity, glm::vec3(5.0f, 0.0f, 0.0f)), kDt);
    CHECK_NEAR(ew.Particles()[0].Position.x, 0.0f, 1e-4f);   // мировая осталась, где родилась
}

// --- 11. Дочерний эффект: осколки там, где умерла частица ---------------------------------
TEST(Particles_sub_emitter_spawns_where_a_particle_dies) {
    ParticleSystem sys;   // без видеокарты: буферы создаются только при отрисовке
    ParticleEffect child = Quiet();
    child.StartLifetime = {5.0f, 5.0f};
    sys.SetEffectLoader([&](const std::string& path, ParticleEffect& out) {
        if (path != "sparks.sagefx") return false;
        out = child;
        return true;
    });
    ParticleEffect parent = Quiet();
    parent.StartLifetime = {0.2f, 0.2f};
    parent.SubEmitters.push_back({SubEmitter::Trigger::Death, "sparks.sagefx", 7, true, 0.0f});
    sys.Burst(parent, glm::vec3(1.0f, 2.0f, 3.0f), 2);
    CHECK_EQ((int)sys.AliveCount(), 2);
    for (int i = 0; i < 20; ++i) sys.Update(kDt);
    // Родители умерли, на их месте — по 7 дочерних.
    CHECK_EQ((int)sys.AliveCount(), 14);
}

// --- 12. Эмиттер объекта: пропал объект — частицы доживают ---------------------------------
TEST(Particles_orphaned_emitter_lets_particles_finish) {
    Scene scene("fx");
    GameObject obj = scene.CreateObject("Torch");
    ParticleEmitterComponent& pe = scene.Registry().emplace<ParticleEmitterComponent>(obj.Entity());
    pe.Effect = Quiet();
    pe.Effect.RateOverTime = 50.0f;
    pe.Effect.StartLifetime = {0.5f, 0.5f};
    ParticleSystem sys;
    for (int i = 0; i < 30; ++i) UpdateEmitters(scene, sys, kDt);
    const size_t alive = sys.AliveCount();
    CHECK_TRUE(alive > 10);
    CHECK_EQ((int)sys.EmitterCount(), 1);

    scene.RemoveObject(obj.Id());
    UpdateEmitters(scene, sys, kDt);
    CHECK_TRUE(sys.AliveCount() > 0);   // не исчезли рывком
    for (int i = 0; i < 60; ++i) UpdateEmitters(scene, sys, kDt);
    CHECK_EQ((int)sys.AliveCount(), 0);
    CHECK_EQ((int)sys.EmitterCount(), 0);   // эмиттер убран, когда всё догорело
}

// --- 13. Файл эффекта: всё переживает запись и чтение ---------------------------------------
TEST(Particles_effect_survives_json_round_trip) {
    ParticleEffect f;
    f.Duration = 3.5f;
    f.Loop = false;
    f.Shape = EmitShape::Box;
    f.BoxSize = {4.0f, 0.5f, 4.0f};
    f.Bursts.push_back({0.25f, 3, 9, 4, 0.1f});
    f.UseForces = true;
    f.Wind = {1.0f, 0.0f, -2.0f};
    f.Gustiness = 0.4f;
    f.UseNoise = true;
    f.NoiseOctaves = 3;
    f.UseSizeOverLifetime = true;
    f.SizeOverLifetime = Curve{{{0.0f, 0.2f}, {0.3f, 1.0f}, {1.0f, 0.0f}}};
    f.UseColorOverLifetime = true;
    f.ColorOverLifetime.Colors = {{0.0f, {1.0f, 0.5f, 0.0f}}, {1.0f, {0.2f, 0.2f, 0.2f}}};
    f.ColorOverLifetime.Alphas = {{0.0f, 0.0f}, {0.1f, 1.0f}, {1.0f, 0.0f}};
    f.UseCollision = true;
    f.Collision = CollisionMode::World;
    f.Frames = {"assets/fx/s_0.png", "assets/fx/s_1.png"};
    f.Flipbook = FlipbookMode::Random;
    f.PixelArt = true;
    f.Render = RenderMode::Trail;
    f.Blend = BlendMode::Additive;
    f.TrailWidth = Curve::Linear(1.0f, 0.0f);
    f.SubEmitters.push_back({SubEmitter::Trigger::Collision, "assets/fx/splash.sagefx", 12, false, 0.5f});

    ParticleEffect back;
    CHECK_TRUE(EffectFromJson(EffectToJson(f), back));
    CHECK_NEAR(back.Duration, 3.5f, 1e-5f);
    CHECK_FALSE(back.Loop);
    CHECK_TRUE(back.Shape == EmitShape::Box);
    CHECK_NEAR(back.BoxSize.x, 4.0f, 1e-5f);
    CHECK_EQ((int)back.Bursts.size(), 1);
    CHECK_EQ(back.Bursts[0].CountMax, 9);
    CHECK_NEAR(back.Wind.z, -2.0f, 1e-5f);
    CHECK_EQ(back.NoiseOctaves, 3);
    CHECK_EQ((int)back.SizeOverLifetime.Keys.size(), 3);
    CHECK_NEAR(back.ColorOverLifetime.Evaluate(0.05f).a, 0.5f, 1e-4f);
    CHECK_TRUE(back.Collision == CollisionMode::World);
    CHECK_EQ((int)back.Frames.size(), 2);
    CHECK_TRUE(back.Flipbook == FlipbookMode::Random);
    CHECK_TRUE(back.PixelArt);
    CHECK_TRUE(back.Render == RenderMode::Trail);
    CHECK_TRUE(back.Blend == BlendMode::Additive);
    CHECK_EQ((int)back.SubEmitters.size(), 1);
    CHECK_TRUE(back.SubEmitters[0].When == SubEmitter::Trigger::Collision);
    CHECK_EQ(back.SubEmitters[0].Count, 12);

    // Терпимость: чужое поле пропускается, битый тип — умолчание, не JSON — отказ.
    ParticleEffect tolerant;
    CHECK_TRUE(EffectFromJson(R"({"futureField": 1, "duration": "не число", "radius": 2.0})", tolerant));
    CHECK_NEAR(tolerant.Duration, ParticleEffect{}.Duration, 1e-6f);
    CHECK_NEAR(tolerant.Radius, 2.0f, 1e-6f);
    CHECK_FALSE(EffectFromJson("{ это не JSON", tolerant));
}

// --- 14. Старая сцена: прежний эмиттер открывается и выглядит как раньше -------------------
TEST(Particles_old_scene_emitter_is_migrated) {
    Scene scene("old");
    GameObject obj = scene.CreateObject("Old");
    scene.Registry().emplace<ParticleEmitterComponent>(obj.Entity());
    nlohmann::json j = nlohmann::json::parse(SceneSerializer::SaveToString(scene));
    // Подменяем эмиттер записью старого формата.
    for (auto& e : j["objects"]) {
        if (!e.contains("particles")) continue;
        e["particles"] = nlohmann::json::parse(R"({
            "active": true, "continuous": true, "emissionRate": 42.0,
            "speedMin": 1.0, "speedMax": 3.0, "gravity": -9.81,
            "startSizeMin": 0.2, "startSizeMax": 0.2, "endSizeMin": 0.0, "endSizeMax": 0.0,
            "startColor": {"x": 1.0, "y": 0.8, "z": 0.2, "w": 1.0},
            "endColor": {"x": 1.0, "y": 0.1, "z": 0.0, "w": 0.0},
            "shape": "quad", "angularVelocityMax": 2.0})");
    }
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(j.dump());
    bool found = false;
    for (auto e : loaded->Registry().view<ParticleEmitterComponent>()) {
        const ParticleEffect& f = loaded->Registry().get<ParticleEmitterComponent>(e).Effect;
        found = true;
        CHECK_NEAR(f.RateOverTime, 42.0f, 1e-4f);
        CHECK_NEAR(f.GravityScale, 1.0f, 1e-3f);          // старое −9.81 по Y — это одно «g»
        CHECK_NEAR(f.StartSpeed.Max, 3.0f, 1e-4f);
        CHECK_TRUE(f.Sprite == ParticleSprite::Square);
        CHECK_TRUE(f.UseColorOverLifetime);
        CHECK_NEAR(f.ColorOverLifetime.Evaluate(0.0f).g, 0.8f, 1e-4f);
        CHECK_NEAR(f.ColorOverLifetime.Evaluate(1.0f).a, 0.0f, 1e-4f);
        CHECK_TRUE(f.UseSizeOverLifetime);
        CHECK_NEAR(f.SizeOverLifetime.Evaluate(1.0f), 0.0f, 1e-4f);
        CHECK_TRUE(f.UseRotation);
    }
    CHECK_TRUE(found);
}

// --- 15. Кадры по номеру: smoke_3.png → весь набор -----------------------------------------
TEST(Particles_numbered_frames_are_collected) {
    const std::set<std::string> files = {"fx/big_smoke_0.png", "fx/big_smoke_1.png", "fx/big_smoke_2.png",
                                         "fx/big_smoke_10.png", "fx/spark_01.png", "fx/spark_02.png",
                                         "fx/spark_03.png"};
    auto exists = [&](const std::string& f) { return files.count(f) > 0; };
    std::vector<std::string> a = sage::editor::NumberedSequence("fx/big_smoke_2.png", exists);
    CHECK_EQ((int)a.size(), 3);   // 0,1,2 — дальше разрыв (3 нет), десятый не берётся
    if (a.size() == 3) CHECK_TRUE(a[0] == "fx/big_smoke_0.png");
    std::vector<std::string> b = sage::editor::NumberedSequence("fx/spark_02.png", exists);
    CHECK_EQ((int)b.size(), 3);   // с ведущим нулём и с единицы
    CHECK_TRUE(sage::editor::NumberedSequence("fx/flame.png", exists).empty());
}

// --- 16. Инспектор: все модули на экране — ни одного двойного идентификатора ----------------
TEST(Particles_inspector_has_no_duplicate_widget_ids) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(700, 4000);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    ParticleEffect f;
    f.UseVelocity = f.UseForces = f.UseNoise = f.UseSizeOverLifetime = f.UseSizeBySpeed = true;
    f.UseColorOverLifetime = f.UseColorBySpeed = f.UseRotation = f.UseCollision = true;
    f.Render = RenderMode::Trail;
    f.Texture = "t.png";
    f.Frames = {"a.png", "b.png"};
    f.Bursts = {Burst{}, Burst{}};
    f.SubEmitters = {SubEmitter{}, SubEmitter{}};
    sage::editor::ParticleUiHooks hooks;
    hooks.TextureSlot = [](const char* id, std::string&) {
        ImGui::Button(id);
        return false;
    };
    hooks.EffectSlot = hooks.TextureSlot;

    int worst = 0;
    {
        for (float y = 4.0f; y < 3980.0f; y += 6.0f) {
            for (float x : {30.0f, 250.0f, 600.0f}) {
                io.AddMousePosEvent(x, y);
                for (int fr = 0; fr < 2; ++fr) {
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(700, 4000));
                    ImGui::Begin("Particles", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    // Все разделы раскрыты — иначе их содержимое не на экране.
                    ImGuiWindow* win = ImGui::GetCurrentWindow();
                    for (const char* s : {"Main", "Emission", "Shape", "Velocity over lifetime", "Forces",
                                          "Turbulence", "Size over lifetime", "Size by speed",
                                          "Colour over lifetime", "Colour by speed", "Rotation", "Collision",
                                          "Texture and frames", "Rendering", "Sub-emitters"}) {
                        ImGui::PushID("particle-effect");
                        ImGui::PushID(s);
                        win->DC.StateStorage->SetInt(ImGui::GetID(s), 1);
                        ImGui::PopID();
                        ImGui::PopID();
                    }
                    sage::editor::DrawParticleEffect(f, hooks);
                    ImGui::End();
                    ImGui::Render();
                    worst = std::max(worst, ctx->HoveredIdPreviousFrameItemCount);
                }
            }
        }
    }
    ImGui::DestroyContext(ctx);
    CHECK_TRUE(worst <= 1);
}
