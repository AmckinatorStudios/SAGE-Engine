#include "PlayerLayer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "sage/core/Application.h"
#include "sage/core/Config.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/Screenshot.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UISceneSystem.h"

namespace fs = std::filesystem;

PlayerLayer::PlayerLayer(fs::path projectDir, std::string launchArgs)
    : sage::Layer("Player"), m_projectDir(std::move(projectDir)),
      m_launchArgs(std::move(launchArgs)) {}
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
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    m_shader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    m_shadowShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_shadows.emplace(cfg.Shadows ? cfg.ShadowResolution : 512, cfg.ShadowCascades); // разрешение и каскады из конфига
    m_sky.emplace();
    m_particles.emplace();

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
    if (m_particles) m_scripts->BindParticles(*m_particles); // паритет с Play редактора

    // Ввод — ДО привязки скриптов: раскладку объявляет сам скрипт в OnStart
    // (BindAction), а OnStart вызывается прямо из AttachScript. Привяжи мы ввод
    // после, первые же BindAction упали бы с «ввод не привязан».
    m_input.Attach(app.GetWindow());
    m_rawInput = std::make_unique<WindowRawInput>(m_input, app.GetWindow());
    m_scripts->BindInput(m_input.Actions());
    m_scripts->BindRawInput(*m_rawInput);

    // Звук игры. Отсутствие звукового устройства (CI, headless) — не повод
    // ронять игру: AudioEngine сам работает вхолостую, а PlaySound из Lua
    // остаётся вызываемым.
    m_audio = std::make_unique<AudioEngine>();
    m_scripts->BindAudio(*m_audio);

    // Модули Lua: require "voxel" найдёт assets/scripts/voxel.lua проекта.
    // CWD уже внутри проекта, поэтому путь относительный — как и у скриптов
    // сущностей в .sage.
    m_scripts->AddScriptSearchPath("assets/scripts");

    // Параметры запуска (--autopilot=1, SAGE_GAME_ARGS) — до AttachScript:
    // скрипт читает их уже в OnStart, выбирая режим (автопрогон, зерно мира).
    if (!m_launchArgs.empty()) {
        m_scripts->SetLaunchArgsFromString(m_launchArgs);
        LOG_INFO("Player") << "Параметры запуска игры: " << m_launchArgs;
    }

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

    // Физика: строим мир по сущностям с RigidBodyComponent (бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный Simple). Игра всегда «в Play».
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты рулят физикой времени выполнения (SetVelocity/SetGravity) — доступно
    // после построения мира (RuntimeBody сущностей уже созданы).
    m_scripts->BindPhysics(*m_physics);

    // Запасная камера, если в сцене НЕТ Primary-камеры. НАРОЧНО отличается от
    // редакторской орбитальной камеры (та — {6.5,5,6.5}, yaw -135, pitch -28):
    // низкий фронтальный ракурс, чтобы «нет камеры» сразу читалось как аварийный
    // вид, а не как настоящая игровая камера (иначе игрок думал бы, что игра
    // показывает вьюпорт редактора).
    m_fallbackCamera.Position = {0.0f, 1.4f, 9.0f};
    m_fallbackCamera.Yaw = -90.0f;   // строго вдоль -Z, на сцену
    m_fallbackCamera.Pitch = -6.0f;
    m_fallbackCamera.ProcessMouse(0.0f, 0.0f);

    LOG_INFO("Player") << "PLAYER: started '" << m_projectName << "', scene "
                       << scenePath.filename().string() << " (" << m_scene->Count()
                       << " entities, " << attached << " scripts, "
                       << m_physics->BodyCount() << " physics bodies on "
                       << m_physics->BackendName() << ")";
}

void PlayerLayer::OnDetach() {
    m_physics.reset();
    m_scripts.reset();
    ResourceManager::Instance().Clear();
}

