#include "TheBoatLayer.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <array>
#include <algorithm>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/core/Stats.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Screenshot.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/ui/Widgets.h"
#include "game/GameActions.h"
#include "game/PlayerActions.h"
#include "game/GameHud.h"

namespace {

constexpr float kNoclipFlySpeed = 10.0f; // м/с, свободный полёт в noclip
// Полусторона ортобокса карты теней вокруг центра корабля.
constexpr float kShadowRadius = 24.0f;

// Читает позицию/угол камеры и время суток из переменных окружения — для
// детерминированных скриншотов CI/автотестов без реального ввода.
void ApplyDebugEnvOverrides(GameState& game, Camera& camera) {
    if (const char* posEnv = std::getenv("SAGE_CAM_POS")) {
        float x, y, z;
        if (std::sscanf(posEnv, "%f,%f,%f", &x, &y, &z) == 3) game.Player.Position = {x, y, z};
    }
    if (const char* yawEnv = std::getenv("SAGE_CAM_YAW")) camera.Yaw = (float)std::atof(yawEnv);
    if (const char* pitchEnv = std::getenv("SAGE_CAM_PITCH")) camera.Pitch = (float)std::atof(pitchEnv);
    if (const char* t = std::getenv("SAGE_TIME_OF_DAY")) game.DayNight.SetTimeOfDay((float)std::atof(t));
    if (std::getenv("SAGE_NOCLIP")) game.Noclip = true;
    if (std::getenv("SAGE_FORCE_COOKING")) game.CookTimer = 999.0f;
    if (std::getenv("SAGE_DEBUG_HUD")) game.DebugHudVisible = true;
    if (std::getenv("SAGE_FORCE_BITE")) {
        game.Fishing.Cast(camera.Position, camera.Front, GameConstants::SeaLevel);
        game.Fishing.DebugForceBite();
    }
    camera.ProcessMouse(0.0f, 0.0f);
}

// Собирает худ (статбары + часы) из виджетов UI-системы движка.
UICanvas BuildStatsHud(GameState& game) {
    UICanvas hud;
    struct BarDef { const char* label; float PlayerStats::* field; glm::vec3 color; };
    BarDef defs[] = {
        {"HP", &PlayerStats::Health, {0.85f, 0.25f, 0.25f}},
        {"EN", &PlayerStats::Energy, {0.95f, 0.85f, 0.30f}},
        {"FD", &PlayerStats::Hunger, {0.90f, 0.55f, 0.20f}},
        {"WT", &PlayerStats::Thirst, {0.30f, 0.60f, 0.95f}},
    };
    for (int i = 0; i < 4; ++i) {
        auto* bar = hud.Add<UIProgressBar>();
        bar->Anchor = UIAnchor::TopLeft;
        bar->Offset = {16.0f, 16.0f + i * 24.0f};
        bar->Size = {180.0f, 16.0f};
        bar->Label = defs[i].label;
        bar->FillColor = defs[i].color;
        float PlayerStats::* field = defs[i].field;
        bar->ValueSource = [&game, field] { return game.Stats.*field / 100.0f; };
    }

    auto* clockPanel = hud.Add<UIPanel>();
    clockPanel->Anchor = UIAnchor::TopRight;
    clockPanel->Offset = {12.0f, 12.0f};
    clockPanel->Size = {172.0f, 26.0f};
    clockPanel->Color = {0.0f, 0.0f, 0.0f};
    clockPanel->Alpha = 0.5f;

    auto* clockLabel = hud.Add<UILabel>();
    clockLabel->Anchor = UIAnchor::TopRight;
    clockLabel->Offset = {20.0f, 18.0f};
    clockLabel->Scale = 2.0f;
    clockLabel->TextSource = [&game] {
        return "Day " + std::to_string(game.DayCounter) + "  " + game.DayNight.ClockString();
    };
    return hud;
}

// Желаемое направление движения в горизонтальной плоскости (относительно взгляда).
glm::vec3 ReadMoveDirection(InputMap& actions, const Camera& camera) {
    glm::vec3 flatFront = glm::normalize(glm::vec3(camera.Front.x, 0.0f, camera.Front.z));
    glm::vec3 flatRight = glm::normalize(glm::vec3(camera.Right.x, 0.0f, camera.Right.z));
    glm::vec3 dir(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) dir += flatFront;
    if (actions.IsDown(GameActions::MoveBackward)) dir -= flatFront;
    if (actions.IsDown(GameActions::MoveLeft)) dir -= flatRight;
    if (actions.IsDown(GameActions::MoveRight)) dir += flatRight;
    return dir;
}

// Свободный полёт (noclip) для стройки/отладки — минует физику и статы.
void UpdateNoclipFly(GameState& game, InputMap& actions, const Camera& camera, float dt) {
    glm::vec3 fly(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) fly += camera.Front;
    if (actions.IsDown(GameActions::MoveBackward)) fly -= camera.Front;
    if (actions.IsDown(GameActions::MoveLeft)) fly -= camera.Right;
    if (actions.IsDown(GameActions::MoveRight)) fly += camera.Right;
    if (actions.IsDown(GameActions::FlyUp)) fly += glm::vec3(0, 1, 0);
    if (actions.IsDown(GameActions::FlyDown)) fly -= glm::vec3(0, 1, 0);
    if (glm::length(fly) > 0.001f) {
        game.Player.Position += glm::normalize(fly) * kNoclipFlySpeed * dt;
    }
    game.Player.Velocity = glm::vec3(0.0f);
}

} // namespace

