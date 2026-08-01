#include "EditorLayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>

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
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/ParticlePresets.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

namespace fs = std::filesystem;

namespace {

// Пересечение луча с произвольным AABB [bmin, bmax] в локальном пространстве
// объекта (slab-тест). Возвращает t входа (>=0) или отрицательное при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (bmin - ro) * inv;
    glm::vec3 t1 = (bmax - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

// Луч vs единичный куб [-0.5,0.5]^3 — маркеры невидимых сущностей (камера/свет).
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    return RayBox(ro, rd, glm::vec3(-0.5f), glm::vec3(0.5f));
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

    // --- Превью-рендер: тени/Viewport/Game/PostFX/гизмо — в EditorSceneRenderer ---
    m_renderer.Init();

    m_gizmoOp = (int)ImGuizmo::TRANSLATE; // дефолтный режим гизмо (default 0 невалиден)

    NewScene(/*withDemoContent=*/true);

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Дефолтные пути диалогов теперь инициализирует DialogsPanel (в конструкторе).
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
    // ПО УМОЛЧАНИЮ ОТКЛЮЧЕНЫ: система плагинов v1 экспериментальная (нестабильный
    // ABI между сборками), поэтому редактор их не грузит, пока явно не разрешено
    // переменной SAGE_EDITOR_PLUGINS=1. Без неё plugins/ игнорируется.
    if (std::getenv("SAGE_EDITOR_PLUGINS")) {
        fs::path pluginsDir = fs::current_path() / "plugins";
        if (const char* dir = std::getenv("SAGE_PLUGINS_DIR")) pluginsDir = dir;
        m_plugins.LoadAll(pluginsDir, m_pluginCtx);
    } else {
        LOG_INFO("Editor") << "Плагины редактора отключены (SAGE_EDITOR_PLUGINS не задан)";
    }

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();
    if (std::getenv("SAGE_EDITOR_E2E")) RunE2EGameTest();
    if (std::getenv("SAGE_EDITOR_OPEN_PROJECT")) RunHeadlessProjectSession();

    // Открыть окно Settings при старте (для скриншот-проверки/демо настроек).
    if (std::getenv("SAGE_EDITOR_SHOW_SETTINGS")) { m_launcher.Dismiss(); m_showSettings = true; }
    // Открыть окно About (версии подсистем) при старте — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_ABOUT")) { m_launcher.Dismiss(); m_showAbout = true; }
    // Вывести вперёд панель Game (вид от игровой камеры) — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_GAME")) m_game.RequestFocus();

    // Авто-вход в Play при старте (визуальная проверка/CI): вешает spin.lua на
    // Green Cube демо-сцены и нажимает Play — на скриншоте куб будет повёрнут,
    // а в тулбаре гореть PLAYING. Launcher в этом режиме не показываем.
    if (std::getenv("SAGE_EDITOR_AUTOPLAY")) {
        m_launcher.Dismiss(); // headless-прогон — hub не должен закрывать кадр
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            SetSelectedId(green.Id());
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
        // Ввод игре — только пока в фокусе панель Game (см. EditorPlayInput).
        // В остальное время действия гасятся: клавиши уходят редактору.
        if (m_playRawInput) {
            GLFWwindow* handle = sage::Application::Get().GetWindow().Handle();
            m_playRawInput->SetGameFocused(m_game.Focused());
            m_playRawInput->SyncCapture();
            if (m_game.Focused()) m_playInput.Update(handle);
            else m_playInput.UpdateIdle();

            // ESC отпускает захваченный курсор, не выходя из Play: иначе из
            // игры от первого лица в редакторе было бы не выбраться мышью.
            if (m_playRawInput->MouseCaptured() &&
                glfwGetKey(handle, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                m_playRawInput->SetMouseCaptured(false);
            }
        }
        if (m_playScripts) m_playScripts->UpdateAll(dt);
        if (m_playPhysics) m_playPhysics->Step(*m_scene, dt);
    }
    // Время сцены для uTime собственных шейдеров + горячая перезагрузка
    // изменённых .vert/.frag: правка шейдера видна во вьюпорте сразу.
    m_renderer.Tick(dt);
    ResourceManager::Instance().ReloadChangedShaders();

    // Анимации проигрываются и в режиме правки — чтобы в вьюпорте было видно
    // движение скелетных моделей (превью), не только в Play.
    sage::anim::UpdateAnimators(*m_scene, dt);
    // Частицы эмиттеров ECS — тоже живут в режиме правки (превью эффектов).
    sage::fx::UpdateEmitters(*m_scene, m_renderer.Particles(), dt);
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
    // Паритет с рантаймом: частицы доступны скриптам уже в OnStart
    // (EmitParticles/CreateParticleStream рисуются в предпросмотре сцены).
    m_playScripts->BindParticles(m_renderer.Particles());

    // Ввод — как в собранной игре: действия объявляют сами скрипты (BindAction),
    // поэтому карту действий начинаем с ЧИСТОГО ЛИСТА на каждый Play (иначе
    // раскладка прошлого запуска пережила бы правку скрипта), а привязываем ДО
    // AttachScript — OnStart скриптов зовёт BindAction прямо оттуда.
    m_playInput = InputSystem();
    m_playInput.Attach(sage::Application::Get().GetWindow());
    m_playRawInput = std::make_unique<EditorPlayInput>(m_playInput, sage::Application::Get().GetWindow());
    m_playScripts->BindInput(m_playInput.Actions());
    m_playScripts->BindRawInput(*m_playRawInput);

    // Звук — как в собранной игре. Устройство может отсутствовать (headless CI):
    // AudioEngine в этом случае работает вхолостую, но вызовы из Lua валидны.
    if (!m_playAudio) m_playAudio = std::make_unique<AudioEngine>();
    m_playScripts->BindAudio(*m_playAudio);

    // Модули Lua (require "voxel") ищутся в скриптовой папке ОТКРЫТОГО ПРОЕКТА —
    // тот же контракт, что в собранной игре, где CWD и есть корень проекта.
    if (m_project.Loaded())
        m_playScripts->AddScriptSearchPath((m_project.Dir() / "assets" / "scripts").string());
    m_playScripts->AddScriptSearchPath("assets/scripts"); // скрипты рядом с редактором

    // Параметры запуска игры (LaunchArg в Lua) — до AttachScript, потому что
    // OnStart скриптов читает их сразу. В редакторе источник один: окружение
    // (headless-прогон CI ставит SAGE_GAME_ARGS="autopilot=1").
    if (const char* args = std::getenv("SAGE_GAME_ARGS")) m_playScripts->SetLaunchArgsFromString(args);

    // Привязываем скрипты всех сущностей со ScriptComponent. Ошибка в одном
    // скрипте (нет файла, синтаксис) не срывает Play — логируется, остальные
    // продолжают работать.
    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent, IdComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        // Пути скриптов в сцене — ОТНОСИТЕЛЬНО ПРОЕКТА ("assets/scripts/x.lua"):
        // так их резолвит собранная игра (SagePlayer делает chdir в проект). CWD
        // редактора — не папка проекта, поэтому здесь резолвим сами: как есть
        // (скрипты редактора, абсолютные пути), иначе — от корня проекта. Без
        // этого скрипты проекта работали бы в собранной игре, но НЕ в Play.
        std::string resolved = path;
        std::error_code scriptEc;
        if (!fs::exists(resolved, scriptEc) && m_project.Loaded()) {
            fs::path inProject = m_project.Dir() / path;
            if (fs::exists(inProject, scriptEc)) resolved = inProject.string();
        }
        try {
            m_playScripts->AttachScript(GameObject(&m_scene->Registry(), e), resolved);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Editor") << "Play: script attach failed: " << ex.what();
        }
    }

    // Физика: строим мир по сущностям с RigidBodyComponent. Бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный Simple (см. PhysicsWorld::DefaultBackend).
    m_playPhysics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты получают доступ к физике времени выполнения (SetVelocity/GetVelocity/
    // SetGravity) — привязываем ПОСЛЕ построения мира, чтобы RuntimeBody сущностей
    // уже существовали к первому OnUpdate.
    m_playScripts->BindPhysics(*m_playPhysics);

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
    // Курсор возвращается человеку РАНЬШЕ всего остального: игра могла его
    // захватить, и без этого Stop оставил бы редактор без мыши.
    if (m_playRawInput) m_playRawInput->ReleaseCapture();
    m_playRawInput.reset();
    RestoreSceneFromString(m_playSnapshot);
    m_playSnapshot.clear();
    m_playState = EditorPlayState::Editing;
    m_viewport.RequestFocus(); // вернулись к редактированию — Viewport вперёд
    LOG_INFO("Editor") << "Play stopped, scene restored";
}

// ============================================================================
//  Undo/Redo (снапшот-модель) + dirty-маркер
// ============================================================================

bool EditorLayer::RestoreSceneFromString(const std::string& snapshot) {
    try {
        std::unique_ptr<Scene> restored = SceneSerializer::LoadFromString(snapshot);
        // Запечённый GI переезжает указателем: строковый снапшот не тащит
        // страницы лайтмап, а бейк валиден для той же статичной геометрии
        // (Transplant сверяет отпечаток и при несовпадении не переносит).
        if (m_scene && restored) sage::gi::Transplant(*m_scene, *restored);
        m_scene = std::move(restored);
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

namespace {
// Копирует компонент T с сущности src на copy, если он есть. Дубликат должен
// нести ВСЕ движковые компоненты — раньше копировались только Script/Camera, и
// дубликат света/физического тела/эмиттера молча терял суть оригинала.
template <typename T>
void CopyComponentIfPresent(GameObject& src, GameObject& copy) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        copy.Registry()->emplace_or_replace<T>(copy.Entity(), *c);
    }
}
} // namespace

namespace {
// Копирует ВСЕ компоненты (кроме имени/иерархии) из src в dst — может быть в
// ДРУГОЙ сцене (CopyComponentIfPresent работает по реестрам независимо). Рантайм-
// поля копии сбрасываются: тело физики/сустав/анимация/эмиттер строятся заново.
void CopyAllComponents(GameObject& src, GameObject& dst) {
    dst.GetTransform() = src.GetTransform();
    dst.Renderer() = src.Renderer();
    CopyComponentIfPresent<ScriptComponent>(src, dst);
    CopyComponentIfPresent<CameraComponent>(src, dst);
    CopyComponentIfPresent<LightComponent>(src, dst);
    CopyComponentIfPresent<RigidBodyComponent>(src, dst);
    CopyComponentIfPresent<ColliderComponent>(src, dst);
    CopyComponentIfPresent<JointComponent>(src, dst);
    CopyComponentIfPresent<ParticleEmitterComponent>(src, dst);
    CopyComponentIfPresent<AnimatedModelComponent>(src, dst);
    CopyComponentIfPresent<UIElementComponent>(src, dst);
    if (auto* rb = dst.Registry()->try_get<RigidBodyComponent>(dst.Entity()))
        rb->RuntimeBody = sage::physics::kInvalidBody;
    if (auto* jc = dst.Registry()->try_get<JointComponent>(dst.Entity()))
        jc->RuntimeJoint = sage::physics::kInvalidJoint;
    if (auto* am = dst.Registry()->try_get<AnimatedModelComponent>(dst.Entity())) {
        am->Model.reset();
        am->Anim = sage::anim::Animator{};
        am->Ready = false;
    }
    if (auto* pe = dst.Registry()->try_get<ParticleEmitterComponent>(dst.Entity()))
        pe->Accumulator = 0.0f;
}

// Рекурсивно копирует поддерево (src.srcE + потомки) в dst под dstParent,
// сохраняя иерархию. Новые сущности получают новые id. Возвращает корень копии.
GameObject CopySubtree(Scene& src, entt::entity srcE, Scene& dst, entt::entity dstParent) {
    GameObject s(&src.Registry(), srcE);
    GameObject d = dst.CreateObject(s.Name());
    CopyAllComponents(s, d);
    if (dstParent != entt::null) dst.SetParent(d.Entity(), dstParent);
    if (const HierarchyComponent* h = src.Registry().try_get<HierarchyComponent>(srcE)) {
        std::vector<entt::entity> kids = h->Children; // копия: SetParent мутирует список
        for (entt::entity k : kids)
            if (src.Registry().valid(k)) CopySubtree(src, k, dst, d.Entity());
    }
    return d;
}
} // namespace

// Копирует одну сущность (без детей) со всеми компонентами; сдвиг, чтобы копия
// не сливалась с оригиналом. Возвращает копию.
GameObject EditorLayer::DuplicateEntity(GameObject src) {
    GameObject copy = m_scene->CreateObject(src.Name() + " Copy");
    CopyAllComponents(src, copy);
    copy.GetTransform().Position.x += 0.5f;
    return copy;
}

void EditorLayer::DuplicateSelected() {
    if (m_selection.empty()) return;
    PushUndoSnapshot();
    std::vector<int> copies;
    for (int id : m_selection) {
        GameObject src = m_scene->Get(id);
        if (!src.Valid()) continue;
        entt::entity parent = m_scene->ParentOf(src.Entity()); // копия остаётся у того же родителя
        GameObject copy = DuplicateEntity(src);
        if (parent != entt::null) m_scene->SetParent(copy.Entity(), parent);
        copies.push_back(copy.Id());
    }
    m_selection = copies;
    m_selectedId = copies.empty() ? -1 : copies.back();
}

void EditorLayer::DeleteSelected() {
    bool any = false;
    for (int id : m_selection)
        if (m_scene->Get(id).Valid()) { any = true; break; }
    if (!any) return;
    PushUndoSnapshot();
    for (int id : m_selection)
        if (m_scene->Get(id).Valid()) m_scene->RemoveObject(id); // удаляет и поддерево
    SetSelectedId(-1);
    m_selection.clear();
}

void EditorLayer::SetSelectedId(int id) {
    m_selectedId = id;
    m_selection.clear();
    if (id != -1) m_selection.push_back(id);
}

bool EditorLayer::IsSelected(int id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void EditorLayer::ToggleSelection(int id) {
    if (id == -1) return;
    auto it = std::find(m_selection.begin(), m_selection.end(), id);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        m_selectedId = m_selection.empty() ? -1 : m_selection.back();
    } else {
        m_selection.push_back(id);
        m_selectedId = id; // добавленная становится первичной
    }
}

// ============================================================================
//  Префабы — переиспользуемые сущности-поддеревья (.sageprefab). Формат —
//  та же JSON-сериализация, что у сцен: префаб = мини-сцена с одним корнем.
// ============================================================================
bool EditorLayer::SaveSelectedAsPrefab(const fs::path& path, std::string& err) {
    GameObject root = m_scene->Get(m_selectedId);
    if (!root.Valid()) { err = "ничего не выбрано"; return false; }
    Scene temp("Prefab");
    CopySubtree(*m_scene, root.Entity(), temp, entt::null); // корень + потомки
    try {
        SceneSerializer::Save(temp, path.string());
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    SetStatusMessage("Префаб сохранён: " + path.filename().string());
    return true;
}

int EditorLayer::InstantiatePrefab(const fs::path& path) {
    std::unique_ptr<Scene> loaded;
    try {
        loaded = SceneSerializer::Load(path.string());
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Префаб не загрузился (" << path.string() << "): " << e.what();
        return -1;
    }
    PushUndoSnapshot();
    // Копируем ВСЕ корни префаба в текущую сцену (новые id, сохранена иерархия).
    int firstRootId = -1;
    auto& reg = loaded->Registry();
    std::vector<entt::entity> roots;
    for (auto e : reg.view<IdComponent>()) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null || !reg.valid(h->Parent)) roots.push_back(e);
    }
    for (entt::entity r : roots) {
        GameObject copy = CopySubtree(*loaded, r, *m_scene, entt::null);
        if (firstRootId == -1) firstRootId = copy.Id();
    }
    if (firstRootId != -1) SetSelectedId(firstRootId);
    return firstRootId;
}

// ============================================================================
//  Сцена / проект
// ============================================================================

void EditorLayer::NewScene(bool withDemoContent) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_undoStack.clear();
    m_redoStack.clear();
    m_scene = std::make_unique<Scene>("Untitled");
    SetSelectedId(-1);
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

