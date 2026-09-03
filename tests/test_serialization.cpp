// Модульные тесты сериализации: EngineConfig (JSON-файл), Material (.sagemat) и
// Scene (SceneSerializer). Round-trip save→load сверяет, что данные переживают
// цикл без потерь. Сцена собирается из сущностей БЕЗ GPU-мешей
// (MeshRef::Type::None), поэтому загрузка не трогает графический бэкенд.
#include "TestFramework.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "sage/core/Config.h"
#include "sage/render/Material.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/ui/UI.h"

namespace fs = std::filesystem;

// Уникальный временный путь (в системном tmp) — чтобы тесты не мусорили в репо.
static std::string TempPath(const std::string& name) {
    return (fs::temp_directory_path() / ("sage_test_" + name)).string();
}

// --- EngineConfig round-trip -----------------------------------------------

// Параметры кинематографических эффектов (глубина резкости, смаз движения,
// хроматическая аберрация) должны переживать сохранение конфига: иначе
// выставленная в редакторе картинка молча возвращалась бы к дефолтам при
// каждом перезапуске.
TEST(Config_cinematic_effects_roundtrip) {
    sage::EngineConfig out;
    out.DepthOfField = true;
    out.FocusDistance = 7.25f;
    out.Aperture = 1.4f;
    out.DofMaxRadius = 20.0f;
    out.MotionBlur = true;
    out.MotionBlurAmount = 0.75f;
    out.MotionBlurSamples = 20;
    out.ChromaticAberration = 0.45f;

    std::string path = TempPath("config_fx.json");
    CHECK_TRUE(out.SaveFile(path));

    sage::EngineConfig in;
    CHECK_TRUE(in.LoadFile(path));
    CHECK_TRUE(in.DepthOfField);
    CHECK_NEAR(in.FocusDistance, 7.25f, 1e-5);
    CHECK_NEAR(in.Aperture, 1.4f, 1e-5);
    CHECK_NEAR(in.DofMaxRadius, 20.0f, 1e-5);
    CHECK_TRUE(in.MotionBlur);
    CHECK_NEAR(in.MotionBlurAmount, 0.75f, 1e-5);
    CHECK_EQ(in.MotionBlurSamples, 20);
    CHECK_NEAR(in.ChromaticAberration, 0.45f, 1e-5);

    std::remove(path.c_str());
}

// Конфиг без раздела эффектов (файл от прежней версии движка) должен грузиться
// с безопасными дефолтами, а не включать тяжёлые проходы сам по себе.
TEST(Config_effects_default_off_for_old_files) {
    sage::EngineConfig cfg;
    CHECK_FALSE(cfg.DepthOfField);
    CHECK_FALSE(cfg.MotionBlur);
    CHECK_NEAR(cfg.ChromaticAberration, 0.0f, 1e-6);
}

TEST(Config_save_load_roundtrip) {
    sage::EngineConfig out;
    out.Width = 1920;
    out.Height = 1080;
    out.Title = "RoundTrip";
    out.Mode = sage::WindowMode::Borderless;
    out.VSync = false;
    out.Msaa = 4;
    out.Aspect = sage::AspectMode::R21x9;
    out.RenderScale = 1.5f;
    out.Shadows = false;
    out.ShadowResolution = 1024;
    out.PostProcessing = false;
    out.Exposure = 1.3f;
    out.Gamma = 2.4f;

    std::string path = TempPath("config.json");
    CHECK_TRUE(out.SaveFile(path));

    sage::EngineConfig in;
    CHECK_TRUE(in.LoadFile(path));
    CHECK_EQ(in.Width, 1920);
    CHECK_EQ(in.Height, 1080);
    CHECK_EQ(in.Title, std::string("RoundTrip"));
    CHECK_TRUE(in.Mode == sage::WindowMode::Borderless);
    CHECK_FALSE(in.VSync);
    CHECK_EQ(in.Msaa, 4);
    CHECK_TRUE(in.Aspect == sage::AspectMode::R21x9);
    CHECK_NEAR(in.RenderScale, 1.5f, 1e-5);
    CHECK_FALSE(in.Shadows);
    CHECK_EQ(in.ShadowResolution, 1024);
    CHECK_FALSE(in.PostProcessing);
    CHECK_NEAR(in.Exposure, 1.3f, 1e-5);
    CHECK_NEAR(in.Gamma, 2.4f, 1e-5);

    fs::remove(path);
}

TEST(Config_load_missing_file_returns_false) {
    sage::EngineConfig cfg;
    CHECK_FALSE(cfg.LoadFile(TempPath("no_such_config_zzz.json")));
    // Значения не должны затираться при неудачной загрузке.
    CHECK_EQ(cfg.Width, 1280);
    CHECK_EQ(cfg.Height, 720);
}

// --- Material round-trip ----------------------------------------------------

