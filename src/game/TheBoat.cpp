#include "TheBoat.h"
#include "GameActions.h"
#include "GameHud.h"
#include "Items.h"
#include "../ui/Widgets.h" // UIProgressBar/UIPanel/UILabel для BuildStatsHud
#include "core/Version.h"  // сгенерирован CMake в build/generated/core/ (см. CMakeLists)
#include "../core/Log.h"
#include "../core/Stats.h"
#include "../render/LightingUpload.h"
#include "../render/ResourceManager.h"
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <algorithm>

namespace {
constexpr float kNoclipFlySpeed = 10.0f; // м/с, свободный полёт в режиме noclip

// Полусторона ортобокса карты теней вокруг центра корабля — накрывает весь
// корабль плюс воду вокруг него с запасом (см. ShadowMap::SetLightMatrix).
constexpr float kShadowRadius = 24.0f;
}

EngineConfig TheBoat::DefaultConfig() {
    EngineConfig config;
    config.Width = 1280;
    config.Height = 720;
    config.Title = std::string("The Boat (alpha) v") + kSageEngineVersion + " - SAGE Engine";
    return config;
}

TheBoat::TheBoat(Engine& engine)
    : m_voxelShader("assets/shaders/voxel.vert", "assets/shaders/voxel.frag"),
      m_waterShader("assets/shaders/water.vert", "assets/shaders/water.frag"),
      m_basicShader("assets/shaders/basic.vert", "assets/shaders/basic.frag"),
      m_skyboxShader("assets/shaders/skybox.vert", "assets/shaders/skybox.frag"),
      m_particleShader("assets/shaders/particle.vert", "assets/shaders/particle.frag"),
      m_billboardShader("assets/shaders/billboard.vert", "assets/shaders/billboard.frag"),
      m_shadowDepthShader("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag"),
      m_postShader("assets/shaders/post.vert", "assets/shaders/post.frag"),
      m_skybox({
          "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
          "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
          "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
      }),
      m_blockAtlas("assets/textures/blocks_atlas.png", TextureFilter::Nearest, /*generateMipmaps=*/false),
      m_alertIcon("assets/textures/icon_alert.png", TextureFilter::Bilinear),
      m_sceneFbo(engine.GetWindow().Width(), engine.GetWindow().Height()),
      m_shadows(2048) {
    m_cubeMesh = ResourceManager::Instance().GetCube(); // мусор, поплавок

    // Иконка "клюёт!" над поплавком — создаём один раз, скрытую, и просто
    // переключаем видимость/позицию по состоянию рыбалки каждый кадр.
    m_biteIconId = m_billboards.Add({
        glm::vec3(0.0f), glm::vec2(0.45f, 0.45f), &m_alertIcon,
        glm::vec4(1.0f), 0.0f, BillboardPivot::Bottom, /*visible=*/false
    });

    // Тумблеры рендер-пайплайна (для сравнения "до/после"): пост-процессинг и
    // тени можно отключить env-флагом, экспозицию — переопределить.
    m_postEnabled = (std::getenv("SAGE_NO_POST") == nullptr);
    m_shadowsEnabled = (std::getenv("SAGE_NO_SHADOWS") == nullptr);
    if (const char* exp = std::getenv("SAGE_EXPOSURE")) m_postSettings.Exposure = (float)std::atof(exp);
}

