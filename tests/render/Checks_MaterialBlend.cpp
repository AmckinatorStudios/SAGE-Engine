// ---------------------------------------------------------------------------
// Смешение материала (MaterialRender::Blend): полупрозрачный красный куб перед
// синей стеной. Обычное смешение гасит синий фона на долю прозрачности,
// сложение — оставляет его целиком и прибавляет красный (свет, светила).
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
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

Image RenderBlend(MaterialRender::Blending blend) {
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
    wall.Renderer().MaterialPtr = unlit({0, 0, 0.6f}, 1.0f, MaterialRender::Blending::Alpha);
    GameObject glow = cube("Glow", {0, 0, -1}, {1, 1, 0.1f});
    glow.Renderer().MaterialPtr = unlit({0.6f, 0, 0}, 0.5f, blend);

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
}

} // namespace sage::rendertest
