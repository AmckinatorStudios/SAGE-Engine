// Ревизия ВСЕХ компонентов сцены: доживает ли каждый до файла и обратно.
//
// ЗАЧЕМ ЭТОТ ФАЙЛ. Компонент в движке живёт не сам по себе, а по цепочке:
// его можно добавить в инспекторе → он сохраняется в сцену → его исполняет
// какая-то система → до него дотягивается скрипт. Цепочка рвётся молча: объект
// добавлен, поля выставлены, всё выглядит рабочим — а после сохранения и
// открытия сцены настройки исчезли, потому что сериализатор про этот компонент
// не знает. Ни сборка, ни один тест этого не замечали.
//
// Здесь проверяется САМОЕ ХРУПКОЕ ЗВЕНО — сохранение и загрузка. Тест ведёт
// список компонентов явно: новый компонент, не дописанный сюда, обязан ронять
// проверку полноты внизу файла, а не тихо появляться без сериализации.
#include "TestFramework.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "sage/anim/AnimationComponents.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/LodGroup.h"
#include "sage/render/ParticleComponents.h"
#include "sage/render/ReflectionComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/ui/UIComponents.h"
#include "sage/physics/PhysicsComponents.h"

namespace fs = std::filesystem;

namespace {

// Сохраняет сцену и читает обратно. Возвращает загруженную — дальше её
// расспрашивают о компонентах.
std::unique_ptr<Scene> RoundTrip(Scene& scene, const char* name) {
    const std::string path = std::string("sage_components_") + name + ".sage";
    SceneSerializer::Save(scene, path);
    std::unique_ptr<Scene> loaded = SceneSerializer::Load(path);
    std::remove(path.c_str());
    return loaded;
}

// Первая сущность с данным компонентом (или entt::null).
template <typename T>
GameObject Find(Scene& scene) {
    auto view = scene.Registry().view<T>();
    for (auto e : view) return GameObject(&scene.Registry(), e);
    return GameObject(&scene.Registry(), entt::null);
}

} // namespace

TEST(Components_survive_a_scene_round_trip) {
    Scene scene("Все компоненты");

    // Один объект — один компонент: так провал называет виновника сам, а не
    // «что-то в сцене не сохранилось».
    GameObject light = scene.CreateObject("Light");
    LightComponent& lc = light.Registry()->emplace<LightComponent>(light.Entity());
    lc.Intensity = 3.25f;
    lc.Range = 17.5f;

    GameObject camera = scene.CreateObject("Camera");
    CameraComponent& cc = camera.Registry()->emplace<CameraComponent>(camera.Entity());
    cc.Fov = 42.0f;
    cc.Primary = true;

    GameObject script = scene.CreateObject("Script");
    script.Registry()->emplace<ScriptComponent>(script.Entity()).Path = "assets/scripts/x.lua";

    GameObject body = scene.CreateObject("Body");
    RigidBodyComponent& rb = body.Registry()->emplace<RigidBodyComponent>(body.Entity());
    rb.Mass = 7.5f;
    ColliderComponent& col = body.Registry()->emplace<ColliderComponent>(body.Entity());
    col.Radius = 2.5f;

    GameObject emitter = scene.CreateObject("Emitter");
    emitter.Registry()->emplace<ParticleEmitterComponent>(emitter.Entity()).BurstCount = 55;

    GameObject probe = scene.CreateObject("Probe");
    probe.Registry()->emplace<ReflectionProbeComponent>(probe.Entity()).Resolution = 64;

    GameObject ui = scene.CreateObject("UI");
    ui.Registry()->emplace<UIElementComponent>(ui.Entity()).Text = "кнопка";

    std::unique_ptr<Scene> loaded = RoundTrip(scene, "core");
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;

    GameObject l = Find<LightComponent>(*loaded);
    CHECK_TRUE(l.Valid());
    if (l.Valid()) {
        CHECK_NEAR(l.Registry()->get<LightComponent>(l.Entity()).Intensity, 3.25f, 1e-4);
        CHECK_NEAR(l.Registry()->get<LightComponent>(l.Entity()).Range, 17.5f, 1e-4);
    }

    GameObject c = Find<CameraComponent>(*loaded);
    CHECK_TRUE(c.Valid());
    if (c.Valid()) {
        CHECK_NEAR(c.Registry()->get<CameraComponent>(c.Entity()).Fov, 42.0f, 1e-4);
        CHECK_TRUE(c.Registry()->get<CameraComponent>(c.Entity()).Primary);
    }

    GameObject s = Find<ScriptComponent>(*loaded);
    CHECK_TRUE(s.Valid());
    if (s.Valid()) {
        CHECK_TRUE(s.Registry()->get<ScriptComponent>(s.Entity()).Path == "assets/scripts/x.lua");
    }

    GameObject b = Find<RigidBodyComponent>(*loaded);
    CHECK_TRUE(b.Valid());
    if (b.Valid()) CHECK_NEAR(b.Registry()->get<RigidBodyComponent>(b.Entity()).Mass, 7.5f, 1e-4);

    GameObject co = Find<ColliderComponent>(*loaded);
    CHECK_TRUE(co.Valid());
    if (co.Valid()) CHECK_NEAR(co.Registry()->get<ColliderComponent>(co.Entity()).Radius, 2.5f, 1e-4);

    GameObject em = Find<ParticleEmitterComponent>(*loaded);
    CHECK_TRUE(em.Valid());
    if (em.Valid()) {
        CHECK_EQ(em.Registry()->get<ParticleEmitterComponent>(em.Entity()).BurstCount, 55);
    }

    GameObject pr = Find<ReflectionProbeComponent>(*loaded);
    CHECK_TRUE(pr.Valid());
    if (pr.Valid()) {
        CHECK_EQ(pr.Registry()->get<ReflectionProbeComponent>(pr.Entity()).Resolution, 64);
    }

    GameObject u = Find<UIElementComponent>(*loaded);
    CHECK_TRUE(u.Valid());
    if (u.Valid()) CHECK_TRUE(u.Registry()->get<UIElementComponent>(u.Entity()).Text == "кнопка");
}