TEST(Material_save_load_roundtrip) {
    Material out;
    out.Albedo = {0.2f, 0.4f, 0.6f};
    out.Emissive = {0.1f, 0.0f, 0.05f};
    out.Metallic = 0.75f;
    out.Roughness = 0.3f;
    out.TexturePath = "textures/brick_albedo.png";
    out.NormalMapPath = "textures/brick_normal.png";
    out.MetallicMapPath = "textures/brick_metallic.png";
    out.RoughnessMapPath = "textures/brick_roughness.png";
    out.AOMapPath = "textures/brick_ao.png";
    out.Shininess = 64.0f;

    std::string path = TempPath("mat.sagemat");
    out.SaveToFile(path);

    Material in = Material::LoadFromFile(path);
    CHECK_NEAR(in.Albedo.r, 0.2f, 1e-5);
    CHECK_NEAR(in.Albedo.g, 0.4f, 1e-5);
    CHECK_NEAR(in.Albedo.b, 0.6f, 1e-5);
    CHECK_NEAR(in.Emissive.r, 0.1f, 1e-5);
    CHECK_NEAR(in.Metallic, 0.75f, 1e-5);
    CHECK_NEAR(in.Roughness, 0.3f, 1e-5);
    CHECK_EQ(in.TexturePath, std::string("textures/brick_albedo.png"));
    CHECK_EQ(in.NormalMapPath, std::string("textures/brick_normal.png"));
    CHECK_EQ(in.MetallicMapPath, std::string("textures/brick_metallic.png"));
    CHECK_EQ(in.RoughnessMapPath, std::string("textures/brick_roughness.png"));
    CHECK_EQ(in.AOMapPath, std::string("textures/brick_ao.png"));
    CHECK_NEAR(in.Shininess, 64.0f, 1e-5);

    fs::remove(path);
}