void TheBoat::OnStart(Engine& engine) {
    LOG_INFO("Game") << "The Boat (alpha) v" << kSageEngineVersion << " запускается...";

    // Раскладка действий — знание ИГРЫ о клавишах. Движок владеет системой
    // ввода и опрашивает её каждый кадр, но какие действия и на чём — решает игра.
    GameActions::RegisterDefaultBindings(engine.GetInput());
    // Необязательный файл настроек рядом с игрой — переопределяет раскладку
    // без пересборки (см. GameActions.h).
    GameActions::LoadBindingsFromFile(engine.GetInput().Actions(), "keybindings.cfg");
    glfwSetInputMode(engine.GetWindow().Handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Базовое GL-состояние для UI-поверх-3D: альфа-блендинг.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Скрипты: привязываем движковые системы, чтобы Lua мог с ними работать.
    // Все Bind* должны отработать ДО RunScript ниже — иначе демо-скрипт упадёт
    // на первом же обращении к ещё не привязанной системе.
    m_state.Scripts.BindInput(engine.GetInput().Actions());
    m_state.Scripts.BindCamera(m_camera);
    m_state.Scripts.BindParticles(m_state.Particles);
    m_state.Scripts.BindBillboards(m_billboards);

    // demo_features.lua — витрина расширенного Lua-API движка, не относится к
    // геймплею The Boat — только по явному запросу.
    if (std::getenv("SAGE_RUN_DEMO_SCRIPT")) {
        m_state.Scripts.RunScript("assets/scripts/demo_features.lua");
    }

    BuildStatsHud();
    ApplyDebugEnvOverrides();

    LOG_INFO("Game") << "Мир создан. Корабль в (" << m_state.Ship.Center.x << ", "
                      << m_state.Ship.Center.y << ", " << m_state.Ship.Center.z << ")";
}

void TheBoat::OnUpdate(Engine& engine, float dt) {
    InputSystem& input = engine.GetInput();
    input.ApplyMouseDelta(m_camera);
    InputMap& actions = input.Actions();

    // ================= РЕАКЦИЯ НА ВВОД =================
    if (actions.WasPressed(GameActions::QuitGame)) engine.RequestClose();
    if (actions.WasPressed(GameActions::Screenshot)) engine.TakeScreenshot();
    if (actions.WasPressed(GameActions::ToggleDebugHud)) m_state.DebugHudVisible = !m_state.DebugHudVisible;
    if (actions.WasPressed(GameActions::ToggleNoclip)) {
        m_state.Noclip = !m_state.Noclip;
        m_state.ShowToast(m_state.Noclip ? "Noclip ON" : "Noclip OFF");
    }
    if (actions.WasPressed(GameActions::ToggleCrafting)) m_state.CraftMenuOpen = !m_state.CraftMenuOpen;

    const char* const* hotbarSlots = GameActions::HotbarSlotNames();
    for (int i = 0; i < Inventory::HotbarSize; ++i) {
        if (actions.WasPressed(hotbarSlots[i])) m_state.Items.SelectSlot(i);
    }
    if (actions.WasPressed(GameActions::HotbarNext)) m_state.Items.ScrollSlot(1);
    if (actions.WasPressed(GameActions::HotbarPrev)) m_state.Items.ScrollSlot(-1);

    // Пока открыто меню крафта, клики мышью ему не передаются — иначе клик по
    // пункту меню одновременно ломал бы блок под прицелом.
    bool craftMenuConsumedClicks = m_state.CraftMenuOpen;
    if (m_state.CraftMenuOpen) {
        PlayerActions::HandleCraftMenuInput(m_state, actions);
    }

    // ================= ДВИЖЕНИЕ И ФИЗИКА =================
    if (m_state.Noclip) {
        UpdateNoclipFly(actions, dt);
    } else {
        glm::vec3 moveDir = m_state.CraftMenuOpen ? glm::vec3(0.0f) : ReadMoveDirection(actions);
        bool wantsJump = !m_state.CraftMenuOpen && actions.IsDown(GameActions::Jump);
        bool wantsRun = !m_state.CraftMenuOpen && actions.IsDown(GameActions::Run);
        m_state.Player.Update(m_state.Terrain, dt, moveDir, wantsJump, wantsRun, m_state.Stats.CanRun());
    }
    m_camera.Position = m_state.Player.EyePosition();

    // ================= СИМУЛЯЦИЯ =================
    bool isExerting = !m_state.Noclip && m_state.Player.IsMovingFast();
    m_state.UpdateSimulation(dt, isExerting);

    // ================= ДЕЙСТВИЯ ИГРОКА =================
    m_look = PlayerActions::ComputeLookContext(m_state, m_camera);

    if (actions.WasPressed(GameActions::PickUpTrash) && !m_state.CraftMenuOpen) {
        PlayerActions::PickUpTrash(m_state, m_look);
    }
    if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::BreakOrHook)) {
        PlayerActions::HandleLeftClick(m_state, m_look);
    }
    if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::UseItem)) {
        PlayerActions::HandleRightClick(m_state, m_camera, m_look);
    }

    m_state.Terrain.RebuildDirtyMeshes();
}

