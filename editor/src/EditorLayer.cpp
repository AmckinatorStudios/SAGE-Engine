#include "EditorLayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "sage/core/Application.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

namespace fs = std::filesystem;

namespace {

// Пересечение луча с AABB [-0.5,0.5]^3 (единичный куб движка) в локальном
// пространстве объекта. Возвращает t входа (>=0) или отрицательное при промахе.
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (glm::vec3(-0.5f) - ro) * inv;
    glm::vec3 t1 = (glm::vec3(0.5f) - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

constexpr float kStatusBarHeight = 26.0f;
constexpr float kToolbarHeight = 34.0f;

} // namespace

// ============================================================================
//  Жизненный цикл
// ============================================================================

void EditorLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // --- ImGui: docking + multi-viewport (панели можно вытаскивать в
    // отдельные OS-окна — «плавающие» панели становятся полноценными окнами) ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = "sage_editor_imgui.ini"; // своё имя, чтобы не пересекаться с другими ImGui-приложениями
    EditorTheme::LoadFont();
    EditorTheme::Apply();
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;

    // --- Console первой: сток лога ловит все сообщения запуска ---
    m_console.Attach();

    // --- Ресурсы превью: полноценное освещение (ambient+sun+point+тени) ---
    m_shader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    m_shadowShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_shadows.emplace(2048);
    m_sceneFbo.emplace(m_viewportW, m_viewportH);
    m_gameFbo.emplace(m_gameW, m_gameH);
    m_debugDraw.emplace();
    m_sky.emplace();
    m_cube = ResourceManager::Instance().GetCube();

    m_gizmoOp = (int)ImGuizmo::TRANSLATE; // дефолтный режим гизмо (default 0 невалиден)

    NewScene(/*withDemoContent=*/true);

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Дефолтная папка диалогов — рядом с бинарником; сборка игр — в dist/.
    std::snprintf(m_dlgProjectDir, sizeof(m_dlgProjectDir), "%s", fs::current_path().string().c_str());
    std::snprintf(m_dlgBuildDir, sizeof(m_dlgBuildDir), "%s",
                  (fs::current_path() / "dist").string().c_str());
    m_assetsCwd = fs::current_path();

    m_recent.Load();

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) {
        m_autoScreenshotFrame = std::atoi(f);
        m_launcher.Dismiss(); // headless-скриншот — hub проектов не нужен, показываем сцену
    }
    // Начальный режим рендера (для headless-скриншотов/CI): shaded|wireframe|unlit|normals.
    if (const char* m = std::getenv("SAGE_EDITOR_RENDER_MODE")) {
        std::string mode = m;
        if (mode == "wireframe") m_renderMode = EditorRenderMode::Wireframe;
        else if (mode == "unlit") m_renderMode = EditorRenderMode::Unlit;
        else if (mode == "normals") m_renderMode = EditorRenderMode::Normals;
    }

    LOG_INFO("Editor") << "SAGE Editor started (entities: " << m_scene->Count() << ")";

    // --- Плагины (v1, только редактор — см. PluginAPI.h) ---
    fs::path pluginsDir = fs::current_path() / "plugins";
    if (const char* dir = std::getenv("SAGE_PLUGINS_DIR")) pluginsDir = dir;
    m_plugins.LoadAll(pluginsDir, m_pluginCtx);

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();

    // Открыть окно Settings при старте (для скриншот-проверки/демо настроек).
    if (std::getenv("SAGE_EDITOR_SHOW_SETTINGS")) { m_launcher.Dismiss(); m_showSettings = true; }

    // Авто-вход в Play при старте (визуальная проверка/CI): вешает spin.lua на
    // Green Cube демо-сцены и нажимает Play — на скриншоте куб будет повёрнут,
    // а в тулбаре гореть PLAYING. Launcher в этом режиме не показываем.
    if (std::getenv("SAGE_EDITOR_AUTOPLAY")) {
        m_launcher.Dismiss(); // headless-прогон — hub не должен закрывать кадр
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            m_selectedId = green.Id();
        }
        StartPlay();
    }

    UpdateWindowTitle();
}

void EditorLayer::OnDetach() {
    m_console.Detach();    // сток ссылается на панель — снять до разрушения
    m_plugins.UnloadAll(); // ДО разрушения ImGui-контекста — плагины рисуют через тот же ImGui
    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    ResourceManager::Instance().Clear();
}

void EditorLayer::OnUpdate(float dt) {
    // Логика правки — событийная, живёт в панелях. Единственный
    // "симуляционный" тик — Play: скрипты сущностей, пока не пауза.
    if (m_playState == EditorPlayState::Playing) {
        if (m_playScripts) m_playScripts->UpdateAll(dt);
        if (m_playPhysics) m_playPhysics->Step(*m_scene, dt);
    }
    // Анимации проигрываются и в режиме правки — чтобы в вьюпорте было видно
    // движение скелетных моделей (превью), не только в Play.
    sage::anim::UpdateAnimators(*m_scene, dt);
    m_plugins.UpdateAll(dt);
}

// ============================================================================
//  Плагины редактора — реализация facade'а EditorPluginContext
// ============================================================================

void EditorLayer::PluginContextImpl::Log(const char* message) {
    LOG_INFO("Plugin") << message;
}

const char* EditorLayer::PluginContextImpl::SelectedEntityName() const {
    GameObject obj = m_owner.m_scene->Get(m_owner.m_selectedId);
    m_selectedNameBuf = obj.Valid() ? obj.Name() : "";
    return m_selectedNameBuf.c_str();
}

void EditorLayer::PluginContextImpl::SetStatusMessage(const char* message) {
    m_owner.m_pluginStatusMessage = message ? message : "";
}

// ============================================================================
//  Play-режим
// ============================================================================

void EditorLayer::StartPlay() {
    if (InPlayMode()) return;

    // Снапшот сцены — Stop вернёт всё ровно как было до Play.
    m_playSnapshot = SceneSerializer::SaveToString(*m_scene);

    m_playScripts = std::make_unique<ScriptEngine>();
    m_playScripts->BindScene(*m_scene);

    // Привязываем скрипты всех сущностей со ScriptComponent. Ошибка в одном
    // скрипте (нет файла, синтаксис) не срывает Play — логируется, остальные
    // продолжают работать.
    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent, IdComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        try {
            m_playScripts->AttachScript(GameObject(&m_scene->Registry(), e), path);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Editor") << "Play: script attach failed: " << ex.what();
        }
    }

    // Физика: строим мир по сущностям с RigidBodyComponent. Бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный Simple (см. PhysicsWorld::DefaultBackend).
    m_playPhysics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    m_playState = EditorPlayState::Playing;
    m_game.RequestFocus(); // «игровое окно» выходит на передний план при запуске
    LOG_INFO("Editor") << "Play started (" << attached << " script(s), "
                       << m_playPhysics->BodyCount() << " physics body(ies) on "
                       << m_playPhysics->BackendName() << ")";
}

void EditorLayer::StopPlay() {
    if (!InPlayMode()) return;

    // Порядок важен: ScriptEngine держит указатель на текущую сцену — гасим
    // его ДО того, как заменить сцену восстановленным снапшотом.
    m_playScripts.reset();
    m_playPhysics.reset();
    RestoreSceneFromString(m_playSnapshot);
    m_playSnapshot.clear();
    m_playState = EditorPlayState::Editing;
    LOG_INFO("Editor") << "Play stopped, scene restored";
}

// ============================================================================
//  Undo/Redo (снапшот-модель) + dirty-маркер
// ============================================================================

bool EditorLayer::RestoreSceneFromString(const std::string& snapshot) {
    try {
        m_scene = SceneSerializer::LoadFromString(snapshot);
        // Выбор хранится как id, а сериализатор сохраняет id — выбор переживает
        // откат, если сущность существует в снапшоте (иначе Get() даст invalid).
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene restore failed: " << e.what();
        return false;
    }
}