TEST(Material_load_missing_file_throws) {
    bool threw = false;
    try {
        Material::LoadFromFile(TempPath("no_such_material_zzz.sagemat"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
}

// --- Scene round-trip (без GPU-мешей) --------------------------------------

// Ищет сущность по имени в загруженной сцене (иначе null-handle).
static GameObject FindByName(Scene& scene, const std::string& name) {
    auto& reg = scene.Registry();
    for (auto e : reg.view<NameComponent>()) {
        if (reg.get<NameComponent>(e).Name == name)
            return GameObject(&reg, e);
    }
    return GameObject();
}

TEST(Scene_string_roundtrip_entities_and_transforms) {
    Scene scene("TestScene");

    GameObject a = scene.CreateObject("Alpha");
    a.GetTransform().Position = {1.0f, 2.0f, 3.0f};
    a.GetTransform().Rotation = {0.0f, 45.0f, 0.0f};
    a.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    scene.Registry().emplace<ScriptComponent>(a.Entity(), ScriptComponent{"scripts/spin.lua"});

    GameObject b = scene.CreateObject("Beta");
    b.GetTransform().Position = {-5.0f, 0.0f, 0.5f};

    // Освещение.
    scene.Lighting.Sun.Direction = {-0.5f, -1.0f, -0.25f};
    scene.Lighting.Sun.Intensity = 0.8f;
    scene.Lighting.SkyColor = {0.4f, 0.5f, 0.9f};
    scene.Lighting.Fog.Enabled = true;
    scene.Lighting.Fog.Start = 10.0f;
    scene.Lighting.Fog.End = 55.0f;
    PointLight pl;
    pl.Position = {3.0f, 2.0f, 1.0f};
    pl.Intensity = 1.7f;
    scene.Lighting.PointLights.push_back(pl);

    std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);

    CHECK_EQ(loaded->Name(), std::string("TestScene"));

    GameObject la = FindByName(*loaded, "Alpha");
    CHECK_TRUE(la.Valid());
    CHECK_NEAR(la.GetTransform().Position.x, 1.0f, 1e-4);
    CHECK_NEAR(la.GetTransform().Position.y, 2.0f, 1e-4);
    CHECK_NEAR(la.GetTransform().Position.z, 3.0f, 1e-4);
    CHECK_NEAR(la.GetTransform().Rotation.y, 45.0f, 1e-4);
    CHECK_NEAR(la.GetTransform().Scale.x, 2.0f, 1e-4);
    // Скрипт пережил round-trip.
    const ScriptComponent* sc = loaded->Registry().try_get<ScriptComponent>(la.Entity());
    CHECK_TRUE(sc != nullptr);
    if (sc) CHECK_EQ(sc->Path, std::string("scripts/spin.lua"));

    GameObject lb = FindByName(*loaded, "Beta");
    CHECK_TRUE(lb.Valid());
    CHECK_NEAR(lb.GetTransform().Position.x, -5.0f, 1e-4);

    // Освещение пережило round-trip.
    CHECK_NEAR(loaded->Lighting.Sun.Intensity, 0.8f, 1e-4);
    CHECK_NEAR(loaded->Lighting.Sun.Direction.x, -0.5f, 1e-4);
    CHECK_NEAR(loaded->Lighting.SkyColor.b, 0.9f, 1e-4);
    CHECK_TRUE(loaded->Lighting.Fog.Enabled);
    CHECK_NEAR(loaded->Lighting.Fog.End, 55.0f, 1e-4);
    CHECK_EQ((int)loaded->Lighting.PointLights.size(), 1);
    if (!loaded->Lighting.PointLights.empty())
        CHECK_NEAR(loaded->Lighting.PointLights[0].Intensity, 1.7f, 1e-4);
}

TEST(Scene_roundtrip_preserves_parent_hierarchy) {
    Scene scene("HierScene");
    GameObject parent = scene.CreateObject("Parent");
    GameObject child = scene.CreateObject("Child");
    scene.SetParent(child.Entity(), parent.Entity());

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    GameObject lp = FindByName(*loaded, "Parent");
    GameObject lc = FindByName(*loaded, "Child");
    CHECK_TRUE(lp.Valid());
    CHECK_TRUE(lc.Valid());
    // Ребёнок после загрузки снова имеет родителя Parent.
    CHECK_TRUE(loaded->ParentOf(lc.Entity()) == lp.Entity());
}

TEST(Scene_json_encodes_primitive_mesh_ref) {
    // Проверка ЗАПИСИ (без загрузки — примитив на загрузке создал бы GPU-меш):
    // тип примитива корректно попадает в JSON.
    Scene scene("MeshRefScene");
    GameObject c = scene.CreateObject("Boxy");
    c.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};

    std::string text = SceneSerializer::SaveToString(scene);
    nlohmann::json j = nlohmann::json::parse(text);

    bool foundCube = false;
    for (const auto& ent : j.value("objects", nlohmann::json::array())) {
        if (ent.value("name", "") == "Boxy") {
            foundCube = ent.contains("mesh") && ent["mesh"].value("type", "") == "cube";
        }
    }
    CHECK_TRUE(foundCube);
}

// ===========================================================================
//  Прозрачность и оформление интерфейса переживают сохранение
// ===========================================================================
TEST(Serialization_opacity_and_ui_style_round_trip) {
    Scene scene("Style");
    GameObject glass = scene.CreateObject("Glass");
    // Тип меша оставляем None: тесты идут без GL-контекста, а любой примитив
    // при загрузке попросил бы у ResourceManager настоящий GPU-меш.
    glass.Renderer().Opacity = 0.35f;

    GameObject hud = scene.CreateObject("Bar");
    entt::registry& reg = scene.Registry();
    reg.emplace<sage::ui::Transform>(hud.Entity(), sage::ui::Transform{});
    sage::ui::Fill fill;
    fill.Gradient = {0.1f, 0.1f, 0.2f, 0.8f};
    fill.ShadowSize = 12.0f;
    reg.emplace<sage::ui::Fill>(hud.Entity(), fill);
    sage::ui::Icon icon;
    icon.Name = "heart";
    icon.Color = {0.9f, 0.2f, 0.2f, 1.0f};
    reg.emplace<sage::ui::Icon>(hud.Entity(), icon);
    reg.emplace<sage::ui::Bar>(hud.Entity(), sage::ui::Bar{});

    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    GameObject g2 = back->FindByName("Glass");
    CHECK_NEAR(g2.Renderer().Opacity, 0.35f, 1e-4);

    entt::registry& r2 = back->Registry();
    const entt::entity e2 = back->FindByName("Bar").Entity();
    const auto* icon2 = r2.try_get<sage::ui::Icon>(e2);
    const auto* fill2 = r2.try_get<sage::ui::Fill>(e2);
    CHECK_TRUE(icon2 != nullptr);
    CHECK_TRUE(fill2 != nullptr);
    if (icon2) {
        CHECK_EQ(icon2->Name, std::string("heart"));
        CHECK_NEAR(icon2->Color.r, 0.9f, 1e-4);
    }
    if (fill2) {
        CHECK_NEAR(fill2->Gradient.a, 0.8f, 1e-4);
        CHECK_NEAR(fill2->ShadowSize, 12.0f, 1e-4);
    }
    CHECK_TRUE(r2.all_of<sage::ui::Bar>(e2));
}

// Материал: своя программа и пользовательские юниформы — тоже данные проекта.
TEST(Serialization_material_custom_shader_round_trip) {
    Material m;
    m.Albedo = {0.2f, 0.4f, 0.9f};
    m.Opacity = 0.5f;
    m.VertexShaderPath = "assets/shaders/water.vert";
    m.FragmentShaderPath = "assets/shaders/water.frag";
    m.Params["uWaveHeight"] = ShaderParam::Make(0.35f);
    m.Params["uFoam"] = ShaderParam::Make(glm::vec3(0.8f, 0.9f, 1.0f));

    const std::string path = "sage_test_material.sagemat";
    m.SaveToFile(path);
    Material back = Material::LoadFromFile(path);
    std::remove(path.c_str());

    CHECK_NEAR(back.Opacity, 0.5f, 1e-4);
    CHECK_TRUE(back.HasCustomShader());
    CHECK_EQ(back.FragmentShaderPath, std::string("assets/shaders/water.frag"));
    CHECK_EQ((int)back.Params.size(), 2);
    CHECK_TRUE(back.Params["uWaveHeight"].Kind == ShaderParam::Type::Float);
    CHECK_NEAR(back.Params["uWaveHeight"].Value.x, 0.35f, 1e-4);
    CHECK_TRUE(back.Params["uFoam"].Kind == ShaderParam::Type::Vec3);
    CHECK_NEAR(back.Params["uFoam"].Value.z, 1.0f, 1e-4);
}

// --- Версия формата сцены и миграции ------------------------------------------
//
// Номер версии писался в файл с самого начала и НИКОГДА не читался. Это худший
// вариант: он создаёт впечатление, что о совместимости позаботились, а первое
// же ломающее изменение формата тихо испортило бы все старые сцены — без
// ошибки, просто «объекты почему-то не там».

#include <nlohmann/json.hpp>

TEST(Scene_migration_upgrades_an_old_file) {
    // Сцена «версии 1»: тела без слоя и без признака сенсора — этих полей в
    // первом формате не было вовсе.
    const std::string oldScene = R"({
      "name": "Old",
      "sage_scene_version": 1,
      "objects": [
        {"id": 1, "name": "Box",
         "position": {"x": 0, "y": 2, "z": 0},
         "rotation": {"x": 0, "y": 0, "z": 0},
         "scale": {"x": 1, "y": 1, "z": 1},
         "mesh": {"type": "cube", "path": ""},
         "rigidBody": {"type": "dynamic", "mass": 2.0}}
      ]
    })";

    const std::string migrated = SceneSerializer::MigrateSceneJson(oldScene);
    const nlohmann::json j = nlohmann::json::parse(migrated);

    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());
    const nlohmann::json& rb = j["objects"][0]["rigidBody"];
    CHECK_TRUE(rb.contains("layer"));
    CHECK_TRUE(rb.contains("sensor"));
    CHECK_EQ(rb["layer"].get<unsigned>(), 1u);
    CHECK_FALSE(rb["sensor"].get<bool>());
    // Что было — обязано уцелеть: миграция ДОБАВЛЯЕТ, а не переписывает.
    CHECK_NEAR(rb["mass"].get<float>(), 2.0f, 1e-6);
    CHECK_TRUE(j["objects"][0]["name"] == "Box");
}