// --- Компоненты, которых сериализация НЕ ЗНАЕТ -------------------------------
//
// Эти два настраиваются (LOD — из C++ и импорта моделей, параметры шейдера — из
// Lua), исполняются рендером каждый кадр, но в файл сцены не попадают вовсе.
// То есть настройка живёт ровно до закрытия сцены.
//
// Тест написан как ОЖИДАНИЕ ПРАВИЛЬНОГО поведения и сейчас падает. Так и надо:
// он фиксирует дефект в исполняемом виде, а не в списке дел, и погаснет ровно
// тогда, когда сериализация научится этим двум.
TEST(Components_lod_and_shader_params_survive_a_round_trip) {
    Scene scene("LOD и параметры шейдера");

    GameObject lodObj = scene.CreateObject("WithLod");
    sage::render::LodComponent& lod =
        lodObj.Registry()->emplace<sage::render::LodComponent>(lodObj.Entity());
    lod.Auto = false;          // не «построить автоматически», а заданные вручную
    lod.AutoCount = 3;
    lod.Levels.Add(0.44f);
    lod.Levels.CullBelow = 0.02f;

    GameObject paramObj = scene.CreateObject("WithParams");
    ShaderParamsComponent& params =
        paramObj.Registry()->emplace<ShaderParamsComponent>(paramObj.Entity());
    params.Params["uGlow"] = ShaderParam::Make(2.5f);

    std::unique_ptr<Scene> loaded = RoundTrip(scene, "lod");
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;

    GameObject l = Find<sage::render::LodComponent>(*loaded);
    CHECK_TRUE(l.Valid());
    if (l.Valid()) {
        const auto& got = l.Registry()->get<sage::render::LodComponent>(l.Entity());
        CHECK_FALSE(got.Auto);
        CHECK_EQ(got.AutoCount, 3);
        CHECK_FALSE(got.Levels.Empty());
        if (!got.Levels.Empty()) CHECK_NEAR(got.Levels.ScreenHeights[0], 0.44f, 1e-4);
        CHECK_NEAR(got.Levels.CullBelow, 0.02f, 1e-4);
    }

    GameObject p = Find<ShaderParamsComponent>(*loaded);
    CHECK_TRUE(p.Valid());
    if (p.Valid()) {
        const auto& got = p.Registry()->get<ShaderParamsComponent>(p.Entity());
        CHECK_TRUE(got.Params.count("uGlow") == 1);
    }
}
