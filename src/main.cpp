// ============================================================================
// The Boat (альфа) на SAGE Engine.
//
// Концепт: бесконечный спокойный океан. Игрок — на небольшом корабле,
// который "плывёт вперёд" (корабль в мире статичен, океан и мусор движутся
// мимо — так проще и стабильнее для воксельной структуры). Корабль целиком
// построен из игровых блоков: его можно ломать, перестраивать и достраивать.
// Из воды вылавливается мусор, из мусора крафтятся блоки, удочка и пресная
// вода. Рыбу можно поймать, пожарить на печке и съесть. У игрока четыре
// стата: здоровье, энергия, голод, жажда. Время суток сменяется, ночью
// разгораются корабельные фонари. Игра про атмосферу и расслабленность.
//
// Управление:
//   WASD — ходьба, Shift — бег, Space — прыжок (в воде — грести вверх)
//   Мышь — взгляд; ЛКМ — сломать блок / подсечь рыбу
//   ПКМ — использовать предмет в руке (поставить блок / закинуть удочку /
//         пожарить рыбу на печке / съесть / выпить)
//   1..9, колесо — хотбар; F — подобрать мусор; Tab — меню крафта
//   V — noclip (режим полёта для стройки), F2 — скриншот, F3 — debug HUD
//
// Устройство файла (после рефакторинга): main() отвечает только за
// инициализацию ресурсов и цикл "ввод -> симуляция -> рендер". Вся игровая
// логика живёт в src/game/:
//   GameState.h     — состояние партии (мир, игрок, инвентарь, системы)
//   PlayerActions.h — реакция на ввод (ломать/ставить/крафтить/есть)
//   GameHud.h        — отрисовка игрового интерфейса
// ============================================================================
#include <cstdlib>
#include <cstdio>
#include <string>
#include <iostream>
#include <algorithm>

#include "core/Version.h"
#include "core/Window.h"
#include "core/Log.h"
#include "core/Stats.h"
#include "core/InputSystem.h"
#include "render/Shader.h"
#include "render/Camera.h"
#include "render/ResourceManager.h"
#include "render/Texture.h"
#include "render/Skybox.h"
#include "render/BillboardSystem.h"
#include "render/LightingUpload.h"
#include "render/Screenshot.h"
#include "render/DebugOverlay.h"
#include "ui/UIRenderer.h"
#include "ui/UICanvas.h"
#include "ui/Widgets.h"
#include "game/GameActions.h"
#include "game/GameState.h"
#include "game/PlayerActions.h"
#include "game/GameHud.h"