void EditorLayer::PushUndoSnapshot() {
    if (InPlayMode()) return; // правки в Play эфемерны — Stop их и так откатит
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    m_redoStack.clear(); // новая мутация обрывает redo-ветку
    m_sceneDirty = true;
    UpdateWindowTitle();
}

void EditorLayer::CapturePendingSnapshot() {
    if (InPlayMode()) return;
    m_pendingEditSnapshot = SceneSerializer::SaveToString(*m_scene);
}

void EditorLayer::CommitPendingSnapshot() {
    if (InPlayMode() || m_pendingEditSnapshot.empty()) return;
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) m_undoStack.erase(m_undoStack.begin());
    m_undoStack.push_back(m_pendingEditSnapshot);
    m_pendingEditSnapshot.clear();
    m_redoStack.clear();
    m_sceneDirty = true;
    UpdateWindowTitle();
}

// Одна запись undo на всё перетаскивание DragFloat/набор текста: состояние
// «до» запоминается на активации виджета, в стек уходит на завершении правки.
void EditorLayer::TrackLastImGuiItem() {
    if (InPlayMode()) return;
    if (ImGui::IsItemActivated()) CapturePendingSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitPendingSnapshot();
}

void EditorLayer::Undo() {
    if (InPlayMode() || m_undoStack.empty()) return;
    m_redoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_undoStack.back())) {
        m_undoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_redoStack.pop_back(); // откат не удался — не ломаем историю
    }
}

void EditorLayer::Redo() {
    if (InPlayMode() || m_redoStack.empty()) return;
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_redoStack.back())) {
        m_redoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_undoStack.pop_back();
    }
}

// ============================================================================
//  Сущности
// ============================================================================

GameObject EditorLayer::CreateCubeEntity(const std::string& name) {
    return CreatePrimitiveEntity(name, MeshRef::Type::Cube);
}

GameObject EditorLayer::CreatePrimitiveEntity(const std::string& name, MeshRef::Type type) {
    GameObject obj = m_scene->CreateObject(name);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{type, ""};
    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(type);
    return obj;
}

void EditorLayer::DuplicateSelected() {
    GameObject src = m_scene->Get(m_selectedId);
    if (!src.Valid()) return;
    PushUndoSnapshot();
    GameObject copy = m_scene->CreateObject(src.Name() + " Copy");
    copy.GetTransform() = src.GetTransform();
    copy.GetTransform().Position.x += 0.5f; // сдвиг, чтобы копия не сливалась с оригиналом
    MeshRendererComponent& mr = copy.Renderer();
    mr = src.Renderer();
    if (const ScriptComponent* sc = src.Registry()->try_get<ScriptComponent>(src.Entity())) {
        copy.Registry()->emplace<ScriptComponent>(copy.Entity(), *sc);
    }
    if (const CameraComponent* cam = src.Registry()->try_get<CameraComponent>(src.Entity())) {
        copy.Registry()->emplace<CameraComponent>(copy.Entity(), *cam);
    }
    m_selectedId = copy.Id();
}

void EditorLayer::DeleteSelected() {
    if (!m_scene->Get(m_selectedId).Valid()) return;
    PushUndoSnapshot();
    m_scene->RemoveObject(m_selectedId);
    m_selectedId = -1;
}

// ============================================================================
//  Сцена / проект
// ============================================================================

void EditorLayer::NewScene(bool withDemoContent) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_undoStack.clear();
    m_redoStack.clear();
    m_scene = std::make_unique<Scene>("Untitled");
    m_selectedId = -1;
    m_scenePath.clear();
    m_sceneDirty = false;

    if (withDemoContent) {
        // Скайбокс включён по умолчанию — сцена сразу с атмосферным фоном.
        m_scene->Lighting.Skybox.Enabled = true;

        struct Def { const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale; };
        Def defs[] = {
            {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}},
            {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}},
            {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}},
            {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}},
            {"Tower",      {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}},
        };
        for (const Def& d : defs) {
            GameObject obj = CreateCubeEntity(d.name);
            obj.GetTransform().Position = d.pos;
            obj.GetTransform().Scale = d.scale;
            obj.Renderer().Color = d.color;
        }

        // Игровая камера сцены — панель Game сразу показывает картинку, а не
        // подсказку «нет камеры»; сущность без меша (не рисуется в мире).
        GameObject camObj = m_scene->CreateObject("Main Camera");
        camObj.GetTransform().Position = {5.0f, 4.5f, 5.0f};
        camObj.GetTransform().Rotation = {-25.0f, 45.0f, 0.0f};
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());

        // Тёплая лампа — демонстрация точечного света-сущности (LightComponent).
        GameObject lamp = m_scene->CreateObject("Lamp");
        lamp.GetTransform().Position = {2.4f, 1.6f, 1.8f};
        m_scene->Registry().emplace<LightComponent>(lamp.Entity());

        // Прожектор сверху — демонстрация конусного света (Spot): смотрит вниз
        // (поворот -90° по X направляет «вперёд» -Z в -Y), кладёт круг света
        // на кубы и пол.
        GameObject spot = m_scene->CreateObject("Spotlight");
        spot.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        spot.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f};
        LightComponent spotLc;
        spotLc.Kind = LightComponent::Type::Spot;
        spotLc.Color = {0.55f, 0.7f, 1.0f};
        spotLc.Intensity = 4.0f;
        spotLc.Range = 12.0f;
        spotLc.InnerConeDeg = 18.0f;
        spotLc.OuterConeDeg = 30.0f;
        m_scene->Registry().emplace<LightComponent>(spot.Entity(), spotLc);

        // Что-то выбрано сразу — гизмо видно, Inspector не пустой.
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) m_selectedId = green.Id();
    }
    UpdateWindowTitle();
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_undoStack.clear();
        m_redoStack.clear();
        m_selectedId = -1;
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene loaded: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene load failed: " << e.what();
        return false;
    }
}

bool EditorLayer::SaveSceneToFile(const fs::path& path) {
    try {
        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        SceneSerializer::Save(*m_scene, path.string());
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene saved: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene save failed: " << e.what();
        return false;
    }
}

bool EditorLayer::CreateProject(const std::string& dir, const std::string& name, std::string& err) {
    if (!m_project.CreateNew(dir, name, err)) return false;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());
    NewScene(/*withDemoContent=*/true);
    UpdateWindowTitle();
    return true;
}

bool EditorLayer::OpenProject(const std::string& path, std::string& err) {
    if (!m_project.Open(path, err)) return false;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());

    // Настройки проекта (sage.cfg) — в окно Settings; отсутствие файла не ошибка
    // (остаются значения по умолчанию).
    m_settings = sage::EngineConfig{};
    m_settings.LoadFile((m_project.Dir() / "sage.cfg").string());

    // Автозагрузка первой сцены проекта (по алфавиту) — открытый проект сразу
    // показывает свой контент, а не осиротевшую демо-сцену.
    std::error_code ec;
    std::vector<fs::path> scenes;
    for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
        if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
    }
    std::sort(scenes.begin(), scenes.end());
    if (!scenes.empty()) LoadSceneFromFile(scenes.front());

    UpdateWindowTitle();
    return true;
}

// ============================================================================
//  Сборка игры: SagePlayer + рантайм-ассеты + project/ => запускаемая папка
// ============================================================================

