// Окружение сцены: схема свойств (sage/scene/EnvironmentSchema.h), цикл дня и
// ночи (sage/ecs/DayNightCycle.h) и их сохранение.
//
// Главное, что сторожится: у каждого типа неба и тумана видны ТОЛЬКО его
// свойства, выключенная система не показывает ничего, кроме включателя, а
// каждое поле LightingEnvironment, которое было доступно раньше, по-прежнему
// где-то в схеме есть — упрощение инспектора не должно было ничего отобрать.
#include "TestFramework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <string>

#include "sage/core/SystemScheduler.h"
#include "sage/ecs/DayNightCycle.h"
#include "sage/ecs/LightSystem.h"
#include "sage/scene/EnvironmentSchema.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace env = sage::env;
using Src = SkyboxSettings::Source;

namespace {
bool Active(const char* key, const LightingEnvironment& e) { return env::IsPropActive(key, e); }
} // namespace

TEST(Environment_each_sky_type_shows_only_its_properties) {
    LightingEnvironment e;
    e.Skybox.Enabled = true;

    e.Skybox.Kind = Src::Procedural;
    CHECK_TRUE(Active("sky.zenith", e));
    CHECK_TRUE(Active("sky.gradient", e));
    CHECK_FALSE(Active("sky.rotation", e));   // процедурному рендер поворот не применяет
    CHECK_FALSE(Active("sky.image", e));

    e.Skybox.Kind = Src::Solid;
    CHECK_TRUE(Active("sky.color", e));
    CHECK_FALSE(Active("sky.zenith", e));
    CHECK_FALSE(Active("sky.sun.size", e));
    CHECK_FALSE(Active("sky.exposure", e));

    e.Skybox.Kind = Src::Image;
    CHECK_TRUE(Active("sky.image", e));
    CHECK_TRUE(Active("sky.rotation", e));
    CHECK_TRUE(Active("sky.exposure", e));
    CHECK_FALSE(Active("sky.folder", e));

    e.Skybox.Kind = Src::Cubemap;
    CHECK_TRUE(Active("sky.folder", e));
    CHECK_FALSE(Active("sky.image", e));

    e.Skybox.Kind = Src::Faces;
    for (const char* f : {"sky.face.px", "sky.face.nx", "sky.face.py", "sky.face.ny", "sky.face.pz", "sky.face.nz"})
        CHECK_TRUE(Active(f, e));
    CHECK_TRUE(Active("sky.rotation", e));

    // Небо выключено — ни одного его свойства.
    e.Skybox.Enabled = false;
    CHECK_FALSE(Active("sky.face.px", e));
    CHECK_FALSE(Active("sky.rotation", e));
}

// Условия внутри процедурного неба: группы с включателем и зависимые поля.
TEST(Environment_procedural_groups_follow_their_toggles) {
    LightingEnvironment e;
    e.Skybox.Enabled = true;
    e.Skybox.Kind = Src::Procedural;
    e.Skybox.Celestials = false;
    e.Skybox.Clouds = false;
    CHECK_FALSE(Active("sky.sun.size", e));
    CHECK_FALSE(Active("sky.clouds.coverage", e));
    CHECK_FALSE(Active("sky.stars.density", e));
    e.Skybox.Celestials = true;
    e.Skybox.Clouds = true;
    CHECK_TRUE(Active("sky.sun.size", e));
    CHECK_TRUE(Active("sky.clouds.coverage", e));
    CHECK_TRUE(Active("sky.stars.density", e));
    // Ночные цвета — только при ночной палитре.
    e.Skybox.DayNight = false;
    CHECK_FALSE(Active("sky.nightZenith", e));
    CHECK_FALSE(Active("sky.clouds.nightColor", e));
    e.Skybox.DayNight = true;
    CHECK_TRUE(Active("sky.nightZenith", e));
}

TEST(Environment_fog_and_cycle_hide_everything_when_off) {
    LightingEnvironment e;
    e.Fog.Enabled = false;
    CHECK_FALSE(Active("fog.color", e));
    CHECK_FALSE(Active("fog.density", e));
    e.Fog.Enabled = true;
    e.Fog.Kind = FogSettings::Mode::Linear;
    CHECK_TRUE(Active("fog.end", e));
    CHECK_FALSE(Active("fog.density", e));
    e.Fog.Kind = FogSettings::Mode::ExponentialHeight;
    CHECK_TRUE(Active("fog.density", e));
    CHECK_FALSE(Active("fog.end", e));

    e.Cycle.Enabled = false;
    CHECK_FALSE(Active("cycle.time", e));
    e.Cycle.Enabled = true;
    CHECK_TRUE(Active("cycle.time", e));
    CHECK_TRUE(Active("cycle.speed", e));
    CHECK_TRUE(Active("cycle.dayLength", e));

    // Окружающий свет: свои цвета — только в режиме «свои значения».
    e.AmbientMode = LightingEnvironment::AmbientSource::FromSky;
    CHECK_FALSE(Active("ambient.sky", e));
    CHECK_TRUE(Active("ambient.intensity", e));
    e.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    CHECK_TRUE(Active("ambient.sky", e));
}

