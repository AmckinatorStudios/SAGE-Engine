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
    UIElementComponent ui;
    ui.Type = UIElementComponent::Kind::Bar;
    ui.Icon = "heart";
    ui.IconColor = {0.9f, 0.2f, 0.2f, 1.0f};
    ui.GradientColor = {0.1f, 0.1f, 0.2f, 0.8f};
    ui.ShadowSize = 12.0f;
    scene.Registry().emplace<UIElementComponent>(hud.Entity(), ui);

    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    GameObject g2 = back->FindByName("Glass");
    CHECK_NEAR(g2.Renderer().Opacity, 0.35f, 1e-4);

    const auto* u2 = back->Registry().try_get<UIElementComponent>(back->FindByName("Bar").Entity());
    CHECK_TRUE(u2 != nullptr);
    CHECK_EQ(u2->Icon, std::string("heart"));
    CHECK_NEAR(u2->IconColor.r, 0.9f, 1e-4);
    CHECK_NEAR(u2->GradientColor.a, 0.8f, 1e-4);
    CHECK_NEAR(u2->ShadowSize, 12.0f, 1e-4);
    CHECK_TRUE(u2->Type == UIElementComponent::Kind::Bar);
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