void TheBoatLayer::OnAttach() {
    Window& win = sage::Application::Get().GetWindow();
    GLFWwindow* handle = win.Handle();

    m_input.Attach(handle);
    GameActions::RegisterDefaultBindings(m_input);
    // Необязательный файл настроек рядом с игрой — переопределяет раскладку.
    GameActions::LoadBindingsFromFile(m_input.Actions(), "keybindings.cfg");
    glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Рендер-ресурсы ----
    m_voxelShader.emplace("assets/shaders/voxel.vert", "assets/shaders/voxel.frag");
    m_waterShader.emplace("assets/shaders/water.vert", "assets/shaders/water.frag");
    m_basicShader.emplace("assets/shaders/basic.vert", "assets/shaders/basic.frag");
    m_skyboxShader.emplace("assets/shaders/skybox.vert", "assets/shaders/skybox.frag");
    m_particleShader.emplace("assets/shaders/particle.vert", "assets/shaders/particle.frag");
    m_billboardShader.emplace("assets/shaders/billboard.vert", "assets/shaders/billboard.frag");
    m_shadowDepthShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_postShader.emplace("assets/shaders/post.vert", "assets/shaders/post.frag");

    m_sceneFbo.emplace(win.Width(), win.Height());
    m_shadows.emplace(2048);
    m_postEnabled = (std::getenv("SAGE_NO_POST") == nullptr);
    m_shadowsEnabled = (std::getenv("SAGE_NO_SHADOWS") == nullptr);
    if (const char* exp = std::getenv("SAGE_EXPOSURE")) m_postSettings.Exposure = (float)std::atof(exp);

    m_skybox.emplace(std::array<std::string, 6>{
        "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
        "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
        "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
    });
    m_blockAtlas.emplace("assets/textures/blocks_atlas.png", TextureFilter::Nearest, /*generateMipmaps=*/false);
    m_alertIcon.emplace("assets/textures/icon_alert.png", TextureFilter::Bilinear);

    // Иконка «клюёт!» над поплавком — создаём один раз скрытой, дальше только
    // переключаем видимость/позицию по состоянию рыбалки.
    m_biteIconId = m_billboards.Add({
        glm::vec3(0.0f), glm::vec2(0.45f, 0.45f), &m_alertIcon.value(),
        glm::vec4(1.0f), 0.0f, BillboardPivot::Bottom, /*visible=*/false
    });

    m_cubeMesh = ResourceManager::Instance().GetCube();

    // Привязки скриптовых систем — ДО RunScript ниже.
    m_game.Scripts.BindInput(m_input.Actions());
    m_game.Scripts.BindCamera(m_camera);
    m_game.Scripts.BindParticles(m_game.Particles);
    m_game.Scripts.BindBillboards(m_billboards);
    m_game.Scripts.BindAudio(m_game.Audio);

    if (std::getenv("SAGE_RUN_DEMO_SCRIPT")) {
        m_game.Scripts.RunScript("assets/scripts/demo_features.lua");
    }

    m_statsHud = BuildStatsHud(m_game);
    ApplyDebugEnvOverrides(m_game, m_camera);

    if (const char* pathEnv = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = pathEnv;
    if (const char* frameEnv = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(frameEnv);

    LOG_INFO("Game") << "Мир создан. Корабль в (" << m_game.Ship.Center.x << ", "
                     << m_game.Ship.Center.y << ", " << m_game.Ship.Center.z << ")";
}

void TheBoatLayer::OnDetach() {
    // ResourceManager — статический синглтон; очищаем его меши, пока GL-контекст
    // ещё жив (окно движок разрушит после того, как все слои отсоединены).
    ResourceManager::Instance().Clear();
}

void TheBoatLayer::DrawShadowCasters(Shader& depthShader) {
    for (auto& [coord, chunk] : m_game.Terrain.Chunks()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
        depthShader.SetMat4("uModel", model);
        chunk->Mesh().Draw();
    }
    sage::ecs::ForEachRenderable(m_game.SceneData, [&](Transform& tr, MeshRendererComponent& mr) {
        depthShader.SetMat4("uModel", tr.GetMatrix());
        mr.MeshPtr->Draw();
    });
}

void TheBoatLayer::OnUpdate(float deltaTime) {
    sage::Application& app = sage::Application::Get();
    GLFWwindow* w = app.GetWindow().Handle();

    // ================= ВВОД =================
    m_input.Update(w);
    m_input.ApplyMouseDelta(m_camera);
    InputMap& actions = m_input.Actions();

    if (actions.WasPressed(GameActions::QuitGame)) app.Close();
    if (actions.WasPressed(GameActions::Screenshot))
        SaveScreenshot(m_screenshotPath, app.GetWindow().Width(), app.GetWindow().Height());
    if (actions.WasPressed(GameActions::ToggleDebugHud)) m_game.DebugHudVisible = !m_game.DebugHudVisible;
    if (actions.WasPressed(GameActions::ToggleNoclip)) {
        m_game.Noclip = !m_game.Noclip;
        m_game.ShowToast(m_game.Noclip ? "Noclip ON" : "Noclip OFF");
    }
    if (actions.WasPressed(GameActions::ToggleCrafting)) {
        m_game.CraftMenuOpen = !m_game.CraftMenuOpen;
        m_game.Audio.PlaySound2D(GameSounds::Pickup, 0.4f);
    }

    const char* const* hotbarSlots = GameActions::HotbarSlotNames();
    for (int i = 0; i < Inventory::HotbarSize; ++i) {
        if (actions.WasPressed(hotbarSlots[i])) m_game.Items.SelectSlot(i);
    }
    if (actions.WasPressed(GameActions::HotbarNext)) m_game.Items.ScrollSlot(1);
    if (actions.WasPressed(GameActions::HotbarPrev)) m_game.Items.ScrollSlot(-1);

    bool craftMenuConsumedClicks = m_game.CraftMenuOpen;
    if (m_game.CraftMenuOpen) {
        PlayerActions::HandleCraftMenuInput(m_game, actions);
    }

    // ================= ДВИЖЕНИЕ И ФИЗИКА =================
    if (m_game.Noclip) {
        UpdateNoclipFly(m_game, actions, m_camera, deltaTime);
    } else {
        glm::vec3 moveDir = m_game.CraftMenuOpen ? glm::vec3(0.0f) : ReadMoveDirection(actions, m_camera);
        bool wantsJump = !m_game.CraftMenuOpen && actions.IsDown(GameActions::Jump);
        bool wantsRun = !m_game.CraftMenuOpen && actions.IsDown(GameActions::Run);
        m_game.Player.Update(m_game.Terrain, deltaTime, moveDir, wantsJump, wantsRun, m_game.Stats.CanRun());
    }
    m_camera.Position = m_game.Player.EyePosition();

    m_game.Audio.SetListener(m_camera.Position, m_camera.Front, m_camera.Up);

    // ================= СИМУЛЯЦИЯ =================
    bool isExerting = !m_game.Noclip && m_game.Player.IsMovingFast();
    m_game.UpdateSimulation(deltaTime, isExerting);

    // ================= ДЕЙСТВИЯ ИГРОКА =================
    PlayerActions::LookContext look = PlayerActions::ComputeLookContext(m_game, m_camera);

    if (actions.WasPressed(GameActions::PickUpTrash) && !m_game.CraftMenuOpen) {
        PlayerActions::PickUpTrash(m_game, look);
    }
    if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::BreakOrHook)) {
        PlayerActions::HandleLeftClick(m_game, look);
    }
    if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::UseItem)) {
        PlayerActions::HandleRightClick(m_game, m_camera, look);
    }

    m_game.Terrain.RebuildDirtyMeshes();
    m_game.Audio.Update();
}