bool EditorLayer::BuildGame(const fs::path& outputDir, std::string& err) {
    if (!m_project.Loaded()) {
        err = "No project open";
        return false;
    }

    // 1. Собранный SagePlayer: явный SAGE_PLAYER_PATH, иначе стандартные
    // места относительно редактора (../runtime в дереве сборки, рядом с exe).
#ifdef _WIN32
    const char* playerName = "SagePlayer.exe";
    std::string exeSuffix = ".exe";
#else
    const char* playerName = "SagePlayer";
    std::string exeSuffix;
#endif
    std::vector<fs::path> candidates;
    if (const char* p = std::getenv("SAGE_PLAYER_PATH")) candidates.push_back(p);
    candidates.push_back(fs::path("..") / "runtime" / playerName);
    candidates.push_back(fs::path(".") / playerName);

    std::error_code ec;
    fs::path player;
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate, ec)) { player = candidate; break; }
    }
    if (player.empty()) {
        err = "SagePlayer not found (build the SagePlayer target or set SAGE_PLAYER_PATH)";
        return false;
    }

    // 2. Слепить папку игры: <out>/<Name>/{<Name>, assets/(рантайм), project/}.
    fs::path gameDir = outputDir / m_project.Name();
    fs::remove_all(gameDir, ec); // пересборка затирает прошлую (это артефакт, не данные)
    fs::create_directories(gameDir, ec);
    if (ec) {
        err = "Cannot create " + gameDir.string() + ": " + ec.message();
        return false;
    }

    fs::copy_file(player, gameDir / (m_project.Name() + exeSuffix),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "Player copy failed: " + ec.message();
        return false;
    }
    fs::copy(player.parent_path() / "assets", gameDir / "assets",
             fs::copy_options::recursive, ec);
    if (ec) {
        err = "Runtime assets copy failed: " + ec.message();
        return false;
    }
    fs::copy(m_project.Dir(), gameDir / "project", fs::copy_options::recursive, ec);
    if (ec) {
        err = "Project copy failed: " + ec.message();
        return false;
    }

    // Настройки проекта — рядом с exe игры (sage.cfg), чтобы игрок мог править их
    // без залезания в project/. SagePlayer грузит и этот, и project/sage.cfg.
    std::error_code cfgEc;
    fs::path projCfg = m_project.Dir() / "sage.cfg";
    if (fs::exists(projCfg, cfgEc)) {
        fs::copy_file(projCfg, gameDir / "sage.cfg", fs::copy_options::overwrite_existing, cfgEc);
    }

    LOG_INFO("Editor") << "Game built: " << gameDir.string();
    return true;
}

// Заголовок OS-окна: "SAGE Editor — сцена[*] — проект". Обновляется только
// по факту изменения (не дёргаем GLFW каждый кадр).
void EditorLayer::UpdateWindowTitle() {
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    std::string title = "SAGE Editor — " + scene + (m_sceneDirty ? "*" : "");
    if (m_project.Loaded()) title += " — " + m_project.Name();
    if (title == m_windowTitle) return;
    m_windowTitle = title;
    glfwSetWindowTitle(sage::Application::Get().GetWindow().Handle(), title.c_str());
}

// ============================================================================
//  Пикинг из вьюпорта
// ============================================================================

void EditorLayer::PickAtViewport(float u, float v) {
    // Луч из камеры через пиксель вьюпорта: unprojection ближней/дальней точек NDC.
    glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    glm::mat4 invVP = glm::inverse(m_proj * m_view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec3 ro = glm::vec3(p0) / p0.w;
    glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    int bestId = -1;
    float bestDist = 1e30f;
    auto view = m_scene->Registry().view<IdComponent, Transform, MeshRendererComponent>();
    for (auto e : view) {
        if (!view.get<MeshRendererComponent>(e).MeshPtr) continue;
        glm::mat4 inv = glm::inverse(view.get<Transform>(e).GetMatrix());
        glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f)); // без нормализации: t остаётся в масштабе мира
        float t = RayUnitCube(lro, lrd);
        if (t >= 0.0f && t < bestDist) {
            bestDist = t;
            bestId = view.get<IdComponent>(e).Id;
        }
    }

    // Невидимые сущности (камера/свет) кликабельны по маленькому боксу вокруг
    // их позиции — иначе их гизмо не выбрать (меша нет).
    auto pickMarker = [&](entt::entity e, int id, const glm::vec3& pos) {
        glm::mat4 boxInv = glm::inverse(glm::translate(glm::mat4(1.0f), pos) *
                                        glm::scale(glm::mat4(1.0f), glm::vec3(0.6f)));
        glm::vec3 lro = glm::vec3(boxInv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(boxInv * glm::vec4(rd, 0.0f));
        float t = RayUnitCube(lro, lrd);
        if (t >= 0.0f && t < bestDist) { bestDist = t; bestId = id; }
    };
    auto camMarkers = m_scene->Registry().view<CameraComponent, Transform, IdComponent>();
    for (auto e : camMarkers)
        pickMarker(e, camMarkers.get<IdComponent>(e).Id, camMarkers.get<Transform>(e).Position);
    auto lightMarkers = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightMarkers)
        pickMarker(e, lightMarkers.get<IdComponent>(e).Id, lightMarkers.get<Transform>(e).Position);

    m_selectedId = bestId; // клик мимо всех объектов — снять выбор
}

// ============================================================================
//  Рендер превью: тени -> Viewport (редакторская камера) -> Game (камера сцены)
// ============================================================================

// Общая карта теней кадра: солнце одно, и Viewport, и Game сэмплируют один
// depth-проход. Центр — на начале координат (сцены редактора компактны).
void EditorLayer::RenderShadowPass(const LightingEnvironment& env) {
    Window& window = sage::Application::Get().GetWindow();
    m_shadows->SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 24.0f);
    m_shadows->BeginRender();
    m_shadowShader->Use();
    m_shadowShader->SetMat4("uLightSpace", m_shadows->LightMatrix());
    sage::ecs::ForEachRenderable(*m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shadowShader->SetMat4("uModel", tr.GetMatrix());
        mr.MeshPtr->Draw();
    });
    m_shadows->EndRender(window.Width(), window.Height());
}

// Освещённый проход сцены в текущий привязанный FBO (общая часть Viewport/Game).
// shadingMode/wireframe задают режим рендера (Viewport — из тулбара, Game —
// всегда Shaded, чтобы игровое окно выглядело как финальная картинка).
void EditorLayer::DrawLitScene(const LightingEnvironment& env, const glm::mat4& view,
                               const glm::mat4& proj, glm::vec3 viewPos,
                               int shadingMode, bool wireframe) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);

    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);
    m_shader->SetVec3("uViewPos", viewPos);
    UploadLighting(*m_shader, env);
    device.BindTexture2D(1, m_shadows->DepthTexture());
    UploadShadowUniforms(*m_shader, m_shadows->LightMatrix(), /*unit=*/1, /*enabled=*/true);
    m_shader->SetInt("uUseTexture", 0);
    m_shader->SetInt("uShadingMode", shadingMode); // 0 lit, 1 unlit, 2 normals
    sage::ecs::ForEachRenderable(*m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shader->SetMat4("uModel", tr.GetMatrix());
        m_shader->SetVec3("uObjectColor", EffectiveColor(mr)); // материал (если назначен) важнее Color
        mr.MeshPtr->Draw();
    });

    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill); // вернуть заливку
}

// Аутлайн выбранного меша: масштабированная «оболочка» плоским цветом с
// отсечением ЛИЦЕВЫХ граней (видны только задние — они образуют кайму вокруг
// объекта). Асимметрично к mesh: работает для выпуклых примитивов/моделей.
void EditorLayer::DrawSelectionOutline(GameObject obj, const glm::mat4& view, const glm::mat4& proj) {
    const MeshRendererComponent* mr = m_scene->Registry().try_get<MeshRendererComponent>(obj.Entity());
    if (!mr || !mr->MeshPtr) return;
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    glm::mat4 model = glm::scale(obj.GetTransform().GetMatrix(), glm::vec3(1.06f));
    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);
    m_shader->SetMat4("uModel", model);
    m_shader->SetInt("uShadingMode", 1); // unlit — плоский цвет каймы
    m_shader->SetInt("uUseTexture", 0);
    m_shader->SetInt("uFogEnabled", 0);
    m_shader->SetVec3("uObjectColor", {1.0f, 0.62f, 0.12f});

    device.SetCullMode(sage::rhi::CullMode::Front); // рисуем только задние грани оболочки
    mr->MeshPtr->Draw();
    device.SetCullMode(sage::rhi::CullMode::Back);
}