        // Криволинейные примитивы — витрина форм И проверка аутлайна выделения:
        // кайма строится из СИЛУЭТА реального меша, поэтому одинаково точна для
        // сферы/цилиндра/конуса, а не только для боксов.
        struct Prim { const char* name; MeshRef::Type type; glm::vec3 pos; glm::vec3 color; };
        Prim prims[] = {
            {"Sphere",   MeshRef::Type::Sphere,   {-3.0f, 0.5f, 1.8f}, {0.85f, 0.55f, 0.25f}},
            {"Cylinder", MeshRef::Type::Cylinder, {-1.5f, 0.5f, 2.2f}, {0.55f, 0.35f, 0.80f}},
            {"Cone",     MeshRef::Type::Cone,     {1.5f,  0.5f, 2.2f}, {0.30f, 0.70f, 0.70f}},
        };
        for (const Prim& p : prims) {
            GameObject obj = CreatePrimitiveEntity(p.name, p.type);
            obj.GetTransform().Position = p.pos;
            obj.Renderer().Color = p.color;
        }

        // Игровая камера сцены — панель Game сразу показывает картинку. НАРОЧНО
        // поставлена НЕ как редакторская орбитальная камера ({6.5,5,6.5}, взгляд
        // сверху): низкий, почти фронтальный «кинематографичный» ракурс с уровня
        // сцены — так сразу видно, что панель Game показывает СВОЮ, игровую
        // камеру, а не вид вьюпорта. Сущность без меша (не рисуется в мире).
        GameObject camObj = m_scene->CreateObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.5f, 6.5f};
        camObj.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f}; // чуть вниз, вдоль -Z
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

        // Демо-худ (UIElementComponent): панель со скруглением и рамкой + полоса
        // здоровья ребёнком — показывает UI-систему сразу в панели Game и служит
        // стартовой точкой для своего интерфейса (правится в Inspector).
        GameObject hud = m_scene->CreateObject("HUD Panel");
        UIElementComponent hudUi;
        hudUi.Type = UIElementComponent::Kind::Panel;
        hudUi.Anchor = UIAnchor::TopLeft;
        hudUi.Offset = {16.0f, 16.0f};
        hudUi.Size = {230.0f, 64.0f};
        hudUi.Rounding = 12.0f;
        hudUi.BorderThickness = 2.0f;
        hudUi.Text = "SAGE UI";
        hudUi.TextCentered = false;
        m_scene->Registry().emplace<UIElementComponent>(hud.Entity(), hudUi);

        GameObject hp = m_scene->CreateObject("HP Bar");
        UIElementComponent hpUi;
        hpUi.Type = UIElementComponent::Kind::Bar;
        hpUi.Anchor = UIAnchor::BottomLeft;   // внутри панели-родителя
        hpUi.Offset = {12.0f, 8.0f};
        hpUi.Size = {206.0f, 18.0f};
        hpUi.Rounding = 8.0f;
        hpUi.Color = {0.0f, 0.0f, 0.0f, 0.55f};
        hpUi.Value = 0.72f;
        hpUi.BarFillColor = {0.85f, 0.30f, 0.30f, 1.0f};
        m_scene->Registry().emplace<UIElementComponent>(hp.Entity(), hpUi);
        m_scene->SetParent(hp.Entity(), hud.Entity());

        // Что-то выбрано сразу — гизмо видно, Inspector не пустой. Выбираем
        // криволинейный примитив: сразу демонстрирует аутлайн на изогнутом
        // силуэте (кайма строится из силуэта меша — точна для любой формы).
        GameObject sel = m_scene->FindByName("Cone");
        if (!sel.Valid()) sel = m_scene->FindByName("Green Cube");
        if (sel.Valid()) SetSelectedId(sel.Id());
    }
    UpdateWindowTitle();
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_undoStack.clear();
        m_redoStack.clear();
        SetSelectedId(-1);
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