TEST(Scene_without_version_is_treated_as_the_first_one) {
    // Сцены, сохранённые до появления проверки, номера не несут. Их не за что
    // винить, и загружаться они обязаны.
    const std::string noVersion = R"({"name":"NoVer","objects":[]})";
    const std::string migrated = SceneSerializer::MigrateSceneJson(noVersion);
    const nlohmann::json j = nlohmann::json::parse(migrated);
    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());
}

TEST(Scene_from_the_future_is_refused_not_half_read) {
    // Файл новее движка читать нельзя: половина полей ему незнакома, и молча
    // потерять их хуже, чем отказаться. Отказ — единственный честный ответ.
    const std::string future =
        R"({"name":"Future","sage_scene_version":9999,"objects":[]})";
    bool threw = false;
    try {
        SceneSerializer::MigrateSceneJson(future);
    } catch (const std::exception& e) {
        threw = true;
        std::printf("       отказ: %s\n", e.what());
    }
    CHECK_TRUE(threw);
}

TEST(Scene_migration_is_idempotent) {
    // Повторная миграция уже актуального файла не должна ничего менять: иначе
    // сохранение-загрузка по кругу дрейфовали бы, а это самый неприятный вид
    // порчи данных — медленный и незаметный.
    Scene scene("Round");
    GameObject o = scene.CreateObject("Body");
    scene.Registry().emplace<RigidBodyComponent>(o.Entity());
    const std::string saved = SceneSerializer::SaveToString(scene);

    const std::string once = SceneSerializer::MigrateSceneJson(saved);
    const std::string twice = SceneSerializer::MigrateSceneJson(once);
    CHECK_TRUE(once == twice);
}

TEST(Scene_saved_today_declares_the_current_version) {
    Scene scene("Fresh");
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::SaveToString(scene));
    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());
    // Версия обязана быть больше единицы: иначе «текущая» и «первая» совпадают,
    // и цепочка миграций не проверяется вовсе.
    CHECK_TRUE(SceneSerializer::CurrentVersion() >= 2);
}

// --- Сцена переживает переименование ассета -----------------------------------
//
// Главная проверка всей затеи с GUID'ами. Раньше это и было симптомом:
// переименовал файл — сцена загрузилась, объект остался на месте, просто без
// модели, и ни строчки в логе.

#include "sage/assets/AssetDatabase.h"
#include <filesystem>
#include <fstream>