// Гизмо невидимых сущностей (камера/свет) — всегда видны в редакторе, чтобы их
// можно было найти и выбрать. Рисуются через DebugDraw (накопление, Flush —
// у вызывающего). Выбранная сущность подсвечивается ярче.
void EditorLayer::DrawEntityGizmos() {
    // Камеры: каркас усечённой пирамиды (frustum) в масштабе near..~2.5.
    auto camView = m_scene->Registry().view<CameraComponent, Transform, IdComponent>();
    for (auto e : camView) {
        const Transform& tr = camView.get<Transform>(e);
        bool selected = camView.get<IdComponent>(e).Id == m_selectedId;
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.8f, 0.2f) : glm::vec3(0.5f, 0.7f, 0.9f);
        glm::vec3 fwd = sage::ecs::ForwardFromEuler(tr.Rotation);
        const CameraComponent& cam = camView.get<CameraComponent>(e);
        m_debugDraw->WireFrustum(tr.Position, fwd, cam.Fov,
                                 (float)m_gameW / (float)std::max(m_gameH, 1), 0.3f, 2.2f, color);
    }
    // Свет-сущности: маленький маркер (сфера) в позиции — всегда, даже если не
    // выбраны (у выбранного зона действия рисуется отдельно, крупнее).
    auto lightView = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightView) {
        if (lightView.get<IdComponent>(e).Id == m_selectedId) continue; // выбранный — крупная зона ниже
        const LightComponent& lc = lightView.get<LightComponent>(e);
        m_debugDraw->WireSphere(lightView.get<Transform>(e).Position, 0.25f, glm::vec3(lc.Color) * 0.9f, 10);
    }
    // Коллайдеры: каркас формы в масштабе Transform — зелёный для выбранной
    // сущности, приглушённый для остальных (чтобы видеть все физические тела).
    auto colView = m_scene->Registry().view<ColliderComponent, Transform, IdComponent>();
    for (auto e : colView) {
        const Transform& tr = colView.get<Transform>(e);
        const ColliderComponent& col = colView.get<ColliderComponent>(e);
        bool selected = colView.get<IdComponent>(e).Id == m_selectedId;
        glm::vec3 color = selected ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(0.25f, 0.55f, 0.30f);
        glm::vec3 scale = glm::abs(tr.Scale);
        switch (col.Shape) {
            case sage::physics::ShapeType::Box:
                m_debugDraw->WireBox(tr.Position, col.HalfExtents * scale, color);
                break;
            case sage::physics::ShapeType::Sphere:
                m_debugDraw->WireSphere(tr.Position,
                    col.Radius * glm::max(scale.x, glm::max(scale.y, scale.z)), color);
                break;
            case sage::physics::ShapeType::Capsule:
                // Каркас как коробка по габаритам капсулы (примитива капсулы в
                // DebugDraw нет — коробка радиус×высота даёт понятный габарит).
                m_debugDraw->WireBox(tr.Position,
                    glm::vec3(col.Radius * scale.x,
                              (col.HalfHeight + col.Radius) * scale.y,
                              col.Radius * scale.z), color);
                break;
        }
    }
}

void EditorLayer::RenderSceneToFramebuffer(const LightingEnvironment& env) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    m_sceneFbo->Resize(m_viewportW, m_viewportH);
    m_sceneFbo->Bind();
    device.SetClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    device.Clear();

    m_view = m_camera.GetViewMatrix();
    m_proj = m_camera.GetProjectionMatrix((float)m_viewportW / (float)std::max(m_viewportH, 1));

    // Скайбокс (если включён) — фон до сцены.
    if (env.Skybox.Enabled) {
        m_sky->Draw(m_view, m_proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    }

    // Режим рендера из тулбара: Shaded(0)/Unlit(1)/Normals(2), Wireframe —
    // unlit + полигоны линиями.
    int shadingMode = 0;
    bool wireframe = false;
    switch (m_renderMode) {
        case EditorRenderMode::Shaded:    shadingMode = 0; break;
        case EditorRenderMode::Wireframe: shadingMode = 1; wireframe = true; break;
        case EditorRenderMode::Unlit:     shadingMode = 1; break;
        case EditorRenderMode::Normals:   shadingMode = 2; break;
    }
    DrawLitScene(env, m_view, m_proj, m_camera.Position, shadingMode, wireframe);

    // Скелетно-анимированные модели (свой скиннинг-шейдер) — в тот же буфер.
    sage::anim::DrawAnimatedModels(*m_scene, m_view, m_proj, env);

    // Аутлайн выбранной сущности (масштабированная оболочка) — поверх сцены,
    // до гизмо-линий.
    GameObject selectedObj = m_scene->Get(m_selectedId);
    if (selectedObj.Valid()) DrawSelectionOutline(selectedObj, m_view, m_proj);

    // Гизмо-графика движка (DebugDraw) — в ТОТ ЖЕ буфер после сцены, с тестом
    // глубины: объекты заслоняют сетку (не 2D-оверлей поверх картинки).
    if (m_showGrid) {
        m_debugDraw->Grid({0.0f, 0.0f, 0.0f}, 12.0f, 1.0f, {0.32f, 0.33f, 0.38f});
    }
    DrawEntityGizmos(); // всегда видимые гизмо камер/светов
    if (selectedObj.Valid()) {
        // Оси выбранной сущности + зона действия света.
        glm::mat4 model = selectedObj.GetTransform().GetMatrix();
        m_debugDraw->Axes(model, 1.4f);
        if (const LightComponent* light =
                m_scene->Registry().try_get<LightComponent>(selectedObj.Entity())) {
            const Transform& lt = selectedObj.GetTransform();
            glm::vec3 lightColor = glm::vec3(light->Color) * 0.9f;
            if (light->Kind == LightComponent::Type::Spot) {
                glm::vec3 dir = sage::ecs::ForwardFromEuler(lt.Rotation);
                m_debugDraw->WireCone(lt.Position, dir, light->Range, light->OuterConeDeg, lightColor);
            } else {
                m_debugDraw->WireSphere(lt.Position, light->Range, lightColor);
            }
        }
    }
    m_debugDraw->Flush(m_view, m_proj);

    device.BindDefaultFramebuffer();
}

bool EditorLayer::HasPrimaryCamera() {
    auto view = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) return true;
    }
    return false;
}

void EditorLayer::RenderGameToFramebuffer(const LightingEnvironment& env) {
    // Первая Primary-камера сцены. Нет камеры — панель Game сама покажет
    // подсказку, кадр не рендерим.
    entt::entity camEntity = entt::null;
    auto camView = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : camView) {
        if (camView.get<CameraComponent>(e).Primary) { camEntity = e; break; }
    }
    if (camEntity == entt::null) return;

    const CameraComponent& cam = camView.get<CameraComponent>(camEntity);
    const Transform& tr = camView.get<Transform>(camEntity);

    // Ориентация из углов Эйлера Transform (порядок XYZ — как в GetMatrix);
    // Scale сущности на камеру не влияет.
    glm::mat4 rot = glm::eulerAngleXYZ(glm::radians(tr.Rotation.x),
                                       glm::radians(tr.Rotation.y),
                                       glm::radians(tr.Rotation.z));
    glm::vec3 fwd = glm::normalize(glm::vec3(rot * glm::vec4(0, 0, -1, 0)));
    glm::vec3 up = glm::normalize(glm::vec3(rot * glm::vec4(0, 1, 0, 0)));
    glm::mat4 view = glm::lookAt(tr.Position, tr.Position + fwd, up);
    float aspect = (float)m_gameW / (float)std::max(m_gameH, 1);
    glm::mat4 proj = glm::perspective(glm::radians(cam.Fov), aspect, cam.NearClip, cam.FarClip);

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    m_gameFbo->Resize(m_gameW, m_gameH);
    m_gameFbo->Bind();
    // Игровое окно очищается цветом неба сцены — как это увидит игрок.
    device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
    device.Clear();

    if (env.Skybox.Enabled) {
        m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    }
    // Игровое окно — всегда полное освещение (Shaded), без гизмо/аутлайна:
    // показывает сцену как её увидит игрок.
    DrawLitScene(env, view, proj, tr.Position, /*shadingMode=*/0, /*wireframe=*/false);
    sage::anim::DrawAnimatedModels(*m_scene, view, proj, env);

    device.BindDefaultFramebuffer();
}