void TheBoatLayer::OnRender() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    float currentFrame = (float)glfwGetTime();

    g_renderStats.Reset();

    glm::mat4 view = m_camera.GetViewMatrix();
    glm::mat4 proj = m_camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());

    m_sceneFbo->Resize(window.Width(), window.Height());

    // Пересобираем LookContext для HUD (тот же расчёт, что в OnUpdate).
    PlayerActions::LookContext look = PlayerActions::ComputeLookContext(m_game, m_camera);

    // ---- ПРОХОД ТЕНЕЙ ----
    if (m_shadowsEnabled) {
        m_shadows->SetLightMatrix(m_game.SceneData.Lighting.Sun.Direction, m_game.Ship.Center, kShadowRadius);
        m_shadows->BeginRender();
        m_shadowDepthShader->Use();
        m_shadowDepthShader->SetMat4("uLightSpace", m_shadows->LightMatrix());
        DrawShadowCasters(*m_shadowDepthShader);
        m_shadows->EndRender(window.Width(), window.Height());
    }

    // ---- ПРОХОД СЦЕНЫ (в HDR-буфер, либо сразу в экран) ----
    if (m_postEnabled) m_sceneFbo->Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_shadows->DepthTexture());

    m_skybox->Draw(*m_skyboxShader, view, proj, m_game.DayNight.SkyTint());

    // --- корабль (воксельные чанки) ---
    m_voxelShader->Use();
    m_voxelShader->SetMat4("uView", view);
    m_voxelShader->SetMat4("uProjection", proj);
    UploadLighting(*m_voxelShader, m_game.SceneData.Lighting);
    UploadShadowUniforms(*m_voxelShader, m_shadows->LightMatrix(), 1, m_shadowsEnabled);
    m_blockAtlas->Bind(0);
    m_voxelShader->SetInt("uAtlas", 0);
    m_voxelShader->SetInt("uUseTexture", 1);
    for (auto& [coord, chunk] : m_game.Terrain.Chunks()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
        m_voxelShader->SetMat4("uModel", model);
        chunk->Mesh().Draw();
    }

    // --- мусор и поплавок (обычные меши) ---
    m_basicShader->Use();
    m_basicShader->SetMat4("uView", view);
    m_basicShader->SetMat4("uProjection", proj);
    m_basicShader->SetVec3("uViewPos", m_camera.Position);
    UploadLighting(*m_basicShader, m_game.SceneData.Lighting);
    UploadShadowUniforms(*m_basicShader, m_shadows->LightMatrix(), 1, m_shadowsEnabled);
    m_basicShader->SetInt("uUseTexture", 0);

    for (const TrashItem& item : m_game.Trash.Items()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), item.Position);
        model = glm::rotate(model, item.BobPhase * 0.4f, glm::vec3(0, 1, 0));
        model = glm::scale(model, glm::vec3(0.35f));
        m_basicShader->SetMat4("uModel", model);
        m_basicShader->SetVec3("uObjectColor", GetItemFloatColor(item.Type));
        m_cubeMesh->Draw();
    }

    // --- объекты сцены, заспавненные Lua-скриптами (ECS-обход рендерящихся) ---
    sage::ecs::ForEachRenderable(m_game.SceneData, [&](Transform& tr, MeshRendererComponent& mr) {
        m_basicShader->SetMat4("uModel", tr.GetMatrix());
        m_basicShader->SetVec3("uObjectColor", mr.Color);
        mr.MeshPtr->Draw();
    });

    if (m_game.Fishing.IsActive()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m_game.Fishing.VisualBobberPosition());
        model = glm::scale(model, glm::vec3(0.16f));
        m_basicShader->SetMat4("uModel", model);
        m_basicShader->SetVec3("uObjectColor", glm::vec3(0.9f, 0.15f, 0.1f));
        m_cubeMesh->Draw();
    }

    bool isBiting = m_game.Fishing.CurrentState() == FishingSystem::State::Bite;
    m_billboards.SetVisible(m_biteIconId, isBiting);
    if (isBiting) {
        m_billboards.SetPosition(m_biteIconId, m_game.Fishing.VisualBobberPosition() + glm::vec3(0.0f, 0.25f, 0.0f));
    }

    // --- океан (полупрозрачная вода) ---
    m_waterShader->Use();
    m_waterShader->SetMat4("uView", view);
    m_waterShader->SetMat4("uProjection", proj);
    m_waterShader->SetVec3("uViewPos", m_camera.Position);
    m_waterShader->SetFloat("uTime", currentFrame);
    m_waterShader->SetFloat("uScrollSpeed", 2.0f);
    m_waterShader->SetVec2("uCenter", glm::vec2(m_game.Player.Position.x, m_game.Player.Position.z));
    UploadLighting(*m_waterShader, m_game.SceneData.Lighting);
    UploadShadowUniforms(*m_waterShader, m_shadows->LightMatrix(), 1, m_shadowsEnabled);
    m_game.Water.Draw();

    // --- частицы поверх воды, до UI ---
    m_game.Particles.Draw(*m_particleShader, m_camera, view, proj);

    // --- билборды ---
    m_billboards.Draw(*m_billboardShader, m_camera, view, proj);

    // ---- ПОСТ-ПРОЦЕССИНГ: HDR-сцена -> экран ----
    if (m_postEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, window.Width(), window.Height());
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        m_postShader->Use();
        m_postShader->SetFloat("uExposure", m_postSettings.Exposure);
        m_postShader->SetFloat("uGamma", m_postSettings.Gamma);
        m_postShader->SetFloat("uSaturation", m_postSettings.Saturation);
        m_postShader->SetFloat("uContrast", m_postSettings.Contrast);
        m_postShader->SetFloat("uVignette", m_postSettings.VignetteStrength);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneFbo->ColorTexture());
        m_postShader->SetInt("uScene", 0);
        m_post.Draw();
    }

    // --- UI: худ + immediate-mode ---
    m_ui.Begin(window.Width(), window.Height());
    m_statsHud.Draw(m_ui);
    GameHud::DrawWorldHud(m_ui, m_game, look, window.Width(), window.Height());
    m_ui.End();

    if (m_game.DebugHudVisible) {
        GameHud::DrawDebugOverlay(m_debugOverlay, m_game, app.Fps(), window.Width(), window.Height(),
                                  m_postEnabled, m_shadowsEnabled);
    }

    // ---- Скриншот на заданном кадре (для CI) ----
    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}