TEST(Scene_reference_survives_renaming_the_asset_file) {
    namespace afs = std::filesystem;
    const afs::path dir = afs::temp_directory_path() / "sage_scene_rename";
    afs::remove_all(dir);
    afs::create_directories(dir / "assets" / "models");
    std::ofstream(dir / "assets/models/hero.glb") << "fake";

    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    db.Clear();
    db.ScanProject(dir.string());
    const sage::AssetGuid guid = db.GuidOf("assets/models/hero.glb");
    CHECK_TRUE(guid.Valid());

    // Сцена ссылается на модель — сохраняем её вместе с GUID'ом.
    Scene scene("RenameScene");
    GameObject o = scene.CreateObject("Hero");
    o.Renderer().Ref.type = MeshRef::Type::Model;
    o.Renderer().Ref.path = "assets/models/hero.glb";
    const std::string saved = SceneSerializer::SaveToString(scene);
    CHECK_TRUE(saved.find(guid.ToString()) != std::string::npos);  // GUID реально записан

    // Переименовываем файл вместе с сайдкаром — так это делают проводник и git.
    afs::rename(dir / "assets/models/hero.glb", dir / "assets/models/protagonist.glb");
    afs::rename(dir / "assets/models/hero.glb.meta", dir / "assets/models/protagonist.glb.meta");
    db.ScanProject(dir.string());

    // Загружаем ТУ ЖЕ сцену: она помнит старый путь, но ссылка обязана
    // разрешиться в новый.
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(saved);
    GameObject hero = loaded->FindByName("Hero");
    CHECK_TRUE(hero.Valid());
    std::printf("       сцена помнила assets/models/hero.glb, получила %s\n",
                hero.Renderer().Ref.path.c_str());
    CHECK_TRUE(hero.Renderer().Ref.path == "assets/models/protagonist.glb");

    db.Clear();
    std::error_code ec;
    afs::remove_all(dir, ec);
}

TEST(Scene_migration_v2_to_v3_stamps_guids_onto_old_references) {
    namespace afs = std::filesystem;
    const afs::path dir = afs::temp_directory_path() / "sage_scene_v3";
    afs::remove_all(dir);
    afs::create_directories(dir / "assets" / "models");
    std::ofstream(dir / "assets/models/box.glb") << "fake";

    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    db.Clear();
    db.ScanProject(dir.string());
    const sage::AssetGuid guid = db.GuidOf("assets/models/box.glb");

    // Сцена формата v2: ссылка только путём, GUID'а нет.
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 2,
      "objects": [{"id": 1, "name": "Box",
        "position": {"x":0,"y":0,"z":0}, "rotation": {"x":0,"y":0,"z":0},
        "scale": {"x":1,"y":1,"z":1},
        "mesh": {"type": "model", "path": "assets/models/box.glb"}}]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());
    CHECK_TRUE(j["objects"][0]["mesh"].contains("pathGuid"));
    CHECK_TRUE(j["objects"][0]["mesh"]["pathGuid"].get<std::string>() == guid.ToString());

    db.Clear();
    std::error_code ec;
    afs::remove_all(dir, ec);
}

TEST(Scene_migration_leaves_a_missing_asset_visibly_broken) {
    // Путь, которого в базе нет, не должен получать выдуманный GUID: сломанная
    // ссылка обязана остаться видимо сломанной, а не притвориться целой.
    sage::AssetDatabase::Instance().Clear();
    const std::string old = R"({
      "name":"Broken","sage_scene_version":2,
      "objects":[{"id":1,"name":"Ghost",
        "position":{"x":0,"y":0,"z":0},"rotation":{"x":0,"y":0,"z":0},
        "scale":{"x":1,"y":1,"z":1},
        "mesh":{"type":"model","path":"assets/models/nope.glb"}}]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_FALSE(j["objects"][0]["mesh"].contains("pathGuid"));
    // Путь при этом сохраняется — по нему человек поймёт, чего не хватает.
    CHECK_TRUE(j["objects"][0]["mesh"]["path"].get<std::string>() == "assets/models/nope.glb");
}

// --- Свойства рендера материала не могут разойтись между чтением и записью ----
//
// Material раньше рос полем на фичу, и дорого было не поле, а его хвост: правку
// приходилось повторять в пяти местах, из которых ДВА — ручные списки чтения и
// записи .sagemat. Такие списки обязаны совпадать и рано или поздно расходятся:
// свойство сохраняется, но не читается, и молча сбрасывается при следующей
// загрузке. Теперь оба ходят в одну таблицу, и тест закрепляет именно это —
// проходя по таблице, а не по заранее известным именам. Свойство, добавленное
// завтра, проверится само.
TEST(Material_render_properties_survive_a_round_trip_by_table) {
    const std::vector<MaterialRenderField>& fields = MaterialRenderFields();
    CHECK_TRUE(!fields.empty());

    Material out;
    // Ставим каждому свойству значение, ОТЛИЧНОЕ от значения по умолчанию:
    // совпадающее с умолчанием прошло бы тест даже при полностью потерянном
    // поле, и проверка была бы декоративной.
    const Material defaults;
    for (const MaterialRenderField& f : fields) {
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
            out.Render.*f.AsBool = !(defaults.Render.*f.AsBool);
        } else if (f.AsFloat) {
            const float def = defaults.Render.*f.AsFloat;
            out.Render.*f.AsFloat = (def == f.Max) ? f.Min : f.Max;
        }
    }

    const std::string path =
        (fs::temp_directory_path() / "sage_material_render.sagemat").string();
    out.SaveToFile(path);
    const Material in = Material::LoadFromFile(path);
    fs::remove(path);

    for (const MaterialRenderField& f : fields) {
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
            CHECK_TRUE(in.Render.*f.AsBool == out.Render.*f.AsBool);
        } else if (f.AsFloat) {
            CHECK_NEAR(in.Render.*f.AsFloat, out.Render.*f.AsFloat, 1e-5);
        }
        // Каждое свойство обязано иметь ключ, подпись и одно (ровно одно) поле.
        CHECK_TRUE(f.Key != nullptr && f.Label != nullptr);
        CHECK_TRUE((f.AsBool != nullptr) != (f.AsFloat != nullptr));
    }
}