// ============================================================================
//  Кадр UI
// ============================================================================

void EditorLayer::OnRender() {
    sage::Application& app = sage::Application::Get();

    // Итоговое освещение кадра: окружение сцены + света-сущности; один
    // shadow-проход на кадр, Viewport и Game сэмплируют общую карту.
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    RenderShadowPass(env);
    RenderSceneToFramebuffer(env);
    RenderGameToFramebuffer(env);

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    DrawDockspaceAndMenu(); // включая модальные диалоги (см. DrawDialogs внутри)
    m_hierarchy.Draw(*this);
    m_inspector.Draw(*this);
    m_lighting.Draw(*this);
    m_viewport.Draw(*this);
    m_game.Draw(*this);
    m_console.Draw();
    m_assets.Draw(*this);
    m_plugins.ImGuiAll();

    // Стартовый launcher проектов: пока проект не открыт (и не отклонён).
    if ((!m_project.Loaded() && !m_launcher.Dismissed()) || m_launcherRequested) {
        m_launcher.Draw(*this, m_recent);
        if (m_project.Loaded()) m_launcherRequested = false;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Multi-viewport: панели, вытащенные за пределы главного окна, живут в
    // собственных OS-окнах — их нужно обновить и отрисовать отдельно.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        Window& win = app.GetWindow();
        SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
        app.Close();
    }
}

void EditorLayer::BuildDefaultDockLayout(unsigned int dockspaceId) {
    // Пересобираем узлы доккинга с нуля: Viewport+Game в центре (табами),
    // панели вокруг.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.21f, nullptr, &center);
    ImGuiID left  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.23f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Lighting", right);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Game", center);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspaceId);

    // По умолчанию активен таб Viewport (Game выходит вперёд при Play).
    ImGui::SetWindowFocus("Viewport");
}

void EditorLayer::DrawStatusBar(float height) {
    // Строка состояния внизу хост-окна: проект | сцена(+dirty) | сущности |
    // Play-статус | сообщение плагинов | FPS.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
    ImGui::BeginChild("##statusbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();

    ImGui::TextDisabled("%s", m_project.Loaded() ? m_project.Name().c_str() : "No project");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    ImGui::Text("%s%s", scene.c_str(), m_sceneDirty ? "*" : "");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::TextDisabled("Entities: %zu", m_scene->Count());

    if (InPlayMode()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        bool playing = m_playState == EditorPlayState::Playing;
        ImGui::TextColored(playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           playing ? "PLAYING" : "PAUSED");
    }
    if (!m_pluginStatusMessage.empty()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::TextDisabled("%s", m_pluginStatusMessage.c_str());
    }

    // FPS — справа.
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f FPS", sage::Application::Get().Fps());
    float w = ImGui::CalcTextSize(fps).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextDisabled("%s", fps);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void EditorLayer::DrawDockspaceAndMenu() {
    // Полноэкранное окно-хост под dockspace: без рамок/заголовка, на весь
    // рабочий вьюпорт, с menu bar. Стандартный приём из демо ImGui.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##SageEditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Тулбар — горизонтальный бар сразу под меню-баром (инструменты гизмо,
    // Play, режим рендера). Рисуется ДО dockspace, чтобы занять свою полосу.
    m_toolbar.Draw(*this, kToolbarHeight);

    ImGuiID dockspaceId = ImGui::GetID("SageDockSpace");
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        BuildDefaultDockLayout(dockspaceId);
    }
    // Док-пространство занимает всё между тулбаром и статус-баром.
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -kStatusBarHeight), ImGuiDockNodeFlags_None);
    DrawStatusBar(kStatusBarHeight);

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project...")) openDialog = "New Project";
            if (ImGui::MenuItem("Open Project...")) openDialog = "Open Project";
            if (ImGui::MenuItem("Project Launcher...")) m_launcherRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem("New Scene")) NewScene(false);
            if (ImGui::MenuItem("Open Scene...")) openDialog = "Open Scene";

            // Сцены открытого проекта — прямой доступ без файлового диалога.
            if (m_project.Loaded() && ImGui::BeginMenu("Project Scenes")) {
                std::error_code ec;
                std::vector<fs::path> scenes;
                for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
                    if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
                }
                std::sort(scenes.begin(), scenes.end());
                if (scenes.empty()) ImGui::TextDisabled("(no scenes yet)");
                for (const fs::path& scenePath : scenes) {
                    bool current = scenePath == m_scenePath;
                    if (ImGui::MenuItem(scenePath.filename().string().c_str(), nullptr, current)) {
                        LoadSceneFromFile(scenePath);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                if (!m_scenePath.empty()) SaveSceneToFile(m_scenePath);
                else openDialog = "Save Scene As";
            }
            if (ImGui::MenuItem("Save Scene As...")) openDialog = "Save Scene As";
            ImGui::Separator();
            if (ImGui::MenuItem("Build Game...", nullptr, false, m_project.Loaded())) {
                m_dlgBuildResult.clear();
                openDialog = "Build Game";
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) sage::Application::Get().Close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undoStack.empty() && !InPlayMode())) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redoStack.empty() && !InPlayMode())) Redo();
            ImGui::Separator();
            bool hasSel = m_scene->Get(m_selectedId).Valid();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) DuplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Play")) {
            if (ImGui::MenuItem("Play", nullptr, false, m_playState == EditorPlayState::Editing)) StartPlay();
            if (ImGui::MenuItem("Pause", nullptr, false, m_playState == EditorPlayState::Playing)) PausePlay();
            if (ImGui::MenuItem("Resume", nullptr, false, m_playState == EditorPlayState::Paused)) ResumePlay();
            if (ImGui::MenuItem("Stop", nullptr, false, InPlayMode())) StopPlay();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Entity")) {
            if (ImGui::MenuItem("Create Empty")) {
                PushUndoSnapshot();
                m_selectedId = m_scene->CreateObject("Empty").Id();
            }
            if (ImGui::BeginMenu("Create Primitive")) {
                struct { const char* name; MeshRef::Type type; } prims[] = {
                    {"Cube", MeshRef::Type::Cube}, {"Sphere", MeshRef::Type::Sphere},
                    {"Plane", MeshRef::Type::Plane}, {"Cylinder", MeshRef::Type::Cylinder},
                    {"Cone", MeshRef::Type::Cone},
                };
                for (const auto& p : prims) {
                    if (ImGui::MenuItem(p.name)) {
                        PushUndoSnapshot();
                        m_selectedId = CreatePrimitiveEntity(p.name, p.type).Id();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Create Camera")) {
                PushUndoSnapshot();
                GameObject camObj = m_scene->CreateObject("Camera");
                m_scene->Registry().emplace<CameraComponent>(camObj.Entity());
                m_selectedId = camObj.Id();
            }
            if (ImGui::MenuItem("Create Light")) {
                PushUndoSnapshot();
                GameObject lightObj = m_scene->CreateObject("Light");
                lightObj.GetTransform().Position = {0.0f, 2.5f, 0.0f};
                m_scene->Registry().emplace<LightComponent>(lightObj.Entity());
                m_selectedId = lightObj.Id();
            }
            ImGui::Separator();
            // Физический куб: меш + динамическое тело + бокс-коллайдер — падает
            // под гравитацией сразу в Play (быстрый способ проверить физику).
            if (ImGui::MenuItem("Create Physics Cube")) {
                PushUndoSnapshot();
                GameObject box = CreatePrimitiveEntity("Physics Cube", MeshRef::Type::Cube);
                box.GetTransform().Position = {0.0f, 4.0f, 0.0f};
                m_scene->Registry().emplace<RigidBodyComponent>(box.Entity());
                m_scene->Registry().emplace<ColliderComponent>(box.Entity());
                m_selectedId = box.Id();
            }
            // Скелетно-анимированная модель: без пути — процедурный демо-щупалец
            // с клипом «Wave» (сразу проигрывается в вьюпорте).
            if (ImGui::MenuItem("Create Animated Model")) {
                PushUndoSnapshot();
                GameObject anim = m_scene->CreateObject("Animated Model");
                anim.GetTransform().Position = {0.0f, 0.0f, 0.0f};
                m_scene->Registry().emplace<AnimatedModelComponent>(anim.Entity());
                m_selectedId = anim.Id();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Reset Layout")) m_rebuildDockLayout = true;
            ImGui::MenuItem("Show Grid", nullptr, &m_showGrid);
            ImGui::Separator();
            ImGui::MenuItem("Settings...", nullptr, &m_showSettings);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре.
        std::string status = m_project.Loaded() ? ("Project: " + m_project.Name()) : "No project";
        float w = ImGui::CalcTextSize(status.c_str()).x + 16.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status.c_str());

        ImGui::EndMenuBar();
    }

    if (openDialog) {
        m_dlgError.clear();
        ImGui::OpenPopup(openDialog);
    }
    // Модалки рисуются в том же ID-пространстве окна-хоста, где их открыли.
    DrawDialogs();
    DrawSettingsWindow();

    ImGui::End();

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_scenePath.empty()) SaveSceneToFile(m_scenePath);
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) Redo();
    }
}