namespace {

constexpr float kNoclipFlySpeed = 10.0f; // м/с, свободный полёт в режиме noclip (см. UpdateNoclipFly)

// Читает позицию/угол камеры и время суток из переменных окружения —
// используется автотестами/CI для детерминированных скриншотов без
// реального ввода (см. SAGE_CAM_POS/YAW/PITCH, SAGE_TIME_OF_DAY, SAGE_NOCLIP).
void ApplyDebugEnvOverrides(GameState& game, Camera& camera) {
    if (const char* posEnv = std::getenv("SAGE_CAM_POS")) {
        float x, y, z;
        if (std::sscanf(posEnv, "%f,%f,%f", &x, &y, &z) == 3) game.Player.Position = {x, y, z};
    }
    if (const char* yawEnv = std::getenv("SAGE_CAM_YAW")) camera.Yaw = (float)std::atof(yawEnv);
    if (const char* pitchEnv = std::getenv("SAGE_CAM_PITCH")) camera.Pitch = (float)std::atof(pitchEnv);
    if (const char* t = std::getenv("SAGE_TIME_OF_DAY")) game.DayNight.SetTimeOfDay((float)std::atof(t));
    if (std::getenv("SAGE_NOCLIP")) game.Noclip = true;
    if (std::getenv("SAGE_FORCE_COOKING")) game.CookTimer = 999.0f; // для CI-скриншотов эффекта готовки/дыма
    if (std::getenv("SAGE_DEBUG_HUD")) game.DebugHudVisible = true;
    if (std::getenv("SAGE_FORCE_BITE")) {
        game.Fishing.Cast(camera.Position, camera.Front, GameConstants::SeaLevel);
        game.Fishing.DebugForceBite();
    }
    camera.ProcessMouse(0.0f, 0.0f); // пересчитать Front/Right/Up после ручной правки Yaw/Pitch
}

// Собирает худ (статбары + часы) из виджетов UI-системы движка. Виджеты
// сами тянут актуальные значения через ValueSource/TextSource каждый кадр —
// после сборки этот канвас не нужно больше трогать вручную.
UICanvas BuildStatsHud(GameState& game) {
    UICanvas hud;
    struct BarDef { const char* label; float PlayerStats::* field; glm::vec3 color; };
    // Порядок принципиален: здоровье, энергия, голод, жажда
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

// Читает действия движения и превращает их в желаемое направление в
// горизонтальной плоскости (относительно текущего взгляда камеры)
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

// Свободный полёт (ToggleNoclip) для стройки/отладки — минует физику и статы целиком
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

int main() {
    Log::Init("sage_engine.log");
    LOG_INFO("Game") << "The Boat (alpha) v" << kSageEngineVersion << " запускается...";

    try {
        // Размер и заголовок окна настраиваются через переменные окружения —
        // тот же паттерн, что и остальные SAGE_* debug-переопределения ниже
        // (см. ApplyDebugEnvOverrides), чтобы не редактировать код движка
        // ради другого разрешения при разработке/тестировании.
        int windowWidth = 1280;
        int windowHeight = 720;
        if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) windowWidth = std::atoi(w);
        if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) windowHeight = std::atoi(h);
        std::string windowTitle = std::string("The Boat (alpha) v") + kSageEngineVersion + " - SAGE Engine";

        Window window(windowWidth, windowHeight, windowTitle);

        InputSystem input;
        input.Attach(window.Handle());
        GameActions::RegisterDefaultBindings(input);
        // Необязательный файл настроек рядом с игрой — переопределяет
        // раскладку по умолчанию без пересборки (см. GameActions.h).
        GameActions::LoadBindingsFromFile(input.Actions(), "keybindings.cfg");
        glfwSetInputMode(window.Handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ---- Рендер-ресурсы ----
        Shader voxelShader("assets/shaders/voxel.vert", "assets/shaders/voxel.frag");
        Shader waterShader("assets/shaders/water.vert", "assets/shaders/water.frag");
        Shader basicShader("assets/shaders/basic.vert", "assets/shaders/basic.frag");
        Shader skyboxShader("assets/shaders/skybox.vert", "assets/shaders/skybox.frag");
        Shader particleShader("assets/shaders/particle.vert", "assets/shaders/particle.frag");
        Shader billboardShader("assets/shaders/billboard.vert", "assets/shaders/billboard.frag");

        Skybox skybox({
            "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
            "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
            "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
        });
        Texture blockAtlas("assets/textures/blocks_atlas.png", TextureFilter::Nearest, /*generateMipmaps=*/false);
        Texture alertIcon("assets/textures/icon_alert.png", TextureFilter::Bilinear);

        BillboardSystem billboards;
        // Иконка "клюёт!" над поплавком — создаём один раз, скрытую, и
        // просто переключаем видимость/позицию по состоянию рыбалки каждый
        // кадр, вместо Add/Remove (дешевле и демонстрирует типичный паттерн
        // использования системы для долгоживущего маркера).
        int biteIconId = billboards.Add({
            glm::vec3(0.0f), glm::vec2(0.45f, 0.45f), &alertIcon,
            glm::vec4(1.0f), 0.0f, BillboardPivot::Bottom, /*visible=*/false
        });

        UIRenderer ui;
        DebugOverlay debugOverlay;
        auto cubeMesh = ResourceManager::Instance().GetCube(); // мусор, поплавок

        Camera camera;

        // ---- Игровое состояние: мир, корабль, игрок, инвентарь, системы ----
        GameState game;
        game.Scripts.BindInput(input.Actions()); // InputSystem создан раньше — привязка снаружи GameState
        UICanvas statsHud = BuildStatsHud(game);
        ApplyDebugEnvOverrides(game, camera);

        // Путь скриншота — общий для F2 (ручной, по хоткею) и автоскриншота
        // для CI/тестов (по кадру); раньше F2 был захардкожен на
        // "screenshot.png" отдельно от переопределяемого CI-пути, из-за чего
        // SAGE_SCREENSHOT_PATH молча не действовал на ручной скриншот.
        std::string screenshotPath = "screenshot.png";
        if (const char* pathEnv = std::getenv("SAGE_SCREENSHOT_PATH")) screenshotPath = pathEnv;

        // ---- Автоскриншот для CI/тестов ----
        int autoScreenshotFrame = -1;
        if (const char* frameEnv = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) autoScreenshotFrame = std::atoi(frameEnv);
        int frameCounter = 0;

        LOG_INFO("Game") << "Мир создан. Корабль в (" << game.Ship.Center.x << ", "
                          << game.Ship.Center.y << ", " << game.Ship.Center.z << ")";

        float lastFrame = (float)glfwGetTime();
        float fpsTimer = 0.0f; int fpsFrames = 0; float fps = 0.0f;

        while (!window.ShouldClose()) {
            float currentFrame = (float)glfwGetTime();
            float deltaTime = std::min(currentFrame - lastFrame, 0.05f); // защита от рывка после паузы/лага
            lastFrame = currentFrame;

            fpsTimer += deltaTime; ++fpsFrames;
            if (fpsTimer >= 0.5f) { fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

            GLFWwindow* w = window.Handle();

            // ================= ВВОД =================
            // Один вызов Update() прогоняет ВСЕ зарегистрированные действия
            // через их привязки (клавиатура/мышь/колесо) — дальше весь код
            // читает именованные действия, а не сырые коды клавиш.
            input.Update(w);
            input.ApplyMouseDelta(camera);
            InputMap& actions = input.Actions();

            if (actions.WasPressed(GameActions::QuitGame)) glfwSetWindowShouldClose(w, true);
            if (actions.WasPressed(GameActions::Screenshot)) SaveScreenshot(screenshotPath, window.Width(), window.Height());
            if (actions.WasPressed(GameActions::ToggleDebugHud)) game.DebugHudVisible = !game.DebugHudVisible;
            if (actions.WasPressed(GameActions::ToggleNoclip)) {
                game.Noclip = !game.Noclip;
                game.ShowToast(game.Noclip ? "Noclip ON" : "Noclip OFF");
            }
            if (actions.WasPressed(GameActions::ToggleCrafting)) game.CraftMenuOpen = !game.CraftMenuOpen;

            const char* const* hotbarSlots = GameActions::HotbarSlotNames();
            for (int i = 0; i < Inventory::HotbarSize; ++i) {
                if (actions.WasPressed(hotbarSlots[i])) game.Items.SelectSlot(i);
            }
            if (actions.WasPressed(GameActions::HotbarNext)) game.Items.ScrollSlot(1);
            if (actions.WasPressed(GameActions::HotbarPrev)) game.Items.ScrollSlot(-1);

            // Пока открыто меню крафта, клики мышью ему не передаются —
            // иначе клик по пункту меню одновременно ломал бы блок под прицелом
            bool craftMenuConsumedClicks = game.CraftMenuOpen;
            if (game.CraftMenuOpen) {
                PlayerActions::HandleCraftMenuInput(game, actions);
            }

            // ================= ДВИЖЕНИЕ И ФИЗИКА =================
            if (game.Noclip) {
                UpdateNoclipFly(game, actions, camera, deltaTime);
            } else {
                glm::vec3 moveDir = game.CraftMenuOpen ? glm::vec3(0.0f) : ReadMoveDirection(actions, camera);
                bool wantsJump = !game.CraftMenuOpen && actions.IsDown(GameActions::Jump);
                bool wantsRun = !game.CraftMenuOpen && actions.IsDown(GameActions::Run);
                game.Player.Update(game.Terrain, deltaTime, moveDir, wantsJump, wantsRun, game.Stats.CanRun());
            }
            camera.Position = game.Player.EyePosition();

            // ================= СИМУЛЯЦИЯ (статы, сутки, мусор, рыбалка, готовка) =================
            bool isExerting = !game.Noclip && game.Player.IsMovingFast();
            game.UpdateSimulation(deltaTime, isExerting);

            // ================= ДЕЙСТВИЯ ИГРОКА =================
            PlayerActions::LookContext look = PlayerActions::ComputeLookContext(game, camera);

            if (actions.WasPressed(GameActions::PickUpTrash) && !game.CraftMenuOpen) {
                PlayerActions::PickUpTrash(game, look);
            }
            if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::BreakOrHook)) {
                PlayerActions::HandleLeftClick(game, look);
            }
            if (!craftMenuConsumedClicks && actions.WasPressed(GameActions::UseItem)) {
                PlayerActions::HandleRightClick(game, camera, look);
            }

            game.Terrain.RebuildDirtyMeshes();

            // ================= РЕНДЕР =================
            g_renderStats.Reset();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 proj = camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());

            skybox.Draw(skyboxShader, view, proj, game.DayNight.SkyTint());

            // --- корабль (воксельные чанки) ---
            voxelShader.Use();
            voxelShader.SetMat4("uView", view);
            voxelShader.SetMat4("uProjection", proj);
            UploadLighting(voxelShader, game.SceneData.Lighting);
            blockAtlas.Bind(0);
            voxelShader.SetInt("uAtlas", 0);
            voxelShader.SetInt("uUseTexture", 1);
            for (auto& [coord, chunk] : game.Terrain.Chunks()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
                voxelShader.SetMat4("uModel", model);
                chunk->Mesh().Draw();
            }

            // --- мусор и поплавок (обычные меши) ---
            basicShader.Use();
            basicShader.SetMat4("uView", view);
            basicShader.SetMat4("uProjection", proj);
            basicShader.SetVec3("uViewPos", camera.Position);
            UploadLighting(basicShader, game.SceneData.Lighting);
            basicShader.SetInt("uUseTexture", 0);

            for (const TrashItem& item : game.Trash.Items()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), item.Position);
                model = glm::rotate(model, item.BobPhase * 0.4f, glm::vec3(0, 1, 0));
                model = glm::scale(model, glm::vec3(0.35f));
                basicShader.SetMat4("uModel", model);
                basicShader.SetVec3("uObjectColor", GetItemFloatColor(item.Type));
                cubeMesh->Draw();
            }

            // --- объекты сцены, заспавненные Lua-скриптами (game.Scripts) ---
            for (auto& object : game.SceneData.Objects()) {
                if (!object->MeshComponent) continue; // скрипт создал GameObject, но не назначил меш — не рисуем
                basicShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
                basicShader.SetVec3("uObjectColor", object->Color);
                object->MeshComponent->Draw();
            }

            if (game.Fishing.IsActive()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), game.Fishing.VisualBobberPosition());
                model = glm::scale(model, glm::vec3(0.16f));
                basicShader.SetMat4("uModel", model);
                basicShader.SetVec3("uObjectColor", glm::vec3(0.9f, 0.15f, 0.1f));
                cubeMesh->Draw();
            }

            // Иконка "клюёт!" — billboard над поплавком, видна только в
            // момент поклёвки (демонстрация BillboardSystem: долгоживущий
            // маркер, у которого меняется позиция/видимость, а не Add/Remove)
            bool isBiting = game.Fishing.CurrentState() == FishingSystem::State::Bite;
            billboards.SetVisible(biteIconId, isBiting);
            if (isBiting) {
                billboards.SetPosition(biteIconId, game.Fishing.VisualBobberPosition() + glm::vec3(0.0f, 0.25f, 0.0f));
            }

            // --- океан (после непрозрачного: полупрозрачная вода) ---
            waterShader.Use();
            waterShader.SetMat4("uView", view);
            waterShader.SetMat4("uProjection", proj);
            waterShader.SetVec3("uViewPos", camera.Position);
            waterShader.SetFloat("uTime", currentFrame);
            waterShader.SetFloat("uScrollSpeed", 2.0f);
            waterShader.SetVec2("uCenter", glm::vec2(game.Player.Position.x, game.Player.Position.z));
            UploadLighting(waterShader, game.SceneData.Lighting);
            game.Water.Draw();

            // --- частицы (дым/искры/всплески) поверх воды, до UI ---
            game.Particles.Draw(particleShader, camera, view, proj);

            // --- билборды (иконки/маркеры, всегда развёрнутые к камере) ---
            billboards.Draw(billboardShader, camera, view, proj);

            // --- UI: худ (виджеты) + immediate-mode (хотбар/подсказки/крафт) ---
            ui.Begin(window.Width(), window.Height());
            statsHud.Draw(ui);
            GameHud::DrawWorldHud(ui, game, look, window.Width(), window.Height());
            ui.End();

            if (game.DebugHudVisible) {
                GameHud::DrawDebugOverlay(debugOverlay, game, fps, window.Width(), window.Height());
            }

            // ---- Скриншот на заданном кадре (для CI) ----
            ++frameCounter;
            if (autoScreenshotFrame >= 0 && frameCounter == autoScreenshotFrame) {
                SaveScreenshot(screenshotPath, window.Width(), window.Height());
                glfwSetWindowShouldClose(window.Handle(), true);
            }

            window.SwapBuffers();
            window.PollEvents();
        }

        // ВАЖНО: ResourceManager — статический синглтон. Если не очистить его
        // здесь, его меши будут удаляться уже ПОСЛЕ main() и разрушения окна,
        // когда OpenGL-контекста больше нет — это давало segfault при выходе.
        ResourceManager::Instance().Clear();
    } catch (const std::exception& e) {
        LOG_ERROR("Game") << "Фатальная ошибка: " << e.what();
        std::cerr << "Фатальная ошибка: " << e.what() << std::endl;
        return -1;
    }

    LOG_INFO("Game") << "Завершение работы";
    return 0;
}