// Старые .sagemat, написанные до разделения на поверхность и поведение, обязаны
// читаться как были: ключи остались в КОРНЕ файла и не уехали во вложенный
// объект. Перекладывание ключей ради красоты структуры — ровно та ломающая
// правка формата, от которой заведены версии.
TEST(Material_reads_files_written_before_the_split) {
    const std::string path =
        (fs::temp_directory_path() / "sage_material_legacy.sagemat").string();
    {
        std::ofstream f(path);
        f << R"({"albedo":[0.5,0.25,0.125],"metallic":0.75,"roughness":0.2,)"
          << R"("doubleSided":false,"planarReflectivity":0.9})";
    }
    const Material m = Material::LoadFromFile(path);
    fs::remove(path);

    CHECK_NEAR(m.Albedo.r, 0.5f, 1e-5);
    CHECK_NEAR(m.Metallic, 0.75f, 1e-5);
    CHECK_FALSE(m.Render.DoubleSided);
    CHECK_NEAR(m.Render.PlanarReflectivity, 0.9f, 1e-5);
}

// --- v3 -> v4: цвет экземпляра стал множителем albedo, а не заменой ----------
//
// Правило наложения поменялось, и без миграции ЛЮБАЯ уже сделанная сцена
// перекрасилась бы при первом открытии: мёртвый до сих пор color домножил бы
// albedo материала. Проверяем обе стороны — у сущности с материалом он
// обнуляется в нейтральный белый, у сущности без материала остаётся как был
// (там color и был цветом объекта).
TEST(Scene_migration_v3_to_v4_neutralises_a_dead_instance_colour) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 3,
      "objects": [
        {"id": 1, "name": "Painted",
         "position": {"x":0,"y":0,"z":0}, "rotation": {"x":0,"y":0,"z":0},
         "scale": {"x":1,"y":1,"z":1},
         "color": [1.0, 0.05, 0.05],
         "material": "assets/red.sagemat"},
        {"id": 2, "name": "Plain",
         "position": {"x":0,"y":0,"z":0}, "rotation": {"x":0,"y":0,"z":0},
         "scale": {"x":1,"y":1,"z":1},
         "color": [1.0, 0.05, 0.05]}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());

    // С материалом: цвет был мёртвым, стал нейтральным — вид сцены не изменился.
    const nlohmann::json& painted = j["objects"][0]["color"];
    CHECK_NEAR(painted[0].get<float>(), 1.0f, 1e-5f);
    CHECK_NEAR(painted[1].get<float>(), 1.0f, 1e-5f);
    CHECK_NEAR(painted[2].get<float>(), 1.0f, 1e-5f);

    // Без материала цвет трогать нельзя — он и был цветом объекта.
    const nlohmann::json& plain = j["objects"][1]["color"];
    CHECK_NEAR(plain[1].get<float>(), 0.05f, 1e-5f);
}

// А это уже само правило: поправка МОДУЛИРУЕТ материал, одинаково у всех трёх
// величин. Раньше правил было три разных, и цвет из них молчал громче всех.
TEST(MeshRenderer_instance_overrides_modulate_the_material) {
    MeshRendererComponent mr;
    auto mat = std::make_shared<Material>();
    mat->Albedo = {0.5f, 0.5f, 0.5f};
    mat->Opacity = 0.8f;
    mat->Emissive = {0.1f, 0.0f, 0.0f};
    mat->EmissiveStrength = 1.0f;
    mr.MaterialPtr = mat;

    // Нейтральные поправки = «как в материале»: включение группы само по себе
    // ничего не меняет.
    CHECK_NEAR(EffectiveColor(mr).r, 0.5f, 1e-5f);
    CHECK_NEAR(EffectiveOpacity(mr), 0.8f, 1e-5f);
    CHECK_NEAR(EffectiveEmissive(mr).r, 0.1f, 1e-5f);

    mr.Color = {1.0f, 0.5f, 0.0f};
    mr.Opacity = 0.5f;
    mr.Emissive = {0.2f, 0.0f, 0.0f};
    CHECK_NEAR(EffectiveColor(mr).r, 0.5f, 1e-5f);    // 0.5 * 1.0
    CHECK_NEAR(EffectiveColor(mr).g, 0.25f, 1e-5f);   // 0.5 * 0.5
    CHECK_NEAR(EffectiveOpacity(mr), 0.4f, 1e-5f);    // 0.8 * 0.5
    CHECK_NEAR(EffectiveEmissive(mr).r, 0.3f, 1e-5f); // 0.1 + 0.2

    // Без материала поправка задаёт вид целиком — как и раньше.
    mr.MaterialPtr = nullptr;
    CHECK_NEAR(EffectiveColor(mr).g, 0.5f, 1e-5f);
    CHECK_NEAR(EffectiveOpacity(mr), 0.5f, 1e-5f);
}