// ============================================================================
//  Диалоги File-меню (модальные окна)
// ============================================================================

void EditorLayer::DrawDialogs() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", m_dlgProjectName, sizeof(m_dlgProjectName));
        ImGui::InputText("Location", m_dlgProjectDir, sizeof(m_dlgProjectDir));
        ImGui::TextDisabled("Creates <Location>/<Name>/project.sageproj + scenes/ + assets/");
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            std::string err;
            if (CreateProject(m_dlgProjectDir, m_dlgProjectName, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_dlgError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Path", m_dlgOpenPath, sizeof(m_dlgOpenPath));
        ImGui::TextDisabled("Path to project.sageproj or the project folder");
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (ImGui::Button("Open", ImVec2(120, 0))) {
            std::string err;
            if (OpenProject(m_dlgOpenPath, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_dlgError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("File name", m_dlgSceneName, sizeof(m_dlgSceneName));
        fs::path target = m_project.Loaded()
            ? m_project.ScenesDir() / (std::string(m_dlgSceneName) + ".sage")
            : fs::path(std::string(m_dlgSceneName) + ".sage");
        ImGui::TextDisabled("-> %s", target.string().c_str());
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            m_scene->SetName(m_dlgSceneName);
            if (SaveSceneToFile(target)) ImGui::CloseCurrentPopup();
            else m_dlgError = "Save failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Packages SagePlayer + project '%s' into a runnable game",
                            m_project.Name().c_str());
        ImGui::InputText("Output dir", m_dlgBuildDir, sizeof(m_dlgBuildDir));
        ImGui::TextDisabled("-> %s/%s/%s", m_dlgBuildDir, m_project.Name().c_str(),
                            m_project.Name().c_str());
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (!m_dlgBuildResult.empty())
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Built: %s", m_dlgBuildResult.c_str());
        if (ImGui::Button("Build", ImVec2(120, 0))) {
            std::string err;
            if (BuildGame(m_dlgBuildDir, err)) {
                m_dlgError.clear();
                m_dlgBuildResult = (fs::path(m_dlgBuildDir) / m_project.Name()).string();
            } else {
                m_dlgError = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Path", m_dlgOpenPath, sizeof(m_dlgOpenPath));
        ImGui::TextDisabled("Path to a .sage scene file (tip: double-click one in Assets)");
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (ImGui::Button("Open", ImVec2(120, 0))) {
            if (LoadSceneFromFile(m_dlgOpenPath)) ImGui::CloseCurrentPopup();
            else m_dlgError = "Load failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================================
//  Окно гибких настроек движка (EngineConfig). Редактирует m_settings и
//  сохраняет в <проект>/sage.cfg — Build Game кладёт файл в собранную игру, и
//  SagePlayer/игра читают его при запуске. Оконные параметры (размер/режим/
//  vsync) применяются при следующем запуске игры, не в самом редакторе.
// ============================================================================
void EditorLayer::DrawSettingsWindow() {
    if (!m_showSettings) return;
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &m_showSettings)) { ImGui::End(); return; }

    sage::EngineConfig& c = m_settings;

    ImGui::TextDisabled("Гибкая конфигурация игры (сохраняется в проект как sage.cfg).");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Window / Окно", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputInt("Width", &c.Width);
        ImGui::InputInt("Height", &c.Height);
        const char* modes[] = {"Windowed", "Borderless", "Fullscreen"};
        int mode = (int)c.Mode;
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) c.Mode = (sage::WindowMode)mode;
        ImGui::Checkbox("VSync", &c.VSync);
        ImGui::SameLine();
        ImGui::Checkbox("Resizable", &c.Resizable);
        ImGui::InputInt("Frame Cap (0=off)", &c.FrameCap);
        static const int kMsaaVals[] = {0, 2, 4, 8};
        const char* msaa[] = {"Off", "2x", "4x", "8x"};
        int msaaIdx = c.Msaa >= 8 ? 3 : c.Msaa >= 4 ? 2 : c.Msaa >= 2 ? 1 : 0;
        if (ImGui::Combo("MSAA", &msaaIdx, msaa, IM_ARRAYSIZE(msaa)))
            c.Msaa = kMsaaVals[msaaIdx];
        ImGui::TextDisabled("Оконные параметры применяются при запуске игры.");
    }

    if (ImGui::CollapsingHeader("Display / Дисплей", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* aspects[] = {"Free", "16:9", "16:10", "4:3", "21:9"};
        int a = (int)c.Aspect;
        if (ImGui::Combo("Aspect Ratio", &a, aspects, IM_ARRAYSIZE(aspects))) c.Aspect = (sage::AspectMode)a;
        ImGui::SliderFloat("Render Scale", &c.RenderScale, 0.25f, 2.0f, "%.2fx");
        ImGui::TextDisabled("Render Scale < 1 — быстрее; > 1 — суперсэмплинг (чётче).");
    }

    if (ImGui::CollapsingHeader("Graphics / Графика", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Shadows", &c.Shadows);
        static const int kShadowVals[] = {512, 1024, 2048, 4096};
        const char* shadowRes[] = {"512", "1024", "2048", "4096"};
        int sr = c.ShadowResolution >= 4096 ? 3 : c.ShadowResolution >= 2048 ? 2 : c.ShadowResolution >= 1024 ? 1 : 0;
        if (ImGui::Combo("Shadow Resolution", &sr, shadowRes, IM_ARRAYSIZE(shadowRes)))
            c.ShadowResolution = kShadowVals[sr];
        ImGui::Checkbox("Post-Processing", &c.PostProcessing);
        ImGui::Checkbox("Fog", &c.Fog);
        ImGui::SameLine();
        ImGui::Checkbox("Skybox", &c.Skybox);
    }

    if (ImGui::CollapsingHeader("Post-Process / Пост-эффекты")) {
        ImGui::BeginDisabled(!c.PostProcessing);
        ImGui::SliderFloat("Exposure", &c.Exposure, 0.1f, 4.0f);
        ImGui::SliderFloat("Gamma", &c.Gamma, 1.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &c.Saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Contrast", &c.Contrast, 0.5f, 2.0f);
        ImGui::SliderFloat("Vignette", &c.Vignette, 0.0f, 1.0f);
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    bool haveProject = m_project.Loaded();
    ImGui::BeginDisabled(!haveProject);
    if (ImGui::Button("Save to Project")) {
        std::string path = (m_project.Dir() / "sage.cfg").string();
        if (c.SaveFile(path)) m_pluginStatusMessage = "Настройки сохранены: sage.cfg";
        else m_pluginStatusMessage = "Не удалось сохранить sage.cfg";
    }
    ImGui::EndDisabled();
    if (!haveProject) {
        ImGui::SameLine();
        ImGui::TextDisabled("(откройте проект, чтобы сохранить)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) c = sage::EngineConfig{};

    ImGui::End();
}

// ============================================================================
//  Self-test (SAGE_EDITOR_SELFTEST=1, headless CI)
// ============================================================================

// Headless-проверка ядра редактора без UI-кликов (модалки недоступны в CI):
// проект -> сцена -> undo/redo -> ассеты -> материалы -> камера -> Play.
// Результат — строкой SELFTEST: PASS/FAIL в лог.
void EditorLayer::RunSelfTest() {
    size_t before = m_scene->Count();
    std::string err;
    bool ok = true;

    std::error_code ec;
    fs::remove_all("selftest_project", ec); // от прошлого прогона
    if (!CreateProject(".", "selftest_project", err)) {
        LOG_ERROR("Editor") << "SELFTEST: create project failed: " << err;
        ok = false;
    }
    if (ok) {
        before = m_scene->Count(); // CreateProject пересоздал демо-сцену
        fs::path scenePath = m_project.ScenesDir() / "selftest.sage";
        if (!SaveSceneToFile(scenePath)) ok = false;
        if (ok) {
            NewScene(false); // пустая сцена — убеждаемся, что загрузка реально восстанавливает
            if (!LoadSceneFromFile(scenePath)) ok = false;
        }
        if (ok && m_scene->Count() != before) {
            LOG_ERROR("Editor") << "SELFTEST: entity count mismatch: saved " << before
                                << ", loaded " << m_scene->Count();
            ok = false;
        }
    }

    // --- Недавние проекты: наш проект должен был попасть в начало списка ---
    if (ok) {
        RecentProjects reload;
        reload.Load();
        bool found = !reload.List().empty() &&
                     reload.List().front() == m_project.Dir().string();
        if (!found) {
            LOG_ERROR("Editor") << "SELFTEST: recent projects did not record the new project";
            ok = false;
        }
    }

    // --- CameraComponent: сериализация вместе со сценой ---
    if (ok) {
        GameObject camObj = m_scene->FindByName("Main Camera");
        bool camOk = camObj.Valid() &&
                     m_scene->Registry().try_get<CameraComponent>(camObj.Entity()) != nullptr &&
                     HasPrimaryCamera();
        if (!camOk) {
            LOG_ERROR("Editor") << "SELFTEST: Main Camera lost after scene save/load round-trip";
            ok = false;
        }
    }

    // --- LightComponent: демо-лампа (точечный) и прожектор переживают
    // save/load, CollectLighting раскладывает их по типам (Point/Spot) ---
    if (ok) {
        GameObject lamp = m_scene->FindByName("Lamp");
        GameObject spot = m_scene->FindByName("Spotlight");
        const LightComponent* pointLc =
            lamp.Valid() ? m_scene->Registry().try_get<LightComponent>(lamp.Entity()) : nullptr;
        const LightComponent* spotLc =
            spot.Valid() ? m_scene->Registry().try_get<LightComponent>(spot.Entity()) : nullptr;

        LightingEnvironment collected = sage::ecs::CollectLighting(*m_scene);
        size_t expectPoint = m_scene->Lighting.PointLights.size() + 1; // + лампа
        size_t expectSpot = m_scene->Lighting.SpotLights.size() + 1;   // + прожектор
        bool typeOk = spotLc && spotLc->Kind == LightComponent::Type::Spot &&
                      pointLc && pointLc->Kind == LightComponent::Type::Point;
        // Направление прожектора смотрит вниз (поворот -90° по X) — Y-компонента
        // «вперёд» должна быть заметно отрицательной.
        bool dirOk = !collected.SpotLights.empty() && collected.SpotLights.front().Direction.y < -0.9f;
        if (!typeOk || collected.PointLights.size() != expectPoint ||
            collected.SpotLights.size() != expectSpot || !dirOk) {
            LOG_ERROR("Editor") << "SELFTEST: light round-trip failed (types " << typeOk
                                << ", points " << collected.PointLights.size() << "/" << expectPoint
                                << ", spots " << collected.SpotLights.size() << "/" << expectSpot
                                << ", dir " << dirOk << ")";
            ok = false;
        }
    }

    // --- Dirty-маркер: мутация ставит, сохранение снимает ---
    if (ok) {
        PushUndoSnapshot();
        CreateCubeEntity("DirtyProbe");
        if (!m_sceneDirty) {
            LOG_ERROR("Editor") << "SELFTEST: scene not marked dirty after mutation";
            ok = false;
        }
        if (ok && !SaveSceneToFile(m_project.ScenesDir() / "selftest.sage")) ok = false;
        if (ok && m_sceneDirty) {
            LOG_ERROR("Editor") << "SELFTEST: dirty flag not cleared by save";
            ok = false;
        }
        Undo(); // вернуть сцену к сохранённому состоянию
    }

    // --- Undo/Redo: создание сущности откатывается и накатывается обратно ---
    if (ok) {
        size_t n0 = m_scene->Count();
        PushUndoSnapshot();
        CreateCubeEntity("UndoProbe");
        Undo();
        if (m_scene->Count() != n0) {
            LOG_ERROR("Editor") << "SELFTEST: undo failed (count " << m_scene->Count() << ", expected " << n0 << ")";
            ok = false;
        }
        Redo();
        if (ok && m_scene->Count() != n0 + 1) {
            LOG_ERROR("Editor") << "SELFTEST: redo failed (count " << m_scene->Count() << ", expected " << n0 + 1 << ")";
            ok = false;
        }
        Undo(); // вернуть сцену к исходным n0 сущностям
    }

    // --- Создание ассетов: та же логика, что у модалки Create Asset ---
    if (ok) {
        struct { AssetsPanel::CreateKind kind; const char* name; const char* expect; } cases[] = {
            {AssetsPanel::CreateKind::Folder, "selftest_dir", "selftest_dir"},
            {AssetsPanel::CreateKind::Script, "selftest_script", "selftest_script.lua"},
            {AssetsPanel::CreateKind::Material, "selftest_mat", "selftest_mat.sagemat"},
        };
        for (const auto& c : cases) {
            fs::path created;
            std::string createErr;
            if (!AssetsPanel::CreateAsset(c.kind, c.name, m_assetsCwd, created, createErr) ||
                !fs::exists(m_assetsCwd / c.expect, ec)) {
                LOG_ERROR("Editor") << "SELFTEST: asset create failed for " << c.expect
                                    << " (" << createErr << ")";
                ok = false;
                break;
            }
        }
    }

    // --- Материалы: правка разделяемого экземпляра + назначение + сохранение
    // сцены + перезагрузка => путь и albedo восстановлены ---
    if (ok) {
        std::string matPath = (m_assetsCwd / "selftest_mat.sagemat").string();
        auto material = ResourceManager::Instance().GetMaterial(matPath);
        material->Albedo = {0.1f, 0.2f, 0.9f};
        try {
            material->SaveToFile(matPath);
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "SELFTEST: material save failed: " << e.what();
            ok = false;
        }
        if (ok) {
            GameObject cube = m_scene->FindByName("Green Cube");
            MeshRendererComponent& mr = cube.Renderer();
            mr.MaterialPath = matPath;
            mr.MaterialPtr = material;

            fs::path scenePath = m_project.ScenesDir() / "selftest_mat.sage";
            if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) ok = false;
            if (ok) {
                MeshRendererComponent& reloaded = m_scene->FindByName("Green Cube").Renderer();
                bool pathOk = reloaded.MaterialPath == matPath;
                bool albedoOk = reloaded.MaterialPtr &&
                                std::abs(reloaded.MaterialPtr->Albedo.b - 0.9f) < 0.001f;
                if (!pathOk || !albedoOk) {
                    LOG_ERROR("Editor") << "SELFTEST: material round-trip failed (path "
                                        << pathOk << ", albedo " << albedoOk << ")";
                    ok = false;
                }
                // Убираем материал, чтобы дальнейшие проверки шли по прежнему сценарию.
                if (ok) {
                    reloaded.MaterialPath.clear();
                    reloaded.MaterialPtr = nullptr;
                }
            }
        }
    }

    // --- Примитивы + атмосфера (скайбокс/туман): переживают save/load ---
    if (ok) {
        GameObject probe = m_scene->FindByName("Red Cube");
        if (probe.Valid()) {
            MeshRendererComponent& mr = probe.Renderer();
            mr.Ref = MeshRef{MeshRef::Type::Sphere, ""};
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
        }
        m_scene->Lighting.Fog.Enabled = true;
        m_scene->Lighting.Fog.Color = {0.1f, 0.4f, 0.7f};
        m_scene->Lighting.Skybox.TopColor = {0.05f, 0.1f, 0.3f};

        fs::path scenePath = m_project.ScenesDir() / "selftest_env.sage";
        if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) ok = false;
        if (ok) {
            GameObject reloaded = m_scene->FindByName("Red Cube");
            bool meshOk = reloaded.Valid() && reloaded.Renderer().Ref.type == MeshRef::Type::Sphere &&
                          reloaded.Renderer().MeshPtr != nullptr;
            bool fogOk = m_scene->Lighting.Fog.Enabled &&
                         std::abs(m_scene->Lighting.Fog.Color.b - 0.7f) < 0.001f;
            bool skyOk = m_scene->Lighting.Skybox.Enabled &&
                         std::abs(m_scene->Lighting.Skybox.TopColor.b - 0.3f) < 0.001f;
            if (!meshOk || !fogOk || !skyOk) {
                LOG_ERROR("Editor") << "SELFTEST: primitive/environment round-trip failed (mesh "
                                    << meshOk << ", fog " << fogOk << ", sky " << skyOk << ")";
                ok = false;
            }
        }
    }

    // --- Сборка игры: SagePlayer + project/ упакованы в запускаемую папку ---
    // (запуск упакованной игры проверяет smoke-тест 5/5 отдельным процессом)
    if (ok) {
        std::string buildErr;
        if (!BuildGame("selftest_dist", buildErr)) {
            LOG_ERROR("Editor") << "SELFTEST: game build failed: " << buildErr;
            ok = false;
        } else {
            fs::path gameDir = fs::path("selftest_dist") / m_project.Name();
#ifdef _WIN32
            fs::path exe = gameDir / (m_project.Name() + ".exe");
#else
            fs::path exe = gameDir / m_project.Name();
#endif
            if (!fs::exists(exe, ec) ||
                !fs::exists(gameDir / "project" / "project.sageproj", ec) ||
                !fs::exists(gameDir / "assets" / "shaders" / "lit.frag", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: built game layout incomplete in " << gameDir.string();
                ok = false;
            }
        }
    }

    // --- Play: скрипт вращает сущность, Stop откатывает сцену к снапшоту ---
    if (ok) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (!green.Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: Green Cube not found for play test";
            ok = false;
        } else {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            float rotBefore = green.GetTransform().Rotation.y;

            StartPlay();
            // Несколько тиков «вручную» — self-test выполняется до главного цикла.
            for (int i = 0; i < 5; ++i) m_playScripts->UpdateAll(0.1f);
            float rotDuring = m_scene->FindByName("Green Cube").GetTransform().Rotation.y;
            StopPlay();
            float rotAfter = m_scene->FindByName("Green Cube").GetTransform().Rotation.y;

            if (std::abs(rotDuring - rotBefore) < 1.0f) {
                LOG_ERROR("Editor") << "SELFTEST: play failed - script did not rotate entity ("
                                    << rotBefore << " -> " << rotDuring << ")";
                ok = false;
            }
            if (ok && std::abs(rotAfter - rotBefore) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: stop failed - scene not restored (rot "
                                    << rotAfter << ", expected " << rotBefore << ")";
                ok = false;
            }
        }
    }

    // --- Физика: динамическое тело падает под гравитацией, Stop откатывает ---
    if (ok) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            // Убираем крутящий скрипт, вешаем твёрдое тело — чистая физика.
            m_scene->Registry().remove<ScriptComponent>(green.Entity());
            m_scene->Registry().emplace_or_replace<RigidBodyComponent>(green.Entity());
            float yBefore = green.GetTransform().Position.y;

            StartPlay();
            if (!m_playPhysics || m_playPhysics->BodyCount() < 1) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - no bodies created";
                ok = false;
            }
            for (int i = 0; i < 10; ++i) m_playPhysics->Step(*m_scene, 0.1f);
            float yDuring = m_scene->FindByName("Green Cube").GetTransform().Position.y;
            StopPlay();
            float yAfter = m_scene->FindByName("Green Cube").GetTransform().Position.y;

            if (ok && yDuring >= yBefore - 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - body did not fall ("
                                    << yBefore << " -> " << yDuring << ")";
                ok = false;
            }
            if (ok && std::abs(yAfter - yBefore) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: physics stop failed - scene not restored (y "
                                    << yAfter << ", expected " << yBefore << ")";
                ok = false;
            }
        }
    }

    // --- Анимация: демо-скелет проигрывается, палитра костей меняется во времени ---
    if (ok) {
        GameObject rig = m_scene->CreateObject("SelftestRig");
        m_scene->Registry().emplace<AnimatedModelComponent>(rig.Entity());

        sage::anim::UpdateAnimators(*m_scene, 0.0f); // инициализация (загрузка демо + rig)
        AnimatedModelComponent& am = m_scene->Registry().get<AnimatedModelComponent>(rig.Entity());
        if (!am.Model || am.Anim.BoneCount() < 2) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - rig not built";
            ok = false;
        } else if (am.Anim.ClipCount() < 1) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - no clips";
            ok = false;
        } else {
            glm::mat4 boneA = am.Anim.BoneMatrices()[1]; // поза в начале
            for (int i = 0; i < 5; ++i) sage::anim::UpdateAnimators(*m_scene, 0.1f); // t=0.5s
            glm::mat4 boneB = am.Anim.BoneMatrices()[1];
            float diff = 0.0f;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                diff += std::abs(boneA[c][r] - boneB[c][r]);
            if (diff < 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: animation failed - bone pose did not change over time";
                ok = false;
            }
        }
        m_scene->RemoveObject(rig.Id());
    }

    // --- Конфиг: сохранение и загрузка настроек сохраняют значения (round-trip) ---
    if (ok) {
        sage::EngineConfig a;
        a.Shadows = false;
        a.PostProcessing = false;
        a.ShadowResolution = 1024;
        a.Aspect = sage::AspectMode::R21x9;
        a.RenderScale = 0.75f;
        a.VSync = false;
        a.Msaa = 4;
        std::string cfgPath = "selftest_settings.cfg";
        if (!a.SaveFile(cfgPath)) {
            LOG_ERROR("Editor") << "SELFTEST: config save failed";
            ok = false;
        } else {
            sage::EngineConfig b; // из значений по умолчанию
            if (!b.LoadFile(cfgPath)) {
                LOG_ERROR("Editor") << "SELFTEST: config load failed";
                ok = false;
            } else if (b.Shadows != false || b.PostProcessing != false ||
                       b.ShadowResolution != 1024 || b.Aspect != sage::AspectMode::R21x9 ||
                       std::abs(b.RenderScale - 0.75f) > 0.001f || b.VSync != false || b.Msaa != 4) {
                LOG_ERROR("Editor") << "SELFTEST: config round-trip mismatch";
                ok = false;
            }
            std::error_code cfgEc;
            fs::remove(cfgPath, cfgEc);
        }
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene + undo/redo + assets + "
                               << "materials + camera + light + primitives + environment + build + "
                               << "recent + dirty + play + physics + animation + config, " << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}
