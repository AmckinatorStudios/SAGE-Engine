// ---------------------------------------------------------------------------
// Проходы камеры и слои отрисовки (sage/ecs/RenderLayers.h, ScenePasses):
// красный куб стоит ЗА серой стеной. Без проходов его не видно; если его слой
// рисует отдельный проход с очисткой глубины, он поверх стены — так рука или
// оружие не уходят в стены, а движок о «руке» ничего не знает.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
#include <memory>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"

namespace sage::rendertest {
namespace {

std::unique_ptr<Scene> BuildPassScene(bool withPasses) {
    auto scene = std::make_unique<Scene>("CameraPasses");
    scene->Lighting.AmbientStrength = 1.0f;
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;
    auto cube = [&](const char* name, glm::vec3 pos, glm::vec3 scale, glm::vec3 color, const char* layer) {
        GameObject obj = scene->CreateObject(name);
        obj.GetTransform().Position = pos;
        obj.GetTransform().Scale = scale;
        obj.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        obj.Renderer().Color = color;
        obj.Renderer().Layer = layer;
        return obj;
    };
    cube("Wall", {0, 0, -2}, {6, 6, 0.2f}, {0.5f, 0.5f, 0.5f}, "");
    cube("Hand", {0, 0, -4}, {1, 1, 1}, {1, 0, 0}, "viewmodel");
    GameObject cam = scene->CreateObject("Camera");
    CameraComponent& cc = scene->Registry().emplace<CameraComponent>(cam.Entity());
    cc.Fov = 60.0f;
    if (withPasses) {
        CameraPass main;
        main.Layers = {"default"};
        CameraPass hand;
        hand.Layers = {"viewmodel"};
        hand.ClearDepth = true;
        cc.Passes = {main, hand};
    }
    return scene;
}

Image Render(bool withPasses) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    std::unique_ptr<Scene> scene = BuildPassScene(withPasses);
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
    sage::render::CameraPassFrame passes;
    passes.Prepare(*scene, (float)kW / (float)kH);
    passes.BeforeMain(*scene, batch, input);
    passes.ApplyMain(input);
    sage::render::RenderSceneColor(*scene, batch, input);
    passes.AfterMain(*scene, batch, input);
    fbo.Resolve();
    fbo.Bind();
    Image img = Capture(kW, kH);
    device.BindDefaultFramebuffer();
    return img;
}

} // namespace

void RunCameraPassChecks() {
    std::printf("=== Проходы камеры ===\n");
    const Image plain = Render(false);
    const Image passes = Render(true);
    const unsigned char* a = &plain.Pixels[((size_t)(kH / 2) * kW + kW / 2) * 3];
    const unsigned char* b = &passes.Pixels[((size_t)(kH / 2) * kW + kW / 2) * 3];
    std::printf("    без проходов %d %d %d, с проходом %d %d %d\n", a[0], a[1], a[2], b[0], b[1], b[2]);
    Check(a[0] < a[1] + 30, "без проходов куб за стеной не виден");
    Check(b[0] > b[1] + 60, "проход с очисткой глубины рисует слой поверх стены");
}

} // namespace sage::rendertest