void PlayerLayer::OnUpdate(float dt) {
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    // Ввод опрашиваем ПЕРВЫМ делом в кадре: скрипты ниже читают именно этот
    // снимок (действия + смещение мыши), и он должен быть одним на весь кадр.
    m_input.Update(window.Handle());

    m_scripts->UpdateAll(dt);
    if (m_physics) m_physics->Step(*m_scene, dt);
    sage::anim::UpdateAnimators(*m_scene, dt);
    if (m_particles) sage::fx::UpdateEmitters(*m_scene, *m_particles, dt);
    if (m_audio) m_audio->Update();

    // ESC: сперва ОТПУСКАЕТ курсор, и только потом закрывает игру. В игре от
    // первого лица курсор захвачен — выйти из неё, не вернув курсор, значит
    // оставить игрока без мыши на рабочем столе; а мгновенный выход по первому
    // же ESC не даёт даже посмотреть на мир без прицела.
    if (glfwGetKey(window.Handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        if (window.CursorCaptured()) {
            window.SetCursorCaptured(false);
            m_escLatched = true;
        } else if (!m_escLatched) {
            app.Close();
        }
    } else {
        m_escLatched = false;
    }
}

void PlayerLayer::OnRender() {
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();

    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    // Флаги качества из конфига переопределяют настройки сцены.
    if (!cfg.Skybox) env.Skybox.Enabled = false;
    if (!cfg.Fog) env.Fog.Enabled = false;

    // --- Соотношение сторон: letterbox-viewport по центру окна (или весь экран) ---
    int vpX, vpY, vpW, vpH;
    cfg.LetterboxViewport(window.Width(), window.Height(), vpX, vpY, vpW, vpH);

    // --- Камера: Primary-CameraComponent сцены либо запасной вид ---
    // Кадр берём ТЕМ ЖЕ хелпером, что и панель Game редактора — вид в собранной
    // игре побайтово совпадает с превью (никакой «фальшивости» превью↔игра).
    glm::mat4 view, proj;
    glm::vec3 viewPos;
    float aspect = (float)vpW / (float)std::max(vpH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(*m_scene, aspect);
    if (frame.HasPrimary) {
        view = frame.View;
        proj = frame.Proj;
        viewPos = frame.Position;
    } else {
        // Явно предупреждаем (один раз): без Primary-камеры игра показывает
        // ЗАПАСНОЙ вид — и он НАРОЧНО не совпадает с редакторской камерой, чтобы
        // «нет камеры» нельзя было спутать с нормальным игровым видом.
        if (!m_warnedNoCamera) {
            LOG_WARN("Player") << "В сцене нет Primary-камеры (CameraComponent) — показываю "
                                  "запасной вид. Добавьте камеру: Entity > Create Camera в редакторе.";
            m_warnedNoCamera = true;
        }
        view = m_fallbackCamera.GetViewMatrix();
        proj = m_fallbackCamera.GetProjectionMatrix(aspect);
        viewPos = m_fallbackCamera.Position;
    }

    // Слушатель звука — на игровой камере: 3D-звуки (PlaySound3D из Lua) должны
    // приходить с той стороны, куда игрок реально смотрит. Базис берём из
    // матрицы вида по той же причине, что и ниже для теней — камера может быть
    // как компонентом сцены, так и запасной.
    if (m_audio) {
        const glm::mat3 basis = glm::mat3(view);
        glm::vec3 forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
        glm::vec3 up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
        m_audio->SetListener(viewPos, forward, up);
    }

    // --- Тени: глубина от солнца (можно отключить в настройках) ---
    // Строго ПОСЛЕ выбора камеры: каскады делят дальность именно её взгляда, и
    // без неё считать их не из чего. Порядок «тени, потом камера» держался
    // только на том, что одной карте камера была не нужна.
    if (cfg.Shadows) {
        if (m_shadows->CascadeCount() > 1) {
            ShadowMap::CameraView v;
            v.Position = viewPos;
            // Направление и «верх» достаём из матрицы вида: камера здесь может
            // быть как компонентом сцены, так и запасной, и общего объекта
            // Camera у них нет.
            const glm::mat3 basis = glm::mat3(view);
            v.Forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
            v.Up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
            // Угол обзора и ближнюю плоскость восстанавливаем из матрицы
            // проекции — по той же причине.
            v.FovY = 2.0f * std::atan(1.0f / proj[1][1]);
            v.Aspect = aspect;
            v.Near = proj[3][2] / (proj[2][2] - 1.0f);
            v.ShadowDistance = cfg.ShadowDistance;
            m_shadows->SetCascades(env.Sun.Direction, v);
        } else {
            m_shadows->SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 24.0f);
        }
        sage::render::RenderShadowDepth(*m_shadows, *m_scene, m_batch, window.Width(),
                                        window.Height());
    }

    // --- Основной проход: полное освещение + тени + туман/скайбокс ---
    // Полосы letterbox чёрные: сперва чистим весь экран, затем рендерим в
    // центральный viewport нужного соотношения.
    if (vpX != 0 || vpY != 0) {
        device.SetViewport(0, 0, window.Width(), window.Height());
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear();
    }
    device.SetViewport(vpX, vpY, vpW, vpH);
    device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
    device.Clear();

    // Сцена рендерится напрямую в экран (HDR-пост-цепочки у рантайма нет) —
    // включаем аппаратную гамма-коррекцию, иначе линейный цвет шейдеров уходит
    // на монитор сырым и вся игра выглядит неоправданно тёмной.
    device.SetSRGBWrite(true);

    if (env.Skybox.Enabled) {
        m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    }

    // Статика — через RenderBatch: отсечение по фрустуму + инстансный батчинг.
    sage::render::SceneColorInput color;
    color.View = view;
    color.Proj = proj;
    color.ViewPos = viewPos;
    color.Env = &env;
    color.Shadows = ShadowBinding(*m_shadows, cfg.Shadows);
    color.OcclusionCulling = cfg.OcclusionCulling;
    sage::render::RenderSceneColor(*m_scene, m_batch, color);

    // Частицы (billboard) — camRight/Up берём из матрицы вида.
    if (m_particles) m_particles->DrawFromView(view, proj);

    device.SetSRGBWrite(false); // всё после сцены (UI/оверлеи) — уже в sRGB

    // UI сцены (UIElementComponent из .sage): худ/меню, собранные в редакторе.
    // Рисуется в letterbox-viewport с его размерами — якоря совпадают с панелью
    // Game редактора (WYSIWYG).
    auto uiView = m_scene->Registry().view<UIElementComponent>();
    if (uiView.begin() != uiView.end()) {
        if (!m_ui) m_ui = std::make_unique<UIRenderer>();
        m_ui->Begin(vpW, vpH);
        sage::ui::DrawSceneUI(*m_scene, *m_ui, vpW, vpH);
        m_ui->End();
    }

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}