void EditorLayer::PickAtViewport(float u, float v, bool additive) {
    // Луч из камеры через пиксель вьюпорта: unprojection ближней/дальней точек NDC.
    glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    glm::mat4 invVP = glm::inverse(m_proj * m_view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec3 ro = glm::vec3(p0) / p0.w;
    glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    int bestId = -1;
    float bestDist = 1e30f;
    auto view = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : view) {
        Mesh* mesh = view.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        // МИРОВАЯ матрица (учёт иерархии родителей): раньше бралась локальная —
        // дочерние сущности выделялись по неверной позиции. AABB меша — из его
        // собственных границ (center±radius), а не фиксированный единичный куб,
        // так пикинг попадает по объектам любого размера/формы (модели, плоскости).
        glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
        glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f)); // без нормализации: t остаётся в масштабе мира
        glm::vec3 c = mesh->BoundsCenter();
        glm::vec3 r(mesh->BoundsRadius());
        float t = RayBox(lro, lrd, c - r, c + r);
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
        pickMarker(e, camMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));
    auto lightMarkers = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightMarkers)
        pickMarker(e, lightMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));

    // Ctrl-клик (additive): добавить/убрать попадание из набора (клик по пустоте
    // ничего не меняет). Обычный клик: одиночный выбор (мимо всех — снять).
    if (additive) {
        if (bestId != -1) ToggleSelection(bestId);
    } else {
        SetSelectedId(bestId);
    }
}