void TheBoat::OnRender(Engine& engine) {
    Window& window = engine.GetWindow();
    g_renderStats.Reset();

    glm::mat4 view = m_camera.GetViewMatrix();
    glm::mat4 proj = m_camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());

    // Держим offscreen-буфер сцены в размер окна (no-op, если не менялось).
    m_sceneFbo.Resize(window.Width(), window.Height());

    // ---- ПРОХОД ТЕНЕЙ: глубина сцены из точки зрения солнца ----
    // Ортобокс следует за кораблём (стабильный центр — меньше мерцания краёв тени).
    if (m_shadowsEnabled) {
        m_shadows.SetLightMatrix(m_state.SceneData.Lighting.Sun.Direction, m_state.Ship.Center, kShadowRadius);
        m_shadows.BeginRender();
        m_shadowDepthShader.Use();
        m_shadowDepthShader.SetMat4("uLightSpace", m_shadows.LightMatrix());
        DrawShadowCasters(m_shadowDepthShader);
        m_shadows.EndRender(window.Width(), window.Height());
    }

    // ---- ПРОХОД СЦЕНЫ: в HDR-буфер (или сразу в экран, если пост выключен) ----
    if (m_postEnabled) m_sceneFbo.Bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Карта теней на юнит 1 — общая для voxel/basic/water на весь проход
    // (юнит 0 занят их собственными текстурами: атлас/спрайт).
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_shadows.DepthTexture());

    m_skybox.Draw(m_skyboxShader, view, proj, m_state.DayNight.SkyTint());

    // --- корабль (воксельные чанки) ---
    m_voxelShader.Use();
    m_voxelShader.SetMat4("uView", view);
    m_voxelShader.SetMat4("uProjection", proj);
    UploadLighting(m_voxelShader, m_state.SceneData.Lighting);
    UploadShadowUniforms(m_voxelShader, m_shadows.LightMatrix(), 1, m_shadowsEnabled);
    m_blockAtlas.Bind(0);
    m_voxelShader.SetInt("uAtlas", 0);
    m_voxelShader.SetInt("uUseTexture", 1);
    for (auto& [coord, chunk] : m_state.Terrain.Chunks()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
        m_voxelShader.SetMat4("uModel", model);
        chunk->Mesh().Draw();
    }

    // --- мусор и поплавок (обычные меши) ---
    m_basicShader.Use();
    m_basicShader.SetMat4("uView", view);
    m_basicShader.SetMat4("uProjection", proj);
    m_basicShader.SetVec3("uViewPos", m_camera.Position);
    UploadLighting(m_basicShader, m_state.SceneData.Lighting);
    UploadShadowUniforms(m_basicShader, m_shadows.LightMatrix(), 1, m_shadowsEnabled);
    m_basicShader.SetInt("uUseTexture", 0);

    for (const TrashItem& item : m_state.Trash.Items()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), item.Position);
        model = glm::rotate(model, item.BobPhase * 0.4f, glm::vec3(0, 1, 0));
        model = glm::scale(model, glm::vec3(0.35f));
        m_basicShader.SetMat4("uModel", model);
        m_basicShader.SetVec3("uObjectColor", GetItemFloatColor(item.Type));
        m_cubeMesh->Draw();
    }

    // --- объекты сцены, заспавненные Lua-скриптами (m_state.Scripts) ---
    for (auto& object : m_state.SceneData.Objects()) {
        if (!object->MeshComponent) continue; // скрипт создал GameObject, но не назначил меш — не рисуем
        m_basicShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
        m_basicShader.SetVec3("uObjectColor", object->Color);
        object->MeshComponent->Draw();
    }

    if (m_state.Fishing.IsActive()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m_state.Fishing.VisualBobberPosition());
        model = glm::scale(model, glm::vec3(0.16f));
        m_basicShader.SetMat4("uModel", model);
        m_basicShader.SetVec3("uObjectColor", glm::vec3(0.9f, 0.15f, 0.1f));
        m_cubeMesh->Draw();
    }

    // Иконка "клюёт!" — billboard над поплавком, видна только в момент поклёвки.
    bool isBiting = m_state.Fishing.CurrentState() == FishingSystem::State::Bite;
    m_billboards.SetVisible(m_biteIconId, isBiting);
    if (isBiting) {
        m_billboards.SetPosition(m_biteIconId, m_state.Fishing.VisualBobberPosition() + glm::vec3(0.0f, 0.25f, 0.0f));
    }

    // --- океан (после непрозрачного: полупрозрачная вода) ---
    m_waterShader.Use();
    m_waterShader.SetMat4("uView", view);
    m_waterShader.SetMat4("uProjection", proj);
    m_waterShader.SetVec3("uViewPos", m_camera.Position);
    m_waterShader.SetFloat("uTime", engine.Time());
    m_waterShader.SetFloat("uScrollSpeed", 2.0f);
    m_waterShader.SetVec2("uCenter", glm::vec2(m_state.Player.Position.x, m_state.Player.Position.z));
    UploadLighting(m_waterShader, m_state.SceneData.Lighting);
    UploadShadowUniforms(m_waterShader, m_shadows.LightMatrix(), 1, m_shadowsEnabled);
    m_state.Water.Draw();

    // --- частицы (дым/искры/всплески) поверх воды, до UI ---
    m_state.Particles.Draw(m_particleShader, m_camera, view, proj);

    // --- билборды (иконки/маркеры, всегда развёрнутые к камере) ---
    m_billboards.Draw(m_billboardShader, m_camera, view, proj);

    // ---- ПОСТ-ПРОЦЕССИНГ: HDR-сцена -> экран ----
    // UI рисуется ПОСЛЕ него, в экранный буфер напрямую, чтобы текст/полоски
    // не тон-мапились и не виньетировались вместе со сценой.
    if (m_postEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, window.Width(), window.Height());
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        m_postShader.Use();
        m_postShader.SetFloat("uExposure", m_postSettings.Exposure);
        m_postShader.SetFloat("uGamma", m_postSettings.Gamma);
        m_postShader.SetFloat("uSaturation", m_postSettings.Saturation);
        m_postShader.SetFloat("uContrast", m_postSettings.Contrast);
        m_postShader.SetFloat("uVignette", m_postSettings.VignetteStrength);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneFbo.ColorTexture());
        m_postShader.SetInt("uScene", 0);
        m_post.Draw();
    }

    // --- UI: худ (виджеты) + immediate-mode (хотбар/подсказки/крафт) ---
    m_ui.Begin(window.Width(), window.Height());
    m_statsHud.Draw(m_ui);
    GameHud::DrawWorldHud(m_ui, m_state, m_look, window.Width(), window.Height());
    m_ui.End();

    if (m_state.DebugHudVisible) {
        GameHud::DrawDebugOverlay(m_debugOverlay, m_state, engine.Fps(), window.Width(), window.Height(),
                                  m_postEnabled, m_shadowsEnabled);
    }
}

