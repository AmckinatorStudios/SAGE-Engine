#include "PlayerLayer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"

namespace fs = std::filesystem;

PlayerLayer::PlayerLayer(fs::path projectDir)
    : sage::Layer("Player"), m_projectDir(std::move(projectDir)) {}
PlayerLayer::~PlayerLayer() = default;

fs::path PlayerLayer::FindMainScene() const {
    fs::path scenesDir = "scenes"; // CWD уже внутри проекта
    fs::path preferred = scenesDir / "main.sage";
    std::error_code ec;
    if (fs::exists(preferred, ec)) return preferred;

    std::vector<fs::path> scenes;
    for (const auto& entry : fs::directory_iterator(scenesDir, ec)) {
        if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes.empty() ? fs::path() : scenes.front();
}

void PlayerLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // 1. Шейдеры плеера — рядом с бинарником, грузим ДО перехода в проект.
    m_shader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    m_shadowShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_shadows.emplace(2048);

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);
    // Скриншот пишется до смены CWD? Нет — путь может быть абсолютным (CI так
    // и делает). Относительный путь окажется внутри папки проекта — норма.

    // 2. Проект: имя из project.sageproj + CWD в корень проекта.
    std::error_code ec;
    fs::path projectFile = m_projectDir / "project.sageproj";
    if (fs::exists(projectFile, ec)) {
        try {
            std::ifstream file(projectFile);
            nlohmann::json root;
            file >> root;
            m_projectName = root.value("name", m_projectName);
        } catch (const std::exception& e) {
            LOG_WARN("Player") << "project.sageproj не парсится (" << e.what() << "), продолжаю";
        }
    }
    fs::current_path(m_projectDir, ec);
    if (ec) {
        LOG_ERROR("Player") << "PLAYER: project dir not accessible: " << m_projectDir.string();
        app.Close();
        return;
    }
    glfwSetWindowTitle(app.GetWindow().Handle(), m_projectName.c_str());

    // 3. Главная сцена.
    fs::path scenePath = FindMainScene();
    if (scenePath.empty()) {
        LOG_ERROR("Player") << "PLAYER: no .sage scenes in " << (m_projectDir / "scenes").string();
        app.Close();
        return;
    }
    try {
        m_scene = SceneSerializer::Load(scenePath.string());
    } catch (const std::exception& e) {
        LOG_ERROR("Player") << "PLAYER: scene load failed: " << e.what();
        app.Close();
        return;
    }

    // 4. Скрипты: игра стартует сразу (Play всегда включён).
    m_scripts = std::make_unique<ScriptEngine>();
    m_scripts->BindScene(*m_scene);
    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        try {
            m_scripts->AttachScript(GameObject(&m_scene->Registry(), e), path);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Player") << "Скрипт не привязался: " << ex.what();
        }
    }

    // Fallback-камера, если сцена без CameraComponent.
    m_fallbackCamera.Position = {6.5f, 5.0f, 6.5f};
    m_fallbackCamera.Yaw = -135.0f;
    m_fallbackCamera.Pitch = -28.0f;
    m_fallbackCamera.ProcessMouse(0.0f, 0.0f);

    LOG_INFO("Player") << "PLAYER: started '" << m_projectName << "', scene "
                       << scenePath.filename().string() << " (" << m_scene->Count()
                       << " entities, " << attached << " scripts)";
}

void PlayerLayer::OnDetach() {
    m_scripts.reset();
    ResourceManager::Instance().Clear();
}

void PlayerLayer::OnUpdate(float dt) {
    if (!m_scene) return;
    m_scripts->UpdateAll(dt);

    if (glfwGetKey(sage::Application::Get().GetWindow().Handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        sage::Application::Get().Close();
    }
}

void PlayerLayer::OnRender() {
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();

    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);

    // --- Тени: глубина от солнца ---
    m_shadows->SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 24.0f);
    m_shadows->BeginRender();
    m_shadowShader->Use();
    m_shadowShader->SetMat4("uLightSpace", m_shadows->LightMatrix());
    sage::ecs::ForEachRenderable(*m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shadowShader->SetMat4("uModel", tr.GetMatrix());
        mr.MeshPtr->Draw();
    });
    m_shadows->EndRender(window.Width(), window.Height());

    // --- Камера: Primary-CameraComponent сцены либо fallback ---
    glm::mat4 view, proj;
    glm::vec3 viewPos;
    float aspect = (float)window.Width() / (float)std::max(window.Height(), 1);
    entt::entity camEntity = entt::null;
    auto camView = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : camView) {
        if (camView.get<CameraComponent>(e).Primary) { camEntity = e; break; }
    }
    if (camEntity != entt::null) {
        const CameraComponent& cam = camView.get<CameraComponent>(camEntity);
        const Transform& tr = camView.get<Transform>(camEntity);
        glm::mat4 rot = glm::eulerAngleXYZ(glm::radians(tr.Rotation.x),
                                           glm::radians(tr.Rotation.y),
                                           glm::radians(tr.Rotation.z));
        glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0, 0, -1, 0)));
        glm::vec3 up = glm::normalize(glm::vec3(rot * glm::vec4(0, 1, 0, 0)));
        view = glm::lookAt(tr.Position, tr.Position + fwd, up);
        proj = glm::perspective(glm::radians(cam.Fov), aspect, cam.NearClip, cam.FarClip);
        viewPos = tr.Position;
    } else {
        view = m_fallbackCamera.GetViewMatrix();
        proj = m_fallbackCamera.GetProjectionMatrix(aspect);
        viewPos = m_fallbackCamera.Position;
    }

    // --- Основной проход: полное освещение + тени ---
    device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
    device.Clear();

    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);
    m_shader->SetVec3("uViewPos", viewPos);
    UploadLighting(*m_shader, env);
    device.BindTexture2D(1, m_shadows->DepthTexture());
    UploadShadowUniforms(*m_shader, m_shadows->LightMatrix(), /*unit=*/1, /*enabled=*/true);
    m_shader->SetInt("uUseTexture", 0);
    sage::ecs::ForEachRenderable(*m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shader->SetMat4("uModel", tr.GetMatrix());
        m_shader->SetVec3("uObjectColor", EffectiveColor(mr));
        mr.MeshPtr->Draw();
    });

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}