bool EditorLayer::HasPrimaryCamera() {
    auto view = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) return true;
    }
    return false;
}

// ============================================================================
//  Кадр UI
// ============================================================================

void EditorLayer::OnRender() {
    sage::Application& app = sage::Application::Get();

    // Итоговое освещение кадра: окружение сцены + света-сущности; один
    // shadow-проход на кадр, Viewport и Game сэмплируют общую карту.
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    m_renderer.RenderShadow(*m_scene, env, m_camera); // общая карта теней (Viewport + Game)
    m_renderer.RenderViewport(*m_scene, m_camera, env, m_selectedId, m_selection, m_renderMode, m_showGrid,
                              cfg, m_view, m_proj); // отдаёт view/proj для гизмо/пикинга
    m_renderer.RenderGame(*m_scene, env, cfg);      // Primary-камера сцены (если есть)

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    DrawDockspaceAndMenu(); // включая модалки (m_dialogs) и окно настроек (m_settingsPanel)
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
    // Viewport докается ПЕРВЫМ в центральный узел — так он и есть таб по
    // умолчанию (первый добавленный к узлу становится выбранным). Раньше первым
    // шёл Game, из-за чего редактор открывался на «игровом окне» без пикинга/
    // гизмо/аутлайна — выглядело как «выделение не работает». Game выходит
    // вперёд при входе в Play (GamePanel::RequestFocus).
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Game", center);
    ImGui::DockBuilderFinish(dockspaceId);

    ImGui::SetWindowFocus("Viewport");
}