void TheBoat::OnShutdown(Engine& engine) {
    (void)engine;
    // ResourceManager — статический синглтон. Чистим его меши здесь, пока
    // GL-контекст ещё жив (OnShutdown вызывается движком до разрушения окна) —
    // иначе GL-объекты удалялись бы уже без контекста (краш при выходе).
    ResourceManager::Instance().Clear();
}

// ---------------------------------------------------------------------
// Вспомогательные методы (были свободными функциями в main.cpp)
// ---------------------------------------------------------------------

void TheBoat::BuildStatsHud() {
    struct BarDef { const char* label; float PlayerStats::* field; glm::vec3 color; };
    // Порядок принципиален: здоровье, энергия, голод, жажда
    BarDef defs[] = {
        {"HP", &PlayerStats::Health, {0.85f, 0.25f, 0.25f}},
        {"EN", &PlayerStats::Energy, {0.95f, 0.85f, 0.30f}},
        {"FD", &PlayerStats::Hunger, {0.90f, 0.55f, 0.20f}},
        {"WT", &PlayerStats::Thirst, {0.30f, 0.60f, 0.95f}},
    };
    for (int i = 0; i < 4; ++i) {
        auto* bar = m_statsHud.Add<UIProgressBar>();
        bar->Anchor = UIAnchor::TopLeft;
        bar->Offset = {16.0f, 16.0f + i * 24.0f};
        bar->Size = {180.0f, 16.0f};
        bar->Label = defs[i].label;
        bar->FillColor = defs[i].color;
        float PlayerStats::* field = defs[i].field;
        bar->ValueSource = [this, field] { return m_state.Stats.*field / 100.0f; };
    }

    auto* clockPanel = m_statsHud.Add<UIPanel>();
    clockPanel->Anchor = UIAnchor::TopRight;
    clockPanel->Offset = {12.0f, 12.0f};
    clockPanel->Size = {172.0f, 26.0f};
    clockPanel->Color = {0.0f, 0.0f, 0.0f};
    clockPanel->Alpha = 0.5f;

    auto* clockLabel = m_statsHud.Add<UILabel>();
    clockLabel->Anchor = UIAnchor::TopRight;
    clockLabel->Offset = {20.0f, 18.0f};
    clockLabel->Scale = 2.0f;
    clockLabel->TextSource = [this] {
        return "Day " + std::to_string(m_state.DayCounter) + "  " + m_state.DayNight.ClockString();
    };
}

