#include "SandboxLayer.h"

#include <cstdlib>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/scene/Components.h"
#include "sage/scripting/ScriptEngine.h"

SandboxLayer::SandboxLayer() : sage::Layer("Sandbox") {}
SandboxLayer::~SandboxLayer() = default;

void SandboxLayer::OnAttach() {
    m_shader.emplace("assets/shaders/basic.vert", "assets/shaders/basic.frag");
    m_cube = ResourceManager::Instance().GetCube();

    // --- Сцена: плоскость-пол + несколько цветных кубов через ECS ---
    struct Def { const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale; };
    Def defs[] = {
        {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}},
        {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}},
        {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}},
        {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}},
    };
    for (const Def& d : defs) {
        GameObject obj = m_scene.CreateObject(d.name);
        obj.GetTransform().Position = d.pos;
        obj.GetTransform().Scale = d.scale;
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = m_cube;
        mr.Color = d.color;
    }

    // Один куб со ScriptComponent — доказывает, что Lua-скриптинг работает вне
    // редактора, обычным ECS-путём (ScriptEngine привязывается и тикает сам,
    // без EditorLayer). Мировая матрица его сущности крутится каждый кадр.
    GameObject spinner = m_scene.CreateObject("Spinning Cube");
    spinner.GetTransform().Position = {0.0f, 1.6f, -1.8f};
    MeshRendererComponent& spinnerMr = spinner.Renderer();
    spinnerMr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    spinnerMr.MeshPtr = m_cube;
    spinnerMr.Color = {0.90f, 0.80f, 0.35f};
    m_scene.Registry().emplace<ScriptComponent>(spinner.Entity(),
        ScriptComponent{"assets/scripts/spin.lua"});

    // ScriptEngine привязывается к сцене здесь же и живёт всю партию — тот же
    // паттерн, что использует Play-режим редактора (EditorLayer::StartPlay).
    m_scripts = std::make_unique<ScriptEngine>();
    m_scripts->BindScene(m_scene);
    auto view = m_scene.Registry().view<ScriptComponent>();
    for (auto e : view) {
        m_scripts->AttachScript(GameObject(&m_scene.Registry(), e), view.get<ScriptComponent>(e).Path);
    }

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);

    LOG_INFO("Sandbox") << "SAGE Sandbox запущен (сущностей: " << m_scene.Count() << ")";
}

void SandboxLayer::OnDetach() {
    ResourceManager::Instance().Clear();
}

void SandboxLayer::OnUpdate(float dt) {
    m_scripts->UpdateAll(dt);

    // Медленный авто-облёт сцены вокруг центра — тот же приём, что в
    // EditorLayer::OnUpdate: превью читается как живой 3D-рендер без
    // необходимости подключать InputSystem ради минимального примера.
    float t = (float)glfwGetTime();
    float radius = 8.5f;
    m_camera.Position = {std::cos(t * 0.25f) * radius, 5.0f, std::sin(t * 0.25f) * radius};
    glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) - m_camera.Position);
    m_camera.Yaw = glm::degrees(std::atan2(dir.z, dir.x));
    m_camera.Pitch = glm::degrees(std::asin(dir.y));
    m_camera.ProcessMouse(0.0f, 0.0f);
}

void SandboxLayer::OnRender() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();

    device.SetClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    device.Clear();

    glm::mat4 view = m_camera.GetViewMatrix();
    glm::mat4 proj = m_camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());

    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);
    sage::ecs::ForEachRenderable(m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shader->SetMat4("uModel", tr.GetMatrix());
        m_shader->SetVec3("uObjectColor", mr.Color);
        mr.MeshPtr->Draw();
    });

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}