// НИЧЕГО НЕ ОТОБРАНО: каждое настраиваемое поле окружения, которое было в
// прежней панели, есть в схеме (по адресу поля, а не по имени).
TEST(Environment_schema_covers_every_existing_setting) {
    LightingEnvironment e;
    std::set<const void*> covered;
    for (const env::System& s : env::Systems()) {
        if (s.Enabled) covered.insert(s.Enabled(e));
        auto scan = [&](const std::vector<env::Group>& groups) {
            for (const env::Group& g : groups) {
                if (g.Toggle) covered.insert(g.Toggle(e));
                for (const env::Prop& p : g.Props)
                    if (p.Field) covered.insert(p.Field(e));
            }
        };
        for (const env::Variant& v : s.Variants) scan(v.Groups);
        scan(s.Common);
    }
    const SkyboxSettings& k = e.Skybox;
    const void* expected[] = {
        &k.Enabled, &k.TopColor, &k.HorizonColor, &k.DayNight, &k.NightTopColor, &k.NightHorizonColor,
        &k.DuskColor, &k.MoonlightColor, &k.MoonlightIntensity, &k.Celestials, &k.SunColor, &k.SunSize,
        &k.Moon, &k.MoonColor, &k.MoonSize, &k.StarIntensity, &k.GradientExponent, &k.HorizonSoftness,
        &k.HorizonOffset, &k.Ground, &k.GroundColor, &k.NightGroundColor, &k.GroundBlend, &k.SunBrightness,
        &k.SunGlow, &k.MoonPhase, &k.SunTexture, &k.MoonTexture, &k.Filtering, &k.StarDensity, &k.StarSize,
        &k.Clouds, &k.CloudColor, &k.NightCloudColor, &k.CloudHeight, &k.CloudScale, &k.CloudCoverage,
        &k.CloudOpacity, &k.CloudWind, &k.CloudFade, &k.CubemapDir, &k.FacePaths[0], &k.FacePaths[5],
        &k.ImagePath, &k.ImageLayout, &k.Intensity, &k.RotationDeg,
        &e.Fog.Enabled, &e.Fog.Color, &e.Fog.Start, &e.Fog.End, &e.Fog.Density, &e.Fog.HeightFalloff,
        &e.Fog.BaseHeight, &e.Fog.MaxOpacity, &e.Fog.SunScatter, &e.Fog.SunExponent,
        &e.SkyColor, &e.GroundColor, &e.AmbientStrength, &e.Shadows.Distance,
        &e.Cycle.Enabled, &e.Cycle.Time, &e.Cycle.Speed, &e.Cycle.DayLengthMinutes, &e.Cycle.RunInPlay,
        &e.Cycle.SunAzimuth, &e.Cycle.NoonElevation,
    };
    int missing = 0;
    for (const void* p : expected)
        if (!covered.count(p)) ++missing;
    CHECK_EQ(missing, 0);
    // Один ключ — одно поле: по ключу файловый диалог находит поле через кадр.
    // Общие поля текстурных небес (поворот, экспозиция) повторяются в трёх
    // типах под одним ключом — это то же самое поле, а не два разных.
    std::map<std::string, const void*> keys;
    int clash = 0;
    for (const env::System& s : env::Systems())
        for (const env::Variant& v : s.Variants)
            for (const env::Group& g : v.Groups)
                for (const env::Prop& p : g.Props) {
                    const void* f = p.Field ? p.Field(e) : nullptr;
                    auto [it, fresh] = keys.emplace(p.Key, f);
                    if (!fresh && it->second != f) ++clash;
                }
    CHECK_EQ(clash, 0);
}

