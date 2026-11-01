// Параметры своего шейдера из его текста (render/ShaderParams.h) и путь
// шейдера экрана в звене пост-обработки. Без GL: разбор — чистый текст.
#include "TestFramework.h"

#include "sage/render/PostChainIO.h"
#include "sage/render/PostEffect.h"
#include "sage/render/ShaderParams.h"

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

TEST(ShaderParams_skip_engine_uniforms_and_samplers) {
    const auto list = ParseShaderUniforms(
        "uniform mat4 uView;\nuniform float uTime;\nuniform vec3 uViewPos;\n"
        "uniform sampler2D uMap;\nuniform float uOpacity;\nuniform float uMine; // 2\n");
    CHECK_EQ(list.size(), (size_t)1);
    CHECK_EQ(list[0].Name, std::string("uMine"));
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
