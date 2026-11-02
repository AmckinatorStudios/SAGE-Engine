// Параметры своего шейдера из его текста (render/ShaderParams.h) и путь
// шейдера экрана в звене пост-обработки. Без GL: разбор — чистый текст.
#include "TestFramework.h"

#include "sage/render/PostChainIO.h"
#include "sage/render/PostEffect.h"
#include "sage/render/ShaderParams.h"
#include "sage/render/Material.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

#include <filesystem>

using sage::render::ParseShaderUniforms;

TEST(ShaderParams_reads_annotations) {
    const auto list = ParseShaderUniforms(
        "uniform float uWaveHeight;   // 0.35 [0, 2] Высота волн\n"
        "uniform vec3 uDeepColor; // color 0.02 0.16 0.28 Цвет глубины\n"
        "uniform vec2 uFlow; // 1\n"
        "uniform float uBare;\n");
    CHECK_EQ(list.size(), (size_t)4);
    CHECK_EQ(list[0].Name, std::string("uWaveHeight"));
    CHECK_NEAR(list[0].Default.x, 0.35f, 1e-5f);
    CHECK_TRUE(list[0].HasRange);
    CHECK_NEAR(list[0].Max, 2.0f, 1e-5f);
    CHECK_EQ(list[0].Label, std::string("Высота волн"));
    CHECK_TRUE(list[1].Color);
    CHECK_TRUE(list[1].Type == ShaderParam::Type::Vec3);
    CHECK_NEAR(list[1].Default.z, 0.28f, 1e-5f);
    // Одно число у вектора — во все компоненты.
    CHECK_NEAR(list[2].Default.y, 1.0f, 1e-5f);
    CHECK_NEAR(list[3].Default.x, 0.0f, 1e-5f);
}

TEST(ShaderParams_skip_engine_uniforms) {
    // Свой sampler2D — параметр-текстура; сэмплеры движка (тени) — нет.
    const auto list = ParseShaderUniforms(
        "uniform mat4 uView;\nuniform float uTime;\nuniform vec3 uViewPos;\nuniform sampler2D uShadowMap;\n"
        "uniform sampler2D uMap;\nuniform float uOpacity;\nuniform float uMine; // 2\n");
    CHECK_EQ(list.size(), (size_t)2);
    CHECK_EQ(list[0].Name, std::string("uMap"));
    CHECK_TRUE(list[0].Texture);
    CHECK_EQ(list[1].Name, std::string("uMine"));
    CHECK_TRUE(sage::render::IsEngineUniform("uView"));
    CHECK_FALSE(sage::render::IsEngineUniform("uMine"));
    CHECK_EQ(sage::render::UniformLabel("uWaveHeight"), std::string("Wave Height"));
}

TEST(ShaderParams_screen_shader_source_survives_save) {
    sage::render::PostChain chain;
    sage::render::AddPostEffect(chain, "shader").Source = "assets/shaders/crt.frag";
    const sage::render::PostChain back = sage::render::PostChainFromJson(sage::render::PostChainToJson(chain));
    CHECK_EQ(back.Effects.size(), (size_t)1);
    CHECK_EQ(back.Effects[0].Kind, std::string("shader"));
    CHECK_EQ(back.Effects[0].Source, std::string("assets/shaders/crt.frag"));
}

TEST(ShaderParams_textures_and_headers) {
    const auto list = ParseShaderUniforms(
        "// @header Волны\n"
        "uniform float uHeight; // 0.3 Высота\n"
        "uniform sampler2D uFoam; // Пена\n"
        "// @header Карты\n"
        "uniform sampler2D uFlow; // linear Течение\n"
        "uniform sampler2D uShadowMap;\n");
    CHECK_EQ(list.size(), (size_t)3);
    CHECK_EQ(list[0].Header, std::string("Волны"));
    CHECK_FALSE(list[0].Texture);
    CHECK_TRUE(list[1].Texture);
    CHECK_FALSE(list[1].Linear);
    CHECK_EQ(list[1].Label, std::string("Пена"));
    CHECK_EQ(list[1].Header, std::string("Волны"));
    CHECK_TRUE(list[2].Texture);
    CHECK_TRUE(list[2].Linear);
    CHECK_EQ(list[2].Label, std::string("Течение"));
    CHECK_EQ(list[2].Header, std::string("Карты"));
}