// Время суток превращается в направление солнца: полдень — высоко, 6 и 18 —
// на горизонте, полночь — под ним.
TEST(Environment_cycle_time_sets_the_sun) {
    DayNightCycle c;
    c.NoonElevation = 60.0f;
    auto height = [&](float t) {
        c.Time = t;
        return -sage::ecs::SunDirectionAt(c).y;   // вверх — на солнце
    };
    CHECK_NEAR(height(12.0f), std::sin(glm::radians(60.0f)), 1e-3f);
    CHECK_NEAR(height(6.0f), 0.0f, 1e-3f);
    CHECK_NEAR(height(18.0f), 0.0f, 1e-3f);
    CHECK_TRUE(height(0.0f) < -0.5f);

    // Ход времени: сутки за 1 минуту — за 15 секунд проходит 6 часов, по кругу.
    c.Time = 22.0f;
    c.DayLengthMinutes = 1.0f;
    c.Speed = 1.0f;
    sage::ecs::AdvanceDayNight(c, 15.0f);
    CHECK_NEAR(c.Time, 4.0f, 1e-3f);
    c.Speed = 0.0f;
    sage::ecs::AdvanceDayNight(c, 15.0f);
    CHECK_NEAR(c.Time, 4.0f, 1e-3f);
}

// Цикл ставит ОБЪЕКТ-солнце (светит в кадре он) и идёт в игре через систему
// кадра; выключенный цикл солнце не трогает.
TEST(Environment_cycle_drives_the_sun_object_and_runs_in_play) {
    Scene scene("s");
    GameObject sun = sage::ecs::CreateSunEntity(scene);
    const glm::vec3 before = sun.GetTransform().Rotation;
    CHECK_FALSE(sage::ecs::ApplyDayNight(scene));
    CHECK_TRUE(sun.GetTransform().Rotation == before);

    scene.Lighting.Cycle.Enabled = true;
    scene.Lighting.Cycle.Time = 12.0f;
    scene.Lighting.Cycle.DayLengthMinutes = 1.0f;
    CHECK_TRUE(sage::ecs::ApplyDayNight(scene));
    LightingEnvironment frame = sage::ecs::CollectLighting(scene);
    CHECK_TRUE(frame.Sun.Direction.y < -0.8f);   // полдень: светит сверху вниз

    sage::SystemScheduler sched;
    sage::CoreSystems core;
    core.Animation = false;
    core.DayNight = true;
    sage::RegisterCoreSystems(sched, core);
    sched.Run(scene, 7.5f);   // сутки за минуту: +3 часа
    CHECK_NEAR(scene.Lighting.Cycle.Time, 15.0f, 1e-2f);
    frame = sage::ecs::CollectLighting(scene);
    CHECK_TRUE(frame.Sun.Direction.y < -0.3f && frame.Sun.Direction.y > -0.8f);
}

// Сохранение: цикл и тип неба переживают запись; сцена без цикла (старая)
// открывается с выключенным циклом.
TEST(Environment_settings_survive_save_and_load) {
    Scene scene("s");
    LightingEnvironment& e = scene.Lighting;
    e.Skybox.Enabled = true;
    e.Skybox.Kind = Src::Faces;
    e.Skybox.FacePaths[3] = "assets/sky/down.png";
    e.Skybox.RotationDeg = 45.0f;
    e.Cycle.Enabled = true;
    e.Cycle.Time = 19.25f;
    e.Cycle.Speed = 2.0f;
    e.Cycle.DayLengthMinutes = 24.0f;
    e.Cycle.RunInPlay = false;
    e.Cycle.SunAzimuth = 30.0f;
    e.Cycle.NoonElevation = 45.0f;
    e.Fog.Enabled = true;
    e.Fog.Kind = FogSettings::Mode::ExponentialHeight;
    e.Fog.SunExponent = 12.0f;
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    const LightingEnvironment& b = back->Lighting;
    CHECK_TRUE(b.Skybox.Kind == Src::Faces);
    CHECK_EQ(b.Skybox.FacePaths[3], std::string("assets/sky/down.png"));
    CHECK_NEAR(b.Skybox.RotationDeg, 45.0f, 1e-4f);
    CHECK_TRUE(b.Cycle.Enabled);
    CHECK_NEAR(b.Cycle.Time, 19.25f, 1e-4f);
    CHECK_NEAR(b.Cycle.Speed, 2.0f, 1e-4f);
    CHECK_NEAR(b.Cycle.DayLengthMinutes, 24.0f, 1e-4f);
    CHECK_FALSE(b.Cycle.RunInPlay);
    CHECK_NEAR(b.Cycle.SunAzimuth, 30.0f, 1e-4f);
    CHECK_NEAR(b.Cycle.NoonElevation, 45.0f, 1e-4f);
    CHECK_TRUE(b.Fog.Kind == FogSettings::Mode::ExponentialHeight);
    CHECK_NEAR(b.Fog.SunExponent, 12.0f, 1e-4f);

    // Старая сцена: ключа цикла нет — он выключен, остальное как было.
    std::string old = SceneSerializer::SaveToString(scene);
    const size_t at = old.find("\"dayNightCycle\"");
    CHECK_TRUE(at != std::string::npos);
    nlohmann::json j = nlohmann::json::parse(old);
    std::function<void(nlohmann::json&)> strip = [&](nlohmann::json& n) {
        if (n.is_object()) {
            n.erase("dayNightCycle");
            for (auto& [k, v] : n.items()) strip(v);
        } else if (n.is_array()) {
            for (auto& v : n) strip(v);
        }
    };
    strip(j);
    std::unique_ptr<Scene> legacy = SceneSerializer::LoadFromString(j.dump());
    CHECK_TRUE(legacy != nullptr);
    if (legacy) {
        CHECK_FALSE(legacy->Lighting.Cycle.Enabled);
        CHECK_TRUE(legacy->Lighting.Skybox.Kind == Src::Faces);
    }
}

