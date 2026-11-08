// ---------------------------------------------------------------------------
// Смешение материала (MaterialRender::Blend): полупрозрачный красный куб перед
// синей стеной. Обычное смешение гасит синий фона на долю прозрачности,
// сложение — оставляет его целиком и прибавляет красный (свет, светила).
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"

namespace sage::rendertest {
namespace {

Image RenderBlend(MaterialRender::Blending blend, glm::vec3 overlay = {0.6f, 0, 0}, float overlayZ = -1.0f,
                  float depthBias = 0.0f, glm::vec3 wallColor = {0, 0, 0.6f}) {
    auto scene = std::make_unique<Scene>("MaterialBlend");
    scene->Lighting.AmbientStrength = 1.0f;
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;
    auto cube = [&](const char* name, glm::vec3 pos, glm::vec3 scale) {
        GameObject obj = scene->CreateObject(name);
        obj.GetTransform().Position = pos;
        obj.GetTransform().Scale = scale;
        obj.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        return obj;
    };
    auto unlit = [](glm::vec3 color, float opacity, MaterialRender::Blending b) {
        auto m = std::make_shared<Material>();
        m->Albedo = color;
        m->Opacity = opacity;
        m->Render.Unlit = true;
        m->Render.Blend = b;
        return m;
    };
    GameObject wall = cube("Wall", {0, 0, -3}, {6, 6, 0.2f});
    wall.Renderer().MaterialPtr = unlit(wallColor, 1.0f, MaterialRender::Blending::Alpha);
    // Слой — та же толщина, что у стены: при overlayZ = −3 его лицевая грань
    // лежит РОВНО на лицевой грани стены.
    GameObject glow = cube("Glow", {0, 0, overlayZ}, {1, 1, overlayZ < -2.5f ? 0.2f : 0.1f});
    auto overlayMat = unlit(overlay, 0.5f, blend);
    overlayMat->Render.DepthBias = depthBias;
    glow.Renderer().MaterialPtr = overlayMat;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    sage::ecs::RenderBatch batch;
    Framebuffer fbo(kW, kH);
    fbo.Bind();
    device.SetViewport(0, 0, kW, kH);
    device.SetClearColor(0, 0, 0, 1);
    device.Clear(true, true);
    sage::render::SceneColorInput input;
    input.View = glm::lookAt(glm::vec3(0, 0, 2), glm::vec3(0, 0, -10), glm::vec3(0, 1, 0));
    input.Proj = glm::perspective(glm::radians(60.0f), (float)kW / (float)kH, 0.1f, 100.0f);
    input.ViewPos = glm::vec3(0, 0, 2);
    input.Env = &env;
    sage::render::RenderSceneColor(*scene, batch, input);
    fbo.Resolve();
    fbo.Bind();
    Image img = Capture(kW, kH);
    device.BindDefaultFramebuffer();
    return img;
}

} // namespace

void RunMaterialBlendChecks() {
    std::printf("=== Смешение материала ===\n");
    const Image alpha = RenderBlend(MaterialRender::Blending::Alpha);
    const Image add = RenderBlend(MaterialRender::Blending::Additive);
    const unsigned char* a = &alpha.Pixels[((size_t)(kH / 2) * kW + kW / 2) * 3];
    const unsigned char* b = &add.Pixels[((size_t)(kH / 2) * kW + kW / 2) * 3];
    std::printf("    обычное %d %d %d, сложение %d %d %d\n", a[0], a[1], a[2], b[0], b[1], b[2]);
    Check(a[0] > 40 && a[2] > 40, "обычное смешение: красный поверх синего, оба видны");
    Check(b[2] > a[2] + 20, "сложение не гасит фон: синий остаётся целиком");
    Check(b[0] > 40, "сложение прибавляет цвет материала");

    // Умножение ×2 (трещины Minecraft: GL_DST_COLOR, GL_SRC_COLOR): линейное
    // 0.5 фон не меняет, 0.25 — гасит вдвое (альбедо задаётся в sRGB:
    // 0.7297 и 0.5325 — это линейные 0.5 и 0.25). Слой лежит прямо на стене
    // со смещением глубины к камере — иначе его не видно или он мерцает.
    using MB = MaterialRender::Blending;
    const glm::vec3 wallC(0.2f, 0.4f, 0.6f);
    const Image wallOnly = RenderBlend(MB::Modulate2x, glm::vec3(0.0f), -6.0f, 0.0f, wallC);
    const Image plain = RenderBlend(MB::Modulate2x, glm::vec3(0.7297f), -3.0f, -3.0f, wallC);
    const Image dark = RenderBlend(MB::Modulate2x, glm::vec3(0.5325f), -3.0f, -3.0f, wallC);
    // Слой на 2·10⁻⁵ ЗА стеной: меньше трёх шагов глубины — смещение −3 его
    // выводит, без смещения он целиком за стеной.
    const Image withBias = RenderBlend(MB::Modulate2x, glm::vec3(0.0f), -3.00002f, -3.0f, wallC);
    auto px = [](const Image& im) { return &im.Pixels[((size_t)(kH / 2) * kW + kW / 2) * 3]; };
    const unsigned char* w = px(wallOnly);
    const unsigned char* p = px(plain);
    const unsigned char* d = px(dark);
    std::printf("    умножение ×2: стена %d %d %d, ×(2·0.5) %d %d %d, ×(2·0.25) %d %d %d\n", w[0], w[1], w[2], p[0], p[1],
                p[2], d[0], d[1], d[2]);
    Check(w[2] > 40 && std::abs((int)p[2] - (int)w[2]) <= 6 && std::abs((int)p[1] - (int)w[1]) <= 6,
          "умножение ×2: линейное 0.5 фон не меняет");
    Check(std::abs((int)d[2] * 2 - (int)w[2]) <= 8 && std::abs((int)d[1] * 2 - (int)w[1]) <= 8,
          "умножение ×2: линейное 0.25 гасит фон вдвое");
    const unsigned char* wb = px(withBias);
    std::printf("    чёрный слой чуть за стеной со смещением −3: %d\n", wb[2]);
    Check(wb[2] < 10, "смещение глубины выводит слой, лежащий на поверхности, вперёд");
}

} // namespace sage::rendertest