// --- Материалы частей модели -------------------------------------------------

// Слоты подмешей обязаны переживать сохранение сцены: без этого «покрасил
// куртку персонажа» жило бы ровно до перезагрузки, и разбираться пришлось бы не
// с рендером, а с файлом сцены.
TEST(MeshRenderer_submesh_material_slots_survive_the_scene_file) {
    Scene scene("slots");
    GameObject obj = scene.CreateObject("Character");
    MeshRendererComponent& mr = obj.Renderer();
    mr.MaterialPath = "materials/skin.sagemat";
    mr.Slots.resize(3);
    mr.Slots[0].Path = "materials/jacket.sagemat";
    // Слот 1 намеренно ПУСТ: он означает «красится материалом объекта», и
    // сжать его при сохранении нельзя — слоты адресуются номером части, и
    // пропуск сдвинул бы все последующие материалы на одну часть модели.
    mr.Slots[2].Path = "materials/eyes.sagemat";

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);

    GameObject loaded = back->Get(obj.Id());
    CHECK_TRUE(loaded.Valid());
    const MeshRendererComponent& lm = loaded.Renderer();
    CHECK_EQ(lm.MaterialPath, std::string("materials/skin.sagemat"));
    CHECK_EQ(lm.Slots.size(), (size_t)3);
    CHECK_EQ(lm.Slots[0].Path, std::string("materials/jacket.sagemat"));
    CHECK_EQ(lm.Slots[1].Path, std::string());
    CHECK_EQ(lm.Slots[2].Path, std::string("materials/eyes.sagemat"));
}

// Сцена, записанная ДО появления слотов, читается как раньше: слотов нет,
// объект целиком красится своим материалом. Иначе обновление движка означало бы
// перебор всех сцен проекта.
TEST(MeshRenderer_scene_without_slots_still_loads) {
    Scene scene("legacy");
    GameObject obj = scene.CreateObject("Crate");
    obj.Renderer().MaterialPath = "materials/wood.sagemat";

    std::string text = SceneSerializer::SaveToString(scene);
    CHECK_TRUE(text.find("materialSlots") == std::string::npos);   // шума в файле нет

    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    const MeshRendererComponent& lm = back->Get(obj.Id()).Renderer();
    CHECK_TRUE(lm.Slots.empty());
    CHECK_EQ(lm.MaterialPath, std::string("materials/wood.sagemat"));
}

// --- v5 -> v6: части перестали двигать и перекрашивать друг друга ------------
//
// Без миграции каждая уже сделанная галка и каждая строка «значок + подпись»
// съехали бы при первом открытии: текст лёг бы на квадратик и на значок, а
// подложка закрасила бы галку целиком. Проверяем все три перевода по
// отдельности — одна формула может быть верной, а вторая нет.
TEST(Scene_migration_v5_to_v6_moves_a_shifted_label_into_a_child) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 5,
      "objects": [
        {"id": 1, "name": "Check",
         "ui": {"transform": {"size": {"x": 200, "y": 40}, "layer": 3},
                "fill": {"color": {"x":0.1,"y":0.1,"z":0.1,"w":1.0}, "rounding": 6.0},
                "range": {"toggle": true},
                "label": {"text": "Звук", "padX": 8.0}}},
        {"id": 2, "name": "Button",
         "ui": {"transform": {"size": {"x": 200, "y": 40}},
                "fill": {},
                "label": {"text": "OK", "padX": 8.0}}}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_EQ(j["sage_scene_version"].get<int>(), SceneSerializer::CurrentVersion());
    CHECK_EQ(j["objects"].size(), (size_t)3);   // родился ровно один объект-надпись

    // У галки надписи больше нет — она стала ребёнком.
    CHECK_FALSE(j["objects"][0]["ui"].contains("label"));
    // Подложка ушла в цвет квадратика: раньше она и рисовалась только там, а
    // теперь закрасила бы весь элемент вместе с местом под подпись.
    CHECK_FALSE(j["objects"][0]["ui"].contains("fill"));
    CHECK_NEAR(j["objects"][0]["ui"]["range"]["trackColor"]["x"].get<float>(), 0.1f, 1e-4f);
    CHECK_NEAR(j["objects"][0]["ui"]["range"]["rounding"].get<float>(), 6.0f, 1e-4f);
    // У обычной кнопки текст рисовался по всему элементу и раньше: не трогаем.
    CHECK_TRUE(j["objects"][1]["ui"].contains("label"));
    CHECK_TRUE(j["objects"][1]["ui"].contains("fill"));

    const nlohmann::json& child = j["objects"][2];
    CHECK_EQ(child["parent"].get<int>(), 1);
    CHECK_EQ(child["ui"]["label"]["text"].get<std::string>(), std::string("Звук"));
    // Растянут на родителя: иначе подпись зависела бы от размера, записанного
    // однажды, и разъезжалась бы при растяжении самой галки.
    CHECK_EQ(child["ui"]["transform"]["stretch"].get<int>(),
             (int)sage::ui::Transform::Stretch::Both);
    // Левое поле = прежний сдвиг (сторона квадратика 40 + отступ 8) минус
    // боковой отступ надписи, который надпись добавит сама.
    CHECK_NEAR(child["ui"]["transform"]["margin"]["x"].get<float>(), 40.0f, 1e-4f);
    CHECK_NEAR(child["ui"]["transform"]["margin"]["y"].get<float>(), 0.0f, 1e-4f);
    // Слой берётся у родителя: надпись обязана лечь поверх того же, поверх чего
    // лежала галка, а не всплыть на нулевой слой холста.
    CHECK_EQ(child["ui"]["transform"]["layer"].get<int>(), 3);
}