// Текстуры материала-шейдера и маска света переживают сохранение.
TEST(ShaderParams_material_textures_and_light_cookie_survive_save) {
    const std::string file = (std::filesystem::temp_directory_path() / "sage_shader_tex.sagemat").string();
    Material m;
    m.VertexShaderPath = "assets/shaders/a.vert";
    m.FragmentShaderPath = "assets/shaders/a.frag";
    m.TextureParams["uFoam"].Path = "assets/textures/foam.png";
    m.SaveToFile(file);
    const Material back = Material::LoadFromFile(file);
    CHECK_EQ(back.TextureParams.size(), (size_t)1);
    CHECK_EQ(back.TextureParams.at("uFoam").Path, std::string("assets/textures/foam.png"));
    // Отпечаток видит текстуру: смена картинки — несохранённая правка.
    Material changed = back;
    changed.TextureParams["uFoam"].Path = "assets/textures/other.png";
    CHECK_TRUE(changed.Fingerprint() != back.Fingerprint());

    Scene scene("Cookie");
    GameObject lamp = scene.CreateObject("Lamp");
    LightComponent& lc = lamp.Registry()->emplace<LightComponent>(lamp.Entity());
    lc.Kind = LightComponent::Type::Spot;
    lc.Cookie = "assets/textures/lantern.png";
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    bool found = false;
    auto view = loaded->Registry().view<LightComponent>();
    for (auto e : view) found = found || view.get<LightComponent>(e).Cookie == "assets/textures/lantern.png";
    CHECK_TRUE(found);
}

// Имя из контракта шейдера экрана (uTint, uStrength…) у материала-шейдера —
// его собственный параметр: с умолчанием из подсказки и полем в инспекторе.
// Раньше uTint считался служебным, оставался нулём, и материал, который им
// умножает цвет, рисовался чёрным.
TEST(ShaderParams_screen_contract_names_are_material_params) {
    const auto list = ParseShaderUniforms(
        "uniform vec3 uTint; // color 1 0.5 0.25 Оттенок\n"
        "uniform float uStrength; // 0.7\n"
        "uniform mat4 uView;\n");
    CHECK_EQ(list.size(), (size_t)2);
    CHECK_EQ(list[0].Name, std::string("uTint"));
    CHECK_TRUE(list[0].Color);
    CHECK_NEAR(list[0].Default.y, 0.5f, 1e-5f);
    CHECK_EQ(list[1].Name, std::string("uStrength"));
    CHECK_FALSE(sage::render::IsEngineUniform("uTint"));
    CHECK_TRUE(sage::render::IsEngineUniform("uViewPos"));
}

// Параметр из .vert без группы и из .frag под заголовком — в группе; группы вместе.
TEST(ShaderParams_merge_keeps_groups_together) {
    const auto vert = ParseShaderUniforms("uniform float uWaveHeight; // 0.25\nuniform float uWaveSpeed; // 1\n");
    const auto frag = ParseShaderUniforms(
        "uniform vec3 uShade; // color 1 1 1\n"
        "// @header Волны\nuniform float uWaveHeight; // 0.25 Высота волн\nuniform float uRipple; // 0.5\n"
        "// @header Цвет\nuniform vec3 uDeep; // color 0 0 0\n");
    const auto all = sage::render::MergeShaderUniforms({vert, frag});
    CHECK_EQ(all.size(), (size_t)5);
    // Без группы — первыми (uWaveSpeed, uShade), затем «Волны», затем «Цвет».
    CHECK_EQ(all[0].Name, std::string("uWaveSpeed"));
    CHECK_EQ(all[1].Name, std::string("uShade"));
    CHECK_EQ(all[2].Name, std::string("uWaveHeight"));
    CHECK_EQ(all[2].Header, std::string("Волны"));
    CHECK_EQ(all[2].Label, std::string("Высота волн"));
    CHECK_EQ(all[3].Name, std::string("uRipple"));
    CHECK_EQ(all[4].Header, std::string("Цвет"));
}