void TheBoat::ApplyDebugEnvOverrides() {
    // Читает позицию/угол камеры и время суток из env — для headless-тестов/CI
    // (детерминированные скриншоты без реального ввода).
    if (const char* posEnv = std::getenv("SAGE_CAM_POS")) {
        float x, y, z;
        if (std::sscanf(posEnv, "%f,%f,%f", &x, &y, &z) == 3) m_state.Player.Position = {x, y, z};
    }
    if (const char* yawEnv = std::getenv("SAGE_CAM_YAW")) m_camera.Yaw = (float)std::atof(yawEnv);
    if (const char* pitchEnv = std::getenv("SAGE_CAM_PITCH")) m_camera.Pitch = (float)std::atof(pitchEnv);
    if (const char* t = std::getenv("SAGE_TIME_OF_DAY")) m_state.DayNight.SetTimeOfDay((float)std::atof(t));
    if (std::getenv("SAGE_NOCLIP")) m_state.Noclip = true;
    if (std::getenv("SAGE_FORCE_COOKING")) m_state.CookTimer = 999.0f; // CI-скриншот эффекта готовки/дыма
    if (std::getenv("SAGE_DEBUG_HUD")) m_state.DebugHudVisible = true;
    if (std::getenv("SAGE_FORCE_BITE")) {
        m_state.Fishing.Cast(m_camera.Position, m_camera.Front, GameConstants::SeaLevel);
        m_state.Fishing.DebugForceBite();
    }
    m_camera.ProcessMouse(0.0f, 0.0f); // пересчитать Front/Right/Up после ручной правки Yaw/Pitch
}

glm::vec3 TheBoat::ReadMoveDirection(InputMap& actions) const {
    glm::vec3 flatFront = glm::normalize(glm::vec3(m_camera.Front.x, 0.0f, m_camera.Front.z));
    glm::vec3 flatRight = glm::normalize(glm::vec3(m_camera.Right.x, 0.0f, m_camera.Right.z));
    glm::vec3 dir(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) dir += flatFront;
    if (actions.IsDown(GameActions::MoveBackward)) dir -= flatFront;
    if (actions.IsDown(GameActions::MoveLeft)) dir -= flatRight;
    if (actions.IsDown(GameActions::MoveRight)) dir += flatRight;
    return dir;
}

void TheBoat::UpdateNoclipFly(InputMap& actions, float dt) {
    glm::vec3 fly(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) fly += m_camera.Front;
    if (actions.IsDown(GameActions::MoveBackward)) fly -= m_camera.Front;
    if (actions.IsDown(GameActions::MoveLeft)) fly -= m_camera.Right;
    if (actions.IsDown(GameActions::MoveRight)) fly += m_camera.Right;
    if (actions.IsDown(GameActions::FlyUp)) fly += glm::vec3(0, 1, 0);
    if (actions.IsDown(GameActions::FlyDown)) fly -= glm::vec3(0, 1, 0);
    if (glm::length(fly) > 0.001f) {
        m_state.Player.Position += glm::normalize(fly) * kNoclipFlySpeed * dt;
    }
    m_state.Player.Velocity = glm::vec3(0.0f);
}

void TheBoat::DrawShadowCasters(Shader& depthShader) {
    // Корабль (воксельные чанки) + заспавненные скриптами объекты. Один
    // depth-шейдер годится и для VoxelMesh, и для Mesh — позиция у обоих в
    // location 0. Вода не кастит (плоская), только принимает тень.
    for (auto& [coord, chunk] : m_state.Terrain.Chunks()) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
        depthShader.SetMat4("uModel", model);
        chunk->Mesh().Draw();
    }
    for (auto& object : m_state.SceneData.Objects()) {
        if (!object->MeshComponent) continue;
        depthShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
        object->MeshComponent->Draw();
    }
}