TEST(Environment_sky_type_keys_round_trip) {
    for (Src s : {Src::Procedural, Src::Solid, Src::Image, Src::Cubemap, Src::Faces}) {
        Src back = Src::Procedural;
        CHECK_TRUE(env::SkySourceFromKey(env::SkySourceKey(s), back));
        CHECK_TRUE(back == s);
    }
    Src untouched = Src::Image;
    CHECK_FALSE(env::SkySourceFromKey("nonsense", untouched));
    CHECK_TRUE(untouched == Src::Image);
}

// «Папка с кубической картой» из выбора убрана: её место занимают шесть
// отдельных файлов с подписью у каждой грани. Сцена со старым типом по-прежнему
// открывается (тип находится), но предложить его заново нельзя.
TEST(Environment_cubemap_folder_is_not_offered_but_old_scenes_open) {
    const env::System* sky = env::FindSystem("sky");
    CHECK_TRUE(sky != nullptr);
    if (!sky) return;
    int offered = 0;
    for (const env::Variant& v : sky->Variants) {
        if (v.Legacy) continue;
        ++offered;
        CHECK_TRUE(v.Key != "cubemap");
    }
    CHECK_EQ(offered, 4);
    LightingEnvironment e;
    e.Skybox.Enabled = true;
    e.Skybox.Kind = Src::Cubemap;
    const env::Variant* cur = env::CurrentVariant(*sky, e);
    CHECK_TRUE(cur != nullptr && cur->Legacy);
    CHECK_TRUE(Active("sky.folder.convert", e));
}

// У каждой из шести граней — своя подсказка «что видно в эту сторону», и
// над слотами — развёртка куба, показывающая, где какая грань.
TEST(Environment_six_faces_each_have_a_hint_and_a_map) {
    for (const char* k : {"sky.face.px", "sky.face.nx", "sky.face.py", "sky.face.ny", "sky.face.pz", "sky.face.nz"}) {
        const env::Prop* p = env::FindProp(k);
        CHECK_TRUE(p != nullptr && !p->Hint.empty());
    }
    LightingEnvironment e;
    e.Skybox.Enabled = true;
    e.Skybox.Kind = Src::Faces;
    CHECK_TRUE(Active("sky.faces.map", e));
    e.Skybox.Kind = Src::Image;
    CHECK_FALSE(Active("sky.faces.map", e));
}

// Перевод старого неба: грани px/nx/py/ny/pz/nz папки — в свои слоты по
// порядку +X, -X, +Y, -Y, +Z, -Z; не хватает хоть одной — небо не трогается.
TEST(Environment_cubemap_folder_converts_to_six_files) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "sage_env_cube_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "sky", ec);
    const char* names[6] = {"px.png", "nx.jpg", "py.png", "ny.png", "pz.png", "nz.png"};
    for (int i = 0; i < 5; ++i) std::ofstream(root / "assets" / "sky" / names[i]) << "x";

    SkyboxSettings sky;
    sky.Kind = Src::Cubemap;
    sky.CubemapDir = "assets/sky";
    CHECK_FALSE(env::CubemapFolderToFaces(sky, root));   // нет nz
    CHECK_TRUE(sky.Kind == Src::Cubemap);
    CHECK_TRUE(sky.FacePaths[0].empty());

    std::ofstream(root / "assets" / "sky" / names[5]) << "x";
    CHECK_TRUE(env::CubemapFolderToFaces(sky, root));
    CHECK_TRUE(sky.Kind == Src::Faces);
    CHECK_EQ(sky.FacePaths[0], std::string("assets/sky/px.png"));
    CHECK_EQ(sky.FacePaths[1], std::string("assets/sky/nx.jpg"));
    CHECK_EQ(sky.FacePaths[5], std::string("assets/sky/nz.png"));
    CHECK_TRUE(sky.HasFaces());
    fs::remove_all(root, ec);
}