// Значок рядом с подписью: он и сам съезжает в свой объект (раньше жался
// квадратиком к левому краю, теперь занимал бы элемент целиком), и подпись
// сдвигает — по формуле, отличной от галкиной.
TEST(Scene_migration_v5_to_v6_keeps_the_icon_shift) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 5,
      "objects": [
        {"id": 7, "name": "Health",
         "ui": {"transform": {"size": {"x": 240, "y": 32}},
                "icon": {"name": "sun", "size": 20.0},
                "label": {"text": "100", "padX": 6.0}}}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_EQ(j["objects"].size(), (size_t)3);   // значок и надпись — два ребёнка
    CHECK_FALSE(j["objects"][0]["ui"].contains("label"));
    CHECK_FALSE(j["objects"][0]["ui"].contains("icon"));

    // pad = min(4, 32*0.18) = 4; значок стоял в (pad, pad) стороной 20.
    const nlohmann::json& iconChild = j["objects"][1];
    CHECK_EQ(iconChild["parent"].get<int>(), 7);
    CHECK_TRUE(iconChild["ui"].contains("icon"));
    CHECK_NEAR(iconChild["ui"]["transform"]["offset"]["x"].get<float>(), 4.0f, 1e-4f);
    CHECK_NEAR(iconChild["ui"]["transform"]["size"]["x"].get<float>(), 20.0f, 1e-4f);

    const nlohmann::json& textChild = j["objects"][2];
    CHECK_EQ(textChild["parent"].get<int>(), 7);
    // сдвиг = pad*2 + 20 = 28; минус боковой отступ надписи 6.
    CHECK_NEAR(textChild["ui"]["transform"]["margin"]["x"].get<float>(), 22.0f, 1e-4f);
    // id детей свободны: совпадение порвало бы связи иерархии.
    CHECK_TRUE(iconChild["id"].get<int>() > 7);
    CHECK_TRUE(textChild["id"].get<int>() > iconChild["id"].get<int>());
}

// Значок ОДИН в элементе занимал его целиком и раньше — трогать нечего. Иначе
// миграция плодила бы лишний объект на каждую иконку в проекте.
TEST(Scene_migration_v5_to_v6_leaves_a_lone_icon_in_place) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 5,
      "objects": [
        {"id": 3, "name": "Gear",
         "ui": {"transform": {"size": {"x": 32, "y": 32}}, "icon": {"name": "gear"}}}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_EQ(j["objects"].size(), (size_t)1);
    CHECK_TRUE(j["objects"][0]["ui"].contains("icon"));
}

// Ползунок со шкалой рядом: шкала не рисовалась вовсе, а её цвет был цветом
// ручки. После миграции цвет живёт у ползунка, а шкалы на элементе нет — иначе
// поверх дорожки легла бы вторая полоса.
TEST(Scene_migration_v5_to_v6_folds_a_bar_colour_into_the_slider) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 5,
      "objects": [
        {"id": 4, "name": "Volume",
         "ui": {"transform": {"size": {"x": 240, "y": 30}},
                "fill": {"color": {"x":0.2,"y":0.2,"z":0.2,"w":1.0}},
                "bar": {"fillColor": {"x":0.9,"y":0.4,"z":0.1,"w":1.0}},
                "range": {"toggle": false, "value": 0.5}}}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    const nlohmann::json& ui = j["objects"][0]["ui"];
    CHECK_FALSE(ui.contains("bar"));
    CHECK_FALSE(ui.contains("fill"));
    CHECK_NEAR(ui["range"]["accentColor"]["x"].get<float>(), 0.9f, 1e-4f);
    CHECK_NEAR(ui["range"]["trackColor"]["x"].get<float>(), 0.2f, 1e-4f);
}

// Поле ввода рисует свой текст САМО (его прячут точками, обрезают кареткой).
// Вынести его к ребёнку значит показать рядом с полем вторую, неживую копию.
TEST(Scene_migration_v5_to_v6_leaves_a_text_input_alone) {
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 5,
      "objects": [
        {"id": 1, "name": "Field",
         "ui": {"transform": {"size": {"x": 200, "y": 40}},
                "icon": {"name": "sun"},
                "textInput": {},
                "label": {"text": "abc"}}}
      ]
    })";
    const nlohmann::json j = nlohmann::json::parse(SceneSerializer::MigrateSceneJson(old));
    CHECK_TRUE(j["objects"][0]["ui"].contains("label"));   // текст остался у поля
    CHECK_EQ(j["objects"].size(), (size_t)2);              // но значок всё же съехал
}