// Help > About SAGE — версия движка + таблица версий ВСЕХ подсистем (пока все
// v1). Единый источник — sage::EngineSystems() (тот же список, что в лог старта).
void EditorLayer::DrawAboutWindow() {
    if (!m_showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("About SAGE", &m_showAbout)) { ImGui::End(); return; }

    ImGui::Text("SAGE Engine %s", kSageEngineVersion);
    ImGui::TextDisabled("Модульный 3D-движок: ECS, RHI, PBR, физика, скриптинг, UI.");
    ImGui::Spacing();
    const auto& systems = sage::EngineSystems();
    ImGui::Text("Подсистемы: %zu (все v1)", systems.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##systems", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const sage::SystemVersion& s : systems) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.Name);
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", s.Tag().c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.Summary);
        }
        ImGui::EndTable();
    }
    ImGui::End();
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

    // Статистика рендера (отсечение/батчинг) + FPS — справа.
    char stat[128];
    const sage::ecs::RenderStats& rs = m_renderer.LastStats();
    std::snprintf(stat, sizeof(stat), "meshes %d/%d  culled %d  batches %d  |  %.0f FPS",
                  rs.Drawn, rs.Total, rs.Culled, rs.Batches, sage::Application::Get().Fps());
    float w = ImGui::CalcTextSize(stat).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextDisabled("%s", stat);

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
                SetSelectedId(m_scene->CreateObject("Empty").Id());
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
                        SetSelectedId(CreatePrimitiveEntity(p.name, p.type).Id());
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Create Camera")) {
                PushUndoSnapshot();
                GameObject camObj = m_scene->CreateObject("Camera");
                m_scene->Registry().emplace<CameraComponent>(camObj.Entity());
                SetSelectedId(camObj.Id());
            }
            if (ImGui::MenuItem("Create Light")) {
                PushUndoSnapshot();
                GameObject lightObj = m_scene->CreateObject("Light");
                lightObj.GetTransform().Position = {0.0f, 2.5f, 0.0f};
                m_scene->Registry().emplace<LightComponent>(lightObj.Entity());
                SetSelectedId(lightObj.Id());
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
                SetSelectedId(box.Id());
            }
            // Скелетно-анимированная модель: без пути — процедурный демо-щупалец
            // с клипом «Wave» (сразу проигрывается в вьюпорте).
            if (ImGui::MenuItem("Create Animated Model")) {
                PushUndoSnapshot();
                GameObject anim = m_scene->CreateObject("Animated Model");
                anim.GetTransform().Position = {0.0f, 0.0f, 0.0f};
                m_scene->Registry().emplace<AnimatedModelComponent>(anim.Entity());
                SetSelectedId(anim.Id());
            }
            // Эмиттер частиц: по умолчанию пресет «Fire» в точке над началом.
            if (ImGui::MenuItem("Create Particle Emitter")) {
                PushUndoSnapshot();
                GameObject fx = m_scene->CreateObject("Particle Emitter");
                fx.GetTransform().Position = {0.0f, 0.5f, 0.0f};
                ParticleEmitterComponent em;
                em.Config = ParticlePresets::Registry()[0].Make(); // Fire
                em.Preset = 0;
                m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity(), em);
                SetSelectedId(fx.Id());
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
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About SAGE...", nullptr, &m_showAbout);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре.
        std::string status = m_project.Loaded() ? ("Project: " + m_project.Name()) : "No project";
        float w = ImGui::CalcTextSize(status.c_str()).x + 16.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status.c_str());

        ImGui::EndMenuBar();
    }

    // Модалки диалогов и окно настроек — самостоятельные панели, но рисуются
    // ЗДЕСЬ, на уровне окна-хоста: модалка ImGui совпадает с OpenPopup по
    // ID-стеку окна, поэтому открытие и отрисовку нельзя разносить по окнам.
    if (openDialog) m_dialogs.Open(openDialog);
    m_dialogs.Draw(*this);
    m_settingsPanel.Draw(*this, m_showSettings);
    DrawAboutWindow();

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
