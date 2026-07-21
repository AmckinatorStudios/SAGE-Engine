#include "EditorLayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <thread>

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
#include "sage/physics/Ragdoll.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/Frustum.h"
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
        if (m_playScripts) m_playScripts->UpdateAll(dt);
        if (m_playPhysics) m_playPhysics->Step(*m_scene, dt);
    }
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
    // Jolt, если собран, иначе встроенный Builtin (см. PhysicsWorld::DefaultBackend).
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
    m_renderer.RenderShadow(*m_scene, env); // общая карта теней (Viewport + Game)
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

    // --- Управление памятью: асинхронная загрузка + GC + LRU-вытеснение ---
    // Полный GPU-путь (у редактора есть GL-контекст): плейсхолдер приходит
    // мгновенно, фоновый поток декодирует файл, PumpAsyncUploads заливает его в
    // VRAM в тот же объект; неиспользуемые текстуры собираются GC; при
    // превышении бюджета LRU-неиспользуемые вытесняются.
    if (ok) {
        auto& rm = ResourceManager::Instance();

        // Пишем настоящий файл-картинку (несжатый TGA) рядом с проектом.
        auto writeTGA = [](const fs::path& path, int w, int h) -> bool {
            std::vector<unsigned char> buf;
            unsigned char hdr[18] = {0};
            hdr[2] = 2; hdr[12] = (unsigned char)(w & 0xFF); hdr[13] = (unsigned char)((w >> 8) & 0xFF);
            hdr[14] = (unsigned char)(h & 0xFF); hdr[15] = (unsigned char)((h >> 8) & 0xFF); hdr[16] = 24;
            buf.insert(buf.end(), hdr, hdr + 18);
            for (int i = 0; i < w * h; ++i) { buf.push_back(20); buf.push_back(120); buf.push_back(220); }
            std::ofstream f(path, std::ios::binary);
            if (!f) return false;
            f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
            return (bool)f;
        };

        fs::path texA = m_project.Dir() / "selftest_texA.tga";
        fs::path texB = m_project.Dir() / "selftest_texB.tga";
        if (!writeTGA(texA, 32, 32) || !writeTGA(texB, 32, 32)) {
            LOG_ERROR("Editor") << "SELFTEST: could not write test textures";
            ok = false;
        }

        // 1) Async: мгновенно возвращается плейсхолдер 1x1, реальные пиксели
        //    приезжают после декодирования фоновым потоком + PumpAsyncUploads.
        if (ok) {
            auto tex = rm.GetTextureAsync(texA.string());
            bool placeholderOk = tex && tex->Width() == 1 && tex->Height() == 1;
            bool resident = false;
            for (int i = 0; i < 500 && !resident; ++i) {
                rm.PumpAsyncUploads();
                if (tex && tex->Width() == 32 && tex->Height() == 32) resident = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!placeholderOk || !resident) {
                LOG_ERROR("Editor") << "SELFTEST: async texture load failed (placeholder "
                                    << placeholderOk << ", resident " << resident << ")";
                ok = false;
            }
            // 2) GC: отпускаем ссылку — текстура становится неиспользуемой и
            //    выгружается (use_count()==1 держал только кэш).
            if (ok) {
                tex.reset();
                int freed = rm.GarbageCollectUnused();
                if (freed < 1) {
                    LOG_ERROR("Editor") << "SELFTEST: GarbageCollectUnused freed nothing after ref drop";
                    ok = false;
                }
            }
        }

        // 3) LRU-вытеснение: бюджет вмещает одну текстуру. Грузим A (не держим
        //    ссылку), затем B — загрузка B видит превышение и вытесняет
        //    неиспользуемую A. Резидентный размер не превышает бюджет.
        if (ok) {
            size_t oneTex = Texture::EstimateBytes(32, 32, true);
            rm.SetTextureBudgetBytes(oneTex + oneTex / 2); // < 2 текстур, >= 1
            rm.GetTexture(texA.string()); // ссылку не держим -> кандидат на вытеснение
            size_t beforeEvict = rm.GetStats().Evictions;
            rm.GetTexture(texB.string()); // триггерит EvictToBudget
            ResourceManager::Stats st = rm.GetStats();
            bool evicted = st.Evictions > beforeEvict;
            bool underBudget = st.TextureBytes <= st.TextureBudget;
            if (!evicted || !underBudget) {
                LOG_ERROR("Editor") << "SELFTEST: LRU eviction failed (evicted " << evicted
                                    << ", bytes " << st.TextureBytes << " / budget " << st.TextureBudget << ")";
                ok = false;
            }
            rm.SetTextureBudgetBytes(0);   // снимаем ограничение — не мешать остальным проверкам
            rm.GarbageCollectUnused();     // прибираем тестовые текстуры
        }

        std::error_code rmec;
        fs::remove(texA, rmec);
        fs::remove(texB, rmec);
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

    // --- Ragdoll + соединения: собранная кукла строит тела и суставы, падает ---
    if (ok) {
        int rootId = sage::physics::BuildRagdoll(*m_scene, {0.0f, 8.0f, 0.0f}, 1.0f);
        GameObject root = m_scene->Get(rootId);
        int jointComps = 0;
        m_scene->Registry().view<JointComponent>().each([&](auto) { ++jointComps; });
        bool built = root.Valid() &&
                     m_scene->Registry().all_of<RigidBodyComponent>(root.Entity()) &&
                     jointComps >= 6; // 11 костей -> 10 суставов (минимум 6)
        if (!built) {
            LOG_ERROR("Editor") << "SELFTEST: ragdoll build failed (root " << root.Valid()
                                << ", joints " << jointComps << ")";
            ok = false;
        } else {
            float yBefore = root.GetTransform().Position.y;
            StartPlay();
            // На Jolt суставы реально строятся; на Builtin — их нет (ожидаемо).
            bool jointsOk = !m_playPhysics->SupportsJoints() || m_playPhysics->JointCount() >= 6;
            for (int i = 0; i < 40; ++i) m_playPhysics->Step(*m_scene, 1.0f / 60.0f);
            float yAfter = m_scene->Get(rootId).GetTransform().Position.y;
            StopPlay();
            if (!jointsOk) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll joints not built on Jolt ("
                                    << m_playPhysics->JointCount() << ")";
                ok = false;
            } else if (yAfter >= yBefore - 0.05f) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll did not fall (" << yBefore
                                    << " -> " << yAfter << ")";
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
            // Кросс-фейд между клипами демо (Wave -> Curl) через ШТАТНЫЙ путь:
            // меняем Clip в компоненте, система сама запускает переход. Во время
            // — Fading()==true и вес в диапазоне; после — CurrentClip == Curl.
            if (ok && am.Anim.ClipCount() >= 2) {
                am.Clip = 1;
                am.BlendTime = 0.4f;
                sage::anim::UpdateAnimators(*m_scene, 0.2f); // система стартует фейд + шаг
                bool midOk = am.Anim.Fading() && am.Anim.FadeWeight() > 0.2f &&
                             am.Anim.FadeWeight() < 0.9f;
                sage::anim::UpdateAnimators(*m_scene, 0.4f); // добить переход
                bool doneOk = !am.Anim.Fading() && am.Anim.CurrentClip() == 1;
                if (!midOk || !doneOk) {
                    LOG_ERROR("Editor") << "SELFTEST: animation crossfade failed (mid " << midOk
                                        << ", done " << doneOk << ")";
                    ok = false;
                }
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

    // --- Частицы: эмиттер рождает живые частицы, пул растёт со временем ---
    if (ok) {
        ParticleSystem& particles = m_renderer.Particles();
        GameObject fx = m_scene->CreateObject("SelftestEmitter");
        fx.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        ParticleEmitterComponent em;
        em.Config = ParticlePresets::Fire();
        m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity(), em);

        for (int i = 0; i < 8; ++i) sage::fx::UpdateEmitters(*m_scene, particles, 0.05f);
        if (particles.AliveCount() == 0) {
            LOG_ERROR("Editor") << "SELFTEST: particles failed - no live particles emitted";
            ok = false;
        }
        m_scene->RemoveObject(fx.Id());
    }

    // --- Отсечение по фрустуму + инстансный батчинг ---
    if (ok) {
        // 1. Математика фрустума: точка перед камерой видна, за камерой — нет.
        glm::mat4 v = glm::lookAt(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 p = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        sage::render::Frustum fr = sage::render::Frustum::FromViewProj(p * v);
        bool inFront = fr.IntersectsSphere(glm::vec3(0, 0, 0), 1.0f);
        bool behind  = fr.IntersectsSphere(glm::vec3(0, 0, 60), 1.0f); // за спиной камеры
        if (!inFront || behind) {
            LOG_ERROR("Editor") << "SELFTEST: frustum cull math failed (front=" << inFront
                                << " behind=" << behind << ")";
            ok = false;
        }

        // 2. Батчинг: демо-сцена рендерится, группы мешей < числа сущностей
        //    (несколько кубов -> один инстанс-вызов), учёт видимых сходится.
        if (ok) {
            LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
            glm::mat4 cv = m_camera.GetViewMatrix();
            glm::mat4 cp = m_camera.GetProjectionMatrix(16.0f / 9.0f);
            sage::ecs::RenderStats st = m_renderer.RenderColorForTest(*m_scene, cv, cp, m_camera.Position, env);
            if (st.Total < 1 || st.Batches < 1 || st.Batches > st.Total ||
                st.Drawn + st.Culled != st.Total) {
                LOG_ERROR("Editor") << "SELFTEST: batch render stats inconsistent (total=" << st.Total
                                    << " drawn=" << st.Drawn << " culled=" << st.Culled
                                    << " batches=" << st.Batches << ")";
                ok = false;
            }
        }
    }

    // --- Дубликат: копия несёт ВСЕ движковые компоненты (свет/тело/коллайдер),
    // рантайм-состояние своё (RuntimeBody сброшен) ---
    if (ok) {
        GameObject src = m_scene->CreateObject("DupSource");
        m_scene->Registry().emplace<LightComponent>(src.Entity());
        RigidBodyComponent rb; rb.RuntimeBody = 12345; // «живое» тело — не должно скопироваться
        m_scene->Registry().emplace<RigidBodyComponent>(src.Entity(), rb);
        m_scene->Registry().emplace<ColliderComponent>(src.Entity());
        SetSelectedId(src.Id());
        DuplicateSelected();
        GameObject copy = m_scene->Get(m_selectedId);
        bool compOk = copy.Valid() && copy.Id() != src.Id() &&
                      m_scene->Registry().all_of<LightComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<RigidBodyComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<ColliderComponent>(copy.Entity());
        bool runtimeOk = compOk &&
            m_scene->Registry().get<RigidBodyComponent>(copy.Entity()).RuntimeBody ==
                sage::physics::kInvalidBody;
        if (!compOk || !runtimeOk) {
            LOG_ERROR("Editor") << "SELFTEST: duplicate failed (components " << compOk
                                << ", runtime reset " << runtimeOk << ")";
            ok = false;
        }
        if (copy.Valid()) m_scene->RemoveObject(copy.Id());
        m_scene->RemoveObject(src.Id());
        SetSelectedId(-1);
    }

    // --- Иерархия: мировая позиция ребёнка складывается из родителя, удаление
    // родителя сносит поддерево, undo возвращает обоих со связью ---
    if (ok) {
        size_t n0 = m_scene->Count();
        GameObject parent = m_scene->CreateObject("HierParent");
        parent.GetTransform().Position = {10.0f, 0.0f, 0.0f};
        GameObject child = m_scene->CreateObject("HierChild");
        child.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        m_scene->SetParent(child.Entity(), parent.Entity());

        glm::vec3 wp = glm::vec3(m_scene->WorldMatrix(child.Entity())[3]);
        if (std::abs(wp.x - 10.0f) > 0.001f || std::abs(wp.y - 5.0f) > 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: hierarchy world matrix wrong (" << wp.x << ", " << wp.y << ")";
            ok = false;
        }
        if (ok) {
            SetSelectedId(parent.Id());
            DeleteSelected(); // PushUndoSnapshot внутри; удаляет родителя С ребёнком
            if (m_scene->Count() != n0) {
                LOG_ERROR("Editor") << "SELFTEST: hierarchy subtree delete failed (count "
                                    << m_scene->Count() << ", expected " << n0 << ")";
                ok = false;
            }
        }
        if (ok) {
            Undo(); // возвращает и родителя, и ребёнка, и связь между ними
            GameObject p2 = m_scene->FindByName("HierParent");
            GameObject c2 = m_scene->FindByName("HierChild");
            bool linkOk = p2.Valid() && c2.Valid() &&
                          m_scene->ParentOf(c2.Entity()) == p2.Entity();
            if (!linkOk) {
                LOG_ERROR("Editor") << "SELFTEST: hierarchy undo did not restore parent link";
                ok = false;
            }
            if (p2.Valid()) m_scene->RemoveObject(p2.Id()); // чистим вместе с поддеревом
        }
        SetSelectedId(-1);
    }

    // --- Мультивыделение: набор из двух, Delete удаляет обе, Undo возвращает,
    // Duplicate дублирует обе ---
    if (ok) {
        GameObject a = m_scene->CreateObject("MultiA");
        GameObject b = m_scene->CreateObject("MultiB");
        SetSelectedId(a.Id());
        ToggleSelection(b.Id()); // теперь выбраны обе, первичная — b
        bool selOk = m_selection.size() == 2 && IsSelected(a.Id()) && IsSelected(b.Id()) &&
                     m_selectedId == b.Id();
        if (!selOk) {
            LOG_ERROR("Editor") << "SELFTEST: multi-select set wrong (size " << m_selection.size() << ")";
            ok = false;
        }
        if (ok) {
            size_t n0 = m_scene->Count();
            DuplicateSelected(); // обе -> +2, выделены копии
            if (m_scene->Count() != n0 + 2 || m_selection.size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: multi-duplicate failed (count " << m_scene->Count()
                                    << ", sel " << m_selection.size() << ")";
                ok = false;
            }
            if (ok) DeleteSelected(); // убрать копии (выделены они) -> снова n0
            // Удаляем исходную пару (снова мультивыбор) и проверяем Undo.
            if (ok) {
                SetSelectedId(m_scene->FindByName("MultiA").Id());
                ToggleSelection(m_scene->FindByName("MultiB").Id());
                size_t nBefore = m_scene->Count();
                DeleteSelected();
                bool delOk = m_scene->Count() == nBefore - 2 && m_selection.empty();
                Undo();
                bool undoOk = m_scene->FindByName("MultiA").Valid() &&
                              m_scene->FindByName("MultiB").Valid();
                if (!delOk || !undoOk) {
                    LOG_ERROR("Editor") << "SELFTEST: multi-delete/undo failed (del " << delOk
                                        << ", undo " << undoOk << ")";
                    ok = false;
                }
            }
        }
        // Чистим тестовые сущности.
        for (const char* nm : {"MultiA", "MultiB"}) {
            GameObject g = m_scene->FindByName(nm);
            if (g.Valid()) m_scene->RemoveObject(g.Id());
        }
        SetSelectedId(-1);
    }

    // --- Префабы: сущность с ребёнком -> .sageprefab -> инстанс восстанавливает
    // поддерево (имя/компонент/иерархия), новые id ---
    if (ok) {
        GameObject root = m_scene->CreateObject("PrefabRoot");
        root.GetTransform().Position = {3.0f, 1.0f, 0.0f};
        m_scene->Registry().emplace<LightComponent>(root.Entity());
        GameObject kid = m_scene->CreateObject("PrefabKid");
        kid.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        m_scene->SetParent(kid.Entity(), root.Entity());

        SetSelectedId(root.Id());
        fs::path prefabPath = m_project.Dir() / "assets" / "selftest.sageprefab";
        std::error_code pec;
        fs::create_directories(prefabPath.parent_path(), pec);
        std::string perr;
        bool saved = SaveSelectedAsPrefab(prefabPath, perr);
        // Удаляем оригинал и инстанцируем — проверяем восстановление из файла.
        if (saved) {
            m_scene->RemoveObject(root.Id());
            SetSelectedId(-1);
            int newRootId = InstantiatePrefab(prefabPath);
            GameObject nr = m_scene->Get(newRootId);
            bool rootOk = nr.Valid() && nr.Name() == "PrefabRoot" &&
                          m_scene->Registry().all_of<LightComponent>(nr.Entity()) &&
                          m_selectedId == newRootId;
            // Ребёнок восстановлен и прикреплён к новому корню.
            const HierarchyComponent* h = nr.Valid()
                ? m_scene->Registry().try_get<HierarchyComponent>(nr.Entity()) : nullptr;
            bool childOk = h && h->Children.size() == 1;
            if (!rootOk || !childOk) {
                LOG_ERROR("Editor") << "SELFTEST: prefab round-trip failed (root " << rootOk
                                    << ", child " << childOk << ")";
                ok = false;
            }
            if (nr.Valid()) m_scene->RemoveObject(nr.Id());
        } else {
            LOG_ERROR("Editor") << "SELFTEST: prefab save failed: " << perr;
            ok = false;
        }
        fs::remove(prefabPath, pec);
        SetSelectedId(-1);
    }

    // --- Пресеты качества: Low гасит тяжёлые проходы, значения переживают
    // sage.cfg round-trip, оконные параметры не тронуты ---
    if (ok) {
        sage::EngineConfig pc;
        pc.Width = 1600; pc.VSync = false;
        pc.ApplyPreset(sage::QualityPreset::Low);
        bool presetOk = !pc.Shadows && !pc.PostProcessing && !pc.AmbientOcclusion &&
                        std::abs(pc.RenderScale - 0.75f) < 0.001f &&
                        pc.Width == 1600 && pc.VSync == false;
        std::string pPath = "selftest_preset.cfg";
        bool fileOk = presetOk && pc.SaveFile(pPath);
        if (fileOk) {
            sage::EngineConfig back;
            fileOk = back.LoadFile(pPath) && !back.Shadows && !back.PostProcessing &&
                     std::abs(back.RenderScale - 0.75f) < 0.001f;
            std::error_code pEc;
            fs::remove(pPath, pEc);
        }
        if (!presetOk || !fileOk) {
            LOG_ERROR("Editor") << "SELFTEST: quality preset failed (preset " << presetOk
                                << ", file " << fileOk << ")";
            ok = false;
        }
    }

    // --- GI: бейк лайтмап + объёма проб и рендер с ними на реальном GPU ---
    // Проверяет весь конвейер запечённого GI: пометка статики, CPU-бейк
    // (BVH/path tracing/развёртка), ленивое создание GPU-ресурсов (HDR-атлас,
    // 3D-текстуры проб, меши с UV2) и отрисовку через RenderBatch.
    if (ok) {
        GameObject giFloor = CreatePrimitiveEntity("GI Floor", MeshRef::Type::Plane);
        giFloor.GetTransform().Scale = {10.0f, 1.0f, 10.0f};
        m_scene->Registry().emplace<GIStaticComponent>(giFloor.Entity());
        GameObject giCube = CreatePrimitiveEntity("GI Cube", MeshRef::Type::Cube);
        giCube.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        m_scene->Registry().emplace<GIStaticComponent>(giCube.Entity());

        sage::gi::GISettings giSettings;
        giSettings.TexelsPerUnit = 2;
        giSettings.AtlasSize = 256;
        giSettings.SampleCount = 16;
        giSettings.Bounces = 2;
        giSettings.ProbeCellSize = 3.0f;
        giSettings.MaxProbeAxis = 8;
        sage::gi::BakeInput giInput = sage::gi::CollectBakeInput(*m_scene, giSettings);
        std::shared_ptr<sage::gi::GIState> giState = sage::gi::Bake(giInput);
        bool giBaked = giState->Baked && giState->Entities.size() == 2 &&
                       !giState->Pages.empty() && giState->Probes.Valid();
        if (giBaked) {
            m_scene->GI = giState;
            glm::mat4 view = glm::lookAt(glm::vec3(6, 6, 6), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
            glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
            sage::ecs::RenderStats st = m_renderer.RenderColorForTest(
                *m_scene, view, proj, glm::vec3(6, 6, 6), sage::ecs::CollectLighting(*m_scene));
            // Оба GI-объекта видимы и нарисованы уникальными мешами с UV2;
            // GPU-ресурсы (страница атласа, 3D-объём) создались лениво.
            bool giDrawn = st.Drawn >= 2;
            bool giGpu = giState->Pages[0].Gpu != nullptr && giState->Probes.GpuR != nullptr;
            if (!giDrawn || !giGpu) {
                LOG_ERROR("Editor") << "SELFTEST: GI render failed (drawn " << st.Drawn
                                    << ", gpu " << giGpu << ")";
                ok = false;
            }
        } else {
            LOG_ERROR("Editor") << "SELFTEST: GI bake failed (baked " << giState->Baked
                                << ", entities " << giState->Entities.size()
                                << ", pages " << giState->Pages.size() << ")";
            ok = false;
        }
        // Прибираем: GI-объекты и бейк не должны влиять на остальные шаги.
        m_scene->GI.reset();
        m_scene->RemoveObject(giFloor.Id());
        m_scene->RemoveObject(giCube.Id());
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene + undo/redo + assets + "
                               << "materials + camera + light + primitives + environment + build + "
                               << "recent + dirty + play + physics + animation + config + particles + "
                               << "culling + duplicate + hierarchy + multiselect + prefab + presets + GI, "
                               << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}

// ============================================================================
//  E2E: полная игра через редактор (SAGE_EDITOR_E2E=1, headless CI)
//
//  «Без иллюзий»: РЕАЛЬНЫМИ операциями редактора (те же методы, что зовут
//  кнопки UI) создаётся проект-игра «Coin Rush» с Lua-логикой: бот сам едет к
//  монетам, собирает их через SendMessage, монеты уничтожают себя, счёт на
//  HUD обновляется из Lua (GetUI). Сцена сохраняется в .sage, ПЕРЕЧИТЫВАЕТСЯ
//  с диска, проигрывается в Play (проверяем: монеты исчезли, счёт «5/5»),
//  Stop откатывает мир, затем File > Build Game упаковывает ИСПОЛНЯЕМУЮ игру.
//  Отдельный шаг smoke-теста запускает СОБРАННЫЙ бинарник и убеждается по
//  логам/скриншоту, что та же Lua-логика работает и в нём.
// ============================================================================
void EditorLayer::RunE2EGameTest() {
    bool ok = true;
    std::error_code ec;
    fs::remove_all("e2e_game", ec);  // от прошлого прогона
    fs::remove_all("e2e_dist", ec);

    // --- 1. Проект (File > New Project) ---
    std::string err;
    if (!CreateProject(".", "e2e_game", err)) {
        LOG_ERROR("Editor") << "E2E: create project failed: " << err;
        LOG_ERROR("Editor") << "E2E: FAIL";
        return;
    }

    // --- 2. Скрипты игры — файлы в assets/scripts проекта (как из панели Assets) ---
    fs::create_directories(m_project.Dir() / "assets" / "scripts", ec);
    auto writeScript = [&](const char* name, const char* body) {
        std::ofstream f(m_project.Dir() / "assets" / "scripts" / name);
        if (!f) { ok = false; LOG_ERROR("Editor") << "E2E: can't write script " << name; }
        f << body;
    };
    // Бот: сам едет к ближайшей живой монете, вблизи шлёт ей "collect".
    writeScript("bot.lua", R"LUA(
function OnUpdate(entity, dt)
    local coin = nil
    for i = 1, 5 do
        local c = FindObject("Coin" .. i)
        if c ~= nil then coin = c break end
    end
    if coin == nil then return end
    local p = entity.Transform.Position
    local d = coin.Transform.Position - p
    d.y = 0.0
    if d:Length() < 0.45 then
        SendMessage(coin, "collect")
    else
        entity.Transform.Position = p + d:Normalized() * (2.5 * dt)
    end
end
)LUA");
    // Монета: крутится; по "collect" объявляет счёт и уничтожает себя.
    writeScript("coin.lua", R"LUA(
function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 240.0
end
function OnMessage(entity, name, data)
    if name == "collect" then
        log("E2E: coin collected: " .. entity.Name)
        Broadcast("scored")
        entity:Destroy()
    end
end
)LUA");
    // HUD: копит счёт по "scored", пишет текст и заполняет полосу из Lua.
    writeScript("hud.lua", R"LUA(
local score = 0
function OnStart(entity)
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: 0 / 5" end
end
function OnMessage(entity, name, data)
    if name ~= "scored" then return end
    score = score + 1
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: " .. score .. " / 5" end
    local bar = FindObject("Score Bar")
    if bar ~= nil then bar:GetUI().Value = score / 5.0 end
    if score >= 5 then log("E2E: ALL COINS COLLECTED") end
end
)LUA");

    // --- 3. Сцена игры — редакторскими операциями создания сущностей ---
    NewScene(false);
    m_scene->Lighting.Skybox.Enabled = true;

    GameObject ground = CreatePrimitiveEntity("Ground", MeshRef::Type::Cube);
    ground.GetTransform().Position = {0.0f, -0.55f, 0.0f};
    ground.GetTransform().Scale = {14.0f, 0.3f, 6.0f};
    ground.Renderer().Color = {0.28f, 0.30f, 0.34f};

    GameObject bot = CreatePrimitiveEntity("Bot", MeshRef::Type::Cube);
    bot.GetTransform().Position = {-6.0f, 0.1f, 0.0f};
    bot.GetTransform().Scale = {0.8f, 0.8f, 0.8f};
    bot.Renderer().Color = {0.30f, 0.55f, 0.95f};
    m_scene->Registry().emplace<ScriptComponent>(bot.Entity(), ScriptComponent{"assets/scripts/bot.lua"});

    for (int i = 1; i <= 5; ++i) {
        GameObject coin = CreatePrimitiveEntity("Coin" + std::to_string(i), MeshRef::Type::Sphere);
        coin.GetTransform().Position = {-4.0f + (float)i * 1.6f, 0.35f, 0.0f};
        coin.GetTransform().Scale = {0.45f, 0.45f, 0.45f};
        coin.Renderer().Color = {0.95f, 0.80f, 0.20f};
        m_scene->Registry().emplace<ScriptComponent>(coin.Entity(), ScriptComponent{"assets/scripts/coin.lua"});
    }

    GameObject cam = m_scene->CreateObject("Main Camera");
    cam.GetTransform().Position = {0.0f, 2.5f, 8.5f};
    cam.GetTransform().Rotation = {-12.0f, 0.0f, 0.0f};
    m_scene->Registry().emplace<CameraComponent>(cam.Entity());

    GameObject hud = m_scene->CreateObject("HUD");
    UIElementComponent hudUi;
    hudUi.Type = UIElementComponent::Kind::Panel;
    hudUi.Offset = {16.0f, 16.0f};
    hudUi.Size = {240.0f, 64.0f};
    hudUi.Rounding = 12.0f;
    hudUi.BorderThickness = 2.0f;
    hudUi.Text = "Score: 0 / 5";
    hudUi.TextCentered = false;
    m_scene->Registry().emplace<UIElementComponent>(hud.Entity(), hudUi);
    m_scene->Registry().emplace<ScriptComponent>(hud.Entity(), ScriptComponent{"assets/scripts/hud.lua"});

    GameObject bar = m_scene->CreateObject("Score Bar");
    UIElementComponent barUi;
    barUi.Type = UIElementComponent::Kind::Bar;
    barUi.Anchor = UIAnchor::BottomLeft;
    barUi.Offset = {12.0f, 8.0f};
    barUi.Size = {216.0f, 16.0f};
    barUi.Rounding = 7.0f;
    barUi.Color = {0.0f, 0.0f, 0.0f, 0.55f};
    barUi.Value = 0.0f;
    barUi.BarFillColor = {0.95f, 0.80f, 0.20f, 1.0f};
    m_scene->Registry().emplace<UIElementComponent>(bar.Entity(), barUi);
    m_scene->SetParent(bar.Entity(), hud.Entity());

    // --- 4. Сохранить и ПЕРЕЧИТАТЬ с диска: играем то, что реально в файле ---
    fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) {
        LOG_ERROR("Editor") << "E2E: scene save/reload failed";
        ok = false;
    }

    // --- 5. Play: логика Lua реально играет (бот собирает все монеты) ---
    if (ok) {
        StartPlay();
        for (int i = 0; i < 240 && ok; ++i) m_playScripts->UpdateAll(0.05f); // ~12 c игры
        bool coinsGone = true;
        for (int i = 1; i <= 5; ++i)
            if (m_scene->FindByName("Coin" + std::to_string(i)).Valid()) coinsGone = false;
        const UIElementComponent* hudNow =
            m_scene->Registry().try_get<UIElementComponent>(m_scene->FindByName("HUD").Entity());
        bool scoreOk = hudNow && hudNow->Text == "Score: 5 / 5";
        const UIElementComponent* barNow =
            m_scene->Registry().try_get<UIElementComponent>(m_scene->FindByName("Score Bar").Entity());
        bool barOk = barNow && std::abs(barNow->Value - 1.0f) < 0.001f;
        if (!coinsGone || !scoreOk || !barOk) {
            LOG_ERROR("Editor") << "E2E: play logic failed (coinsGone=" << coinsGone
                                << " scoreOk=" << scoreOk << " barOk=" << barOk
                                << " hudText='" << (hudNow ? hudNow->Text : "?") << "')";
            ok = false;
        }
        StopPlay();
        // Stop обязан вернуть мир: монеты на месте, счёт исходный.
        if (ok && (!m_scene->FindByName("Coin1").Valid() || !m_scene->FindByName("Coin5").Valid())) {
            LOG_ERROR("Editor") << "E2E: stop did not restore coins";
            ok = false;
        }
    }

    // --- 6. File > Build Game: упаковка ИСПОЛНЯЕМОЙ игры ---
    if (ok) {
        std::string buildErr;
        if (!BuildGame("e2e_dist", buildErr)) {
            LOG_ERROR("Editor") << "E2E: build game failed: " << buildErr;
            ok = false;
        } else {
#ifdef _WIN32
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game.exe";
#else
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game";
#endif
            if (!fs::exists(exe, ec)) {
                LOG_ERROR("Editor") << "E2E: built game exe missing: " << exe.string();
                ok = false;
            }
        }
    }

    if (ok) LOG_INFO("Editor") << "E2E: PASS (project + lua scripts + scene save/reload + "
                                  "play logic [5 coins, messaging, HUD from Lua] + stop restore + build exe)";
    else LOG_ERROR("Editor") << "E2E: FAIL";
}
