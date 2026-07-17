#include "EditorLayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "sage/core/Application.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

namespace fs = std::filesystem;

namespace {

// Раскладывает мировую матрицу обратно в Transform (Position/Rotation/Scale).
// ВАЖНО: порядок углов должен совпадать с Transform::GetMatrix (T*Rx*Ry*Rz*S),
// поэтому используется glm::extractEulerAngleXYZ, а не декомпозиция ImGuizmo
// (у неё другой порядок осей — гизмо «прыгал» бы на повёрнутых объектах).
void DecomposeToTransform(const glm::mat4& m, Transform& out) {
    out.Position = glm::vec3(m[3]);

    glm::vec3 scale(glm::length(glm::vec3(m[0])),
                    glm::length(glm::vec3(m[1])),
                    glm::length(glm::vec3(m[2])));
    // защита от вырожденного масштаба (деление на ноль при нормализации)
    scale = glm::max(scale, glm::vec3(1e-6f));
    out.Scale = scale;

    glm::mat4 rot(1.0f);
    rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
    rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
    rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);

    float rx, ry, rz;
    glm::extractEulerAngleXYZ(rot, rx, ry, rz);
    out.Rotation = glm::degrees(glm::vec3(rx, ry, rz));
}

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

// Шрифт с кириллицей: дефолтный ProggyClean не содержит кириллических глифов
// (русский текст рисовался бы как '???'). Пробуем известные системные шрифты;
// приоритет — assets/fonts/editor.ttf, чтобы можно было вложить свой шрифт в
// поставку редактора. Если ничего не нашлось — остаёмся на дефолтном (ASCII).
void LoadEditorFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "assets/fonts/editor.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* path : candidates) {
        FILE* probe = std::fopen(path, "rb");
        if (!probe) continue;
        std::fclose(probe);
        io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        return;
    }
}

} // namespace

// ============================================================================
//  Жизненный цикл
// ============================================================================

void EditorLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // --- ImGui (docking) ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "sage_editor_imgui.ini"; // своё имя, чтобы не пересекаться с другими ImGui-приложениями
    LoadEditorFont();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;

    m_gizmoOp = (int)ImGuizmo::TRANSLATE;

    // --- Сток лога -> панель Console (снимается в OnDetach) ---
    Log::SetSink([this](LogLevel level, const std::string& cat, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        if (m_console.size() > 2000) m_console.erase(m_console.begin(), m_console.begin() + 500);
        m_console.push_back({level, cat, msg});
    });

    // --- Ресурсы превью ---
    m_shader.emplace("assets/shaders/editor_basic.vert", "assets/shaders/editor_basic.frag");
    m_sceneFbo.emplace(m_viewportW, m_viewportH);
    m_debugDraw.emplace();
    m_cube = ResourceManager::Instance().GetCube();

    NewScene(/*withDemoContent=*/true);

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Дефолтная папка диалогов — рядом с бинарником.
    std::snprintf(m_dlgProjectDir, sizeof(m_dlgProjectDir), "%s", fs::current_path().string().c_str());
    m_assetsCwd = fs::current_path();

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);

    LOG_INFO("Editor") << "SAGE Editor started (entities: " << m_scene->Count() << ")";

    // --- Плагины (v1, только редактор — см. PluginAPI.h) ---
    fs::path pluginsDir = fs::current_path() / "plugins";
    if (const char* dir = std::getenv("SAGE_PLUGINS_DIR")) pluginsDir = dir;
    m_plugins.LoadAll(pluginsDir, m_pluginCtx);

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();

    // Авто-вход в Play при старте (визуальная проверка/CI): вешает spin.lua на
    // Green Cube демо-сцены и нажимает Play — на скриншоте куб будет повёрнут,
    // а в тулбаре гореть PLAYING.
    if (std::getenv("SAGE_EDITOR_AUTOPLAY")) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            m_selectedId = green.Id();
        }
        StartPlay();
    }
}

// Headless-проверка ядра редактора без UI-кликов (модалки недоступны в CI):
// создать проект -> сохранить сцену в его scenes/ -> очистить -> загрузить
// обратно -> сверить число сущностей. Результат — строкой PASS/FAIL в лог.
void EditorLayer::RunSelfTest() {
    size_t before = m_scene->Count();
    std::string err;
    bool ok = true;

    std::error_code ec;
    fs::remove_all("selftest_project", ec); // от прошлого прогона
    if (!m_project.CreateNew(".", "selftest_project", err)) {
        LOG_ERROR("Editor") << "SELFTEST: create project failed: " << err;
        ok = false;
    }
    if (ok) {
        m_assetsCwd = m_project.Dir();
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
        struct { AssetCreateKind kind; const char* name; const char* expect; } cases[] = {
            {AssetCreateKind::Folder, "selftest_dir", "selftest_dir"},
            {AssetCreateKind::Script, "selftest_script", "selftest_script.lua"},
            {AssetCreateKind::Material, "selftest_mat", "selftest_mat.sagemat"},
        };
        for (const auto& c : cases) {
            m_assetsCreateKind = c.kind;
            std::snprintf(m_assetsCreateName, sizeof(m_assetsCreateName), "%s", c.name);
            fs::path created;
            if (!CreatePendingAsset(created) || !fs::exists(m_assetsCwd / c.expect, ec)) {
                LOG_ERROR("Editor") << "SELFTEST: asset create failed for " << c.expect
                                    << " (" << m_dlgError << ")";
                ok = false;
                break;
            }
        }
        m_assetsCreateKind = AssetCreateKind::None;
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
                // Убираем материал, чтобы дальнейшие проверки (undo/play) шли
                // по прежнему сценарию.
                if (ok) {
                    reloaded.MaterialPath.clear();
                    reloaded.MaterialPtr = nullptr;
                }
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

    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene save/load + undo/redo + play, "
                               << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}

void EditorLayer::OnDetach() {
    Log::SetSink(nullptr); // сток ссылается на this — снять до разрушения слоя
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
    // Логика правки — событийная, живёт в панелях (камера — в Viewport, где
    // известно состояние наведения). Единственный "симуляционный" тик — Play:
    // скрипты сущностей обновляются, пока не нажата пауза.
    if (m_playState == PlayState::Playing && m_playScripts) {
        m_playScripts->UpdateAll(dt);
    }
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

    m_playState = PlayState::Playing;
    LOG_INFO("Editor") << "Play started (" << attached << " script(s) attached)";
}

void EditorLayer::StopPlay() {
    if (!InPlayMode()) return;

    // Порядок важен: ScriptEngine держит указатель на текущую сцену — гасим
    // его ДО того, как заменить сцену восстановленным снапшотом.
    m_playScripts.reset();
    RestoreSceneFromString(m_playSnapshot);
    m_playSnapshot.clear();
    m_playState = PlayState::Editing;
    LOG_INFO("Editor") << "Play stopped, scene restored";
}

// ============================================================================
//  Undo/Redo (снапшот-модель)
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
}

void EditorLayer::Undo() {
    if (InPlayMode() || m_undoStack.empty()) return;
    m_redoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_undoStack.back())) {
        m_undoStack.pop_back();
    } else {
        m_redoStack.pop_back(); // откат не удался — не ломаем историю
    }
}

void EditorLayer::Redo() {
    if (InPlayMode() || m_redoStack.empty()) return;
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_redoStack.back())) {
        m_redoStack.pop_back();
    } else {
        m_undoStack.pop_back();
    }
}

// ============================================================================
//  Сцена
// ============================================================================

GameObject EditorLayer::CreateCubeEntity(const std::string& name) {
    GameObject obj = m_scene->CreateObject(name);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    mr.MeshPtr = m_cube;
    return obj;
}

void EditorLayer::NewScene(bool withDemoContent) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_undoStack.clear();
    m_redoStack.clear();
    m_scene = std::make_unique<Scene>("Untitled");
    m_selectedId = -1;
    m_scenePath.clear();

    if (withDemoContent) {
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
        // Что-то выбрано сразу — гизмо видно, Inspector не пустой.
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) m_selectedId = green.Id();
    }
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_undoStack.clear();
        m_redoStack.clear();
        m_selectedId = -1;
        m_scenePath = path;
        LOG_INFO("Editor") << "Scene loaded: " << path.string();
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
        LOG_INFO("Editor") << "Scene saved: " << path.string();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene save failed: " << e.what();
        return false;
    }
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
    m_selectedId = copy.Id();
}

void EditorLayer::DeleteSelected() {
    if (!m_scene->Get(m_selectedId).Valid()) return;
    PushUndoSnapshot();
    m_scene->RemoveObject(m_selectedId);
    m_selectedId = -1;
}

// ============================================================================
//  Рендер превью сцены
// ============================================================================

void EditorLayer::RenderSceneToFramebuffer() {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    m_sceneFbo->Resize(m_viewportW, m_viewportH);
    m_sceneFbo->Bind();
    device.SetClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    device.Clear();

    m_view = m_camera.GetViewMatrix();
    m_proj = m_camera.GetProjectionMatrix((float)m_viewportW / (float)std::max(m_viewportH, 1));

    m_shader->Use();
    m_shader->SetMat4("uView", m_view);
    m_shader->SetMat4("uProjection", m_proj);
    sage::ecs::ForEachRenderable(*m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shader->SetMat4("uModel", tr.GetMatrix());
        m_shader->SetVec3("uObjectColor", EffectiveColor(mr)); // материал (если назначен) важнее Color
        mr.MeshPtr->Draw();
    });

    // Гизмо-графика движка (DebugDraw) — рисуется В ТОТ ЖЕ буфер после сцены,
    // с тестом глубины: объекты заслоняют сетку (раньше сетка была 2D-оверлеем
    // ImGuizmo поверх готовой картинки и просвечивала сквозь всю геометрию).
    if (m_showGrid) {
        m_debugDraw->Grid({0.0f, 0.0f, 0.0f}, 12.0f, 1.0f, {0.32f, 0.33f, 0.38f});
    }
    GameObject selectedObj = m_scene->Get(m_selectedId);
    if (selectedObj.Valid()) {
        // Подсветка выбора: каркас вокруг сущности + её оси.
        glm::mat4 model = selectedObj.GetTransform().GetMatrix();
        glm::mat4 slightlyLarger = glm::scale(model, glm::vec3(1.02f));
        m_debugDraw->WireBox(slightlyLarger, {1.0f, 0.62f, 0.12f});
        m_debugDraw->Axes(model, 1.4f);
    }
    m_debugDraw->Flush(m_view, m_proj);

    device.BindDefaultFramebuffer();
}

// ============================================================================
//  Кадр UI
// ============================================================================

void EditorLayer::OnRender() {
    sage::Application& app = sage::Application::Get();

    RenderSceneToFramebuffer();

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    DrawDockspaceAndMenu(); // включая модальные диалоги (см. DrawDialogs внутри)
    DrawHierarchyPanel();
    DrawInspectorPanel();
    DrawViewportPanel();
    DrawConsolePanel();
    DrawAssetsPanel();
    m_plugins.ImGuiAll();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        Window& win = app.GetWindow();
        SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
        app.Close();
    }
}

void EditorLayer::BuildDefaultDockLayout(unsigned int dockspaceId) {
    // Пересобираем узлы доккинга с нуля: Viewport в центре, панели вокруг.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.21f, nullptr, &center);
    ImGuiID left  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.23f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspaceId);
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

    ImGuiID dockspaceId = ImGui::GetID("SageDockSpace");
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        BuildDefaultDockLayout(dockspaceId);
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project...")) openDialog = "New Project";
            if (ImGui::MenuItem("Open Project...")) openDialog = "Open Project";
            ImGui::Separator();
            if (ImGui::MenuItem("New Scene")) NewScene(false);
            if (ImGui::MenuItem("Open Scene...")) openDialog = "Open Scene";
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                if (!m_scenePath.empty()) SaveSceneToFile(m_scenePath);
                else openDialog = "Save Scene As";
            }
            if (ImGui::MenuItem("Save Scene As...")) openDialog = "Save Scene As";
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
            if (ImGui::MenuItem("Play", nullptr, false, m_playState == PlayState::Editing)) StartPlay();
            if (ImGui::MenuItem("Pause", nullptr, false, m_playState == PlayState::Playing)) PausePlay();
            if (ImGui::MenuItem("Resume", nullptr, false, m_playState == PlayState::Paused)) ResumePlay();
            if (ImGui::MenuItem("Stop", nullptr, false, InPlayMode())) StopPlay();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Entity")) {
            if (ImGui::MenuItem("Create Empty")) {
                PushUndoSnapshot();
                m_selectedId = m_scene->CreateObject("Empty").Id();
            }
            if (ImGui::MenuItem("Create Cube")) {
                PushUndoSnapshot();
                m_selectedId = CreateCubeEntity("Cube").Id();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Reset Layout")) m_rebuildDockLayout = true;
            ImGui::MenuItem("Show Grid", nullptr, &m_showGrid);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре (плагины могут дописать свой статус через SetStatusMessage).
        std::string status = m_project.Loaded() ? ("Project: " + m_project.Name()) : "No project";
        if (!m_pluginStatusMessage.empty()) status += "  |  " + m_pluginStatusMessage;
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
//  Диалоги (модальные окна)
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
            if (m_project.CreateNew(m_dlgProjectDir, m_dlgProjectName, err)) {
                m_assetsCwd = m_project.Dir();
                NewScene(false);
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
            if (m_project.Open(m_dlgOpenPath, err)) {
                m_assetsCwd = m_project.Dir();
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

    // Rename/Delete ассета — открываются из DrawAssetTile (деферред: сама
    // плитка только выставляет m_assetsRenameTarget/m_assetsDeleteTarget,
    // OpenPopup зовётся здесь же, на уровне хоста, по тому же паттерну, что и
    // File-меню выше).
    if (!m_assetsRenameTarget.empty() && !ImGui::IsPopupOpen("Rename Asset")) {
        ImGui::OpenPopup("Rename Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", m_assetsRenameTarget.filename().string().c_str());
        bool justOpened = ImGui::IsWindowAppearing();
        if (justOpened) {
            std::string stem = m_assetsRenameTarget.filename().string();
            std::snprintf(m_assetsRenameBuf, sizeof(m_assetsRenameBuf), "%s", stem.c_str());
        }
        bool enterPressed = ImGui::InputText("New name", m_assetsRenameBuf, sizeof(m_assetsRenameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        bool doRename = enterPressed || ImGui::Button("Rename", ImVec2(120, 0));
        if (doRename) {
            fs::path target = m_assetsRenameTarget.parent_path() / m_assetsRenameBuf;
            std::error_code ec;
            fs::rename(m_assetsRenameTarget, target, ec);
            if (ec) {
                m_dlgError = "Rename failed: " + ec.message();
                LOG_ERROR("Editor") << "Asset rename failed: " << ec.message();
            } else {
                m_assetsRenameTarget.clear();
                m_dlgError.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_assetsRenameTarget.clear();
            m_dlgError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Создание ассета (New Folder / Script / Text / Material из панели Assets).
    if (m_assetsCreateKind != AssetCreateKind::None && !ImGui::IsPopupOpen("Create Asset")) {
        ImGui::OpenPopup("Create Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Create Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* kindLabel = "";
        switch (m_assetsCreateKind) {
            case AssetCreateKind::Folder:   kindLabel = "Folder"; break;
            case AssetCreateKind::Script:   kindLabel = "Lua script"; break;
            case AssetCreateKind::TextFile: kindLabel = "Text file"; break;
            case AssetCreateKind::Material: kindLabel = "Material"; break;
            default: break;
        }
        ImGui::TextDisabled("%s in %s", kindLabel, m_assetsCwd.filename().string().c_str());
        bool enterPressed = ImGui::InputText("Name", m_assetsCreateName, sizeof(m_assetsCreateName),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_dlgError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_dlgError.c_str());
        if (enterPressed || ImGui::Button("Create", ImVec2(120, 0))) {
            fs::path created;
            if (CreatePendingAsset(created)) {
                m_assetsSelected = created;
                m_assetsCreateKind = AssetCreateKind::None;
                m_dlgError.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_assetsCreateKind = AssetCreateKind::None;
            m_dlgError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_assetsDeleteTarget.empty() && !ImGui::IsPopupOpen("Delete Asset")) {
        ImGui::OpenPopup("Delete Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", m_assetsDeleteTarget.filename().string().c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            std::error_code ec;
            fs::remove_all(m_assetsDeleteTarget, ec);
            if (ec) LOG_ERROR("Editor") << "Asset delete failed: " << ec.message();
            if (m_assetsSelected == m_assetsDeleteTarget) m_assetsSelected.clear();
            m_assetsDeleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_assetsDeleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
//  Панели
// ============================================================================

void EditorLayer::DrawHierarchyPanel() {
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Scene: %s  |  Entities: %zu", m_scene->Name().c_str(), m_scene->Count());
    ImGui::Separator();

    // Стабильный порядок — сортировка по id (порядок обхода entt не гарантирован).
    std::vector<std::pair<int, std::string>> items;
    auto view = m_scene->Registry().view<IdComponent, NameComponent>();
    for (auto e : view) {
        items.push_back({view.get<IdComponent>(e).Id, view.get<NameComponent>(e).Name});
    }
    std::sort(items.begin(), items.end());

    for (auto& [id, name] : items) {
        std::string label = name + "##" + std::to_string(id);
        if (ImGui::Selectable(label.c_str(), m_selectedId == id)) m_selectedId = id;
        if (ImGui::BeginPopupContextItem()) {
            m_selectedId = id;
            if (ImGui::MenuItem("Duplicate")) DuplicateSelected();
            if (ImGui::MenuItem("Delete")) DeleteSelected();
            ImGui::EndPopup();
        }
    }

    // Контекстное меню пустого места — создание сущностей.
    if (ImGui::BeginPopupContextWindow("##hierarchy_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Empty")) { PushUndoSnapshot(); m_selectedId = m_scene->CreateObject("Empty").Id(); }
        if (ImGui::MenuItem("Create Cube")) { PushUndoSnapshot(); m_selectedId = CreateCubeEntity("Cube").Id(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// Редактор материала: правит поля РАЗДЕЛЯЕМОГО экземпляра из кэша
// ResourceManager — все сущности с этим материалом обновляются в вьюпорте
// сразу; Save фиксирует значения на диск, Revert перечитывает файл.
void EditorLayer::DrawMaterialEditor() {
    std::string pathStr = m_assetsSelected.string();
    std::shared_ptr<Material> material = ResourceManager::Instance().GetMaterial(pathStr);

    ImGui::TextDisabled("Material: %s", m_assetsSelected.filename().string().c_str());
    ImGui::ColorEdit3("Albedo", &material->Albedo.x);
    ImGui::ColorEdit3("Emissive", &material->Emissive.x);
    ImGui::DragFloat("Shininess", &material->Shininess, 0.5f, 1.0f, 256.0f);
    char texBuf[512];
    std::snprintf(texBuf, sizeof(texBuf), "%s", material->TexturePath.c_str());
    if (ImGui::InputText("Texture", texBuf, sizeof(texBuf))) material->TexturePath = texBuf;
    ImGui::TextDisabled("Edits apply live to every entity using this material.");

    if (ImGui::Button("Save Material")) {
        try {
            material->SaveToFile(pathStr);
            LOG_INFO("Editor") << "Material saved: " << pathStr;
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "Material save failed: " << e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) ResourceManager::Instance().ReloadMaterial(pathStr);
}

void EditorLayer::DrawInspectorPanel() {
    ImGui::Begin("Inspector");

    // Выбранный в Assets материал редактируется здесь же — независимо от
    // того, выбрана ли сущность.
    if (m_assetsSelected.extension() == ".sagemat") {
        DrawMaterialEditor();
        ImGui::Separator();
    }

    GameObject obj = m_scene->Get(m_selectedId);
    if (!obj.Valid()) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }

    // Undo-трекинг «размазанных» правок (drag/текст): на активации виджета
    // запоминаем состояние «до», на завершении правки — кладём его в undo-стек.
    // Так всё перетаскивание DragFloat3 или набор имени = одна запись undo.
    auto trackLastItem = [this]() {
        if (InPlayMode()) return;
        if (ImGui::IsItemActivated()) m_pendingEditSnapshot = SceneSerializer::SaveToString(*m_scene);
        if (ImGui::IsItemDeactivatedAfterEdit() && !m_pendingEditSnapshot.empty()) {
            constexpr size_t kMaxUndoEntries = 100;
            if (m_undoStack.size() >= kMaxUndoEntries) m_undoStack.erase(m_undoStack.begin());
            m_undoStack.push_back(m_pendingEditSnapshot);
            m_pendingEditSnapshot.clear();
            m_redoStack.clear();
        }
    };

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) obj.SetName(buf);
    trackLastItem();
    ImGui::TextDisabled("Id: %d", obj.Id());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& tr = obj.GetTransform();
        ImGui::DragFloat3("Position", &tr.Position.x, 0.05f); trackLastItem();
        ImGui::DragFloat3("Rotation", &tr.Rotation.x, 0.5f); trackLastItem();
        ImGui::DragFloat3("Scale", &tr.Scale.x, 0.05f, 0.01f, 100.0f); trackLastItem();
    }

    if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        ImGui::ColorEdit3("Color", &mr.Color.x); trackLastItem();

        const char* kinds[] = {"None", "Cube", "Model"};
        int kind = (int)mr.Ref.type;
        if (ImGui::Combo("Mesh", &kind, kinds, 3)) {
            PushUndoSnapshot(); // дискретное изменение — прямая запись undo
            mr.Ref.type = (MeshRef::Type)kind;
            if (mr.Ref.type == MeshRef::Type::Cube) { mr.Ref.path.clear(); mr.MeshPtr = m_cube; }
            if (mr.Ref.type == MeshRef::Type::None) { mr.Ref.path.clear(); mr.MeshPtr = nullptr; }
            // Model — путь задаётся ниже и грузится по кнопке Load.
        }
        if (mr.Ref.type == MeshRef::Type::Model) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", mr.Ref.path.c_str());
            if (ImGui::InputText("Path", pathBuf, sizeof(pathBuf))) mr.Ref.path = pathBuf;
            trackLastItem();
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                try {
                    mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
                } catch (const std::exception& e) {
                    LOG_ERROR("Editor") << "Model load failed: " << e.what();
                }
            }
        }
    }

    // --- Материал (.sagemat): заменяет Color, общий для всех сущностей с ним ---
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        if (mr.MaterialPath.empty()) {
            ImGui::TextDisabled("No material (entity uses Color above)");
        } else {
            if (mr.MaterialPtr) {
                ImGui::ColorButton("##mat_preview", ImVec4(mr.MaterialPtr->Albedo.r, mr.MaterialPtr->Albedo.g,
                                                            mr.MaterialPtr->Albedo.b, 1.0f));
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%s", mr.MaterialPath.c_str());
        }

        // Назначение: выбери .sagemat в Assets (клик по тайлу) — тут появится
        // кнопка Assign. Отдельного файлового диалога нет намеренно: панель
        // Assets и есть браузер файлов проекта.
        if (m_assetsSelected.extension() == ".sagemat") {
            std::string label = "Assign \"" + m_assetsSelected.filename().string() + "\"";
            if (ImGui::Button(label.c_str())) {
                PushUndoSnapshot();
                mr.MaterialPath = m_assetsSelected.string();
                mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
            }
        } else {
            ImGui::TextDisabled("Select a .sagemat in Assets to assign it");
        }
        if (!mr.MaterialPath.empty()) {
            if (ImGui::Button("Clear Material")) {
                PushUndoSnapshot();
                mr.MaterialPath.clear();
                mr.MaterialPtr = nullptr;
            }
        }
    }

    // --- Скрипт (поведение в Play-режиме) ---
    if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        entt::registry& reg = m_scene->Registry();
        if (ScriptComponent* sc = reg.try_get<ScriptComponent>(obj.Entity())) {
            char scriptBuf[512];
            std::snprintf(scriptBuf, sizeof(scriptBuf), "%s", sc->Path.c_str());
            if (ImGui::InputText("Lua file", scriptBuf, sizeof(scriptBuf))) sc->Path = scriptBuf;
            trackLastItem();
            ImGui::TextDisabled("Runs in Play mode: OnStart(entity), OnUpdate(entity, dt)");
            if (ImGui::Button("Remove Script")) {
                PushUndoSnapshot();
                reg.remove<ScriptComponent>(obj.Entity());
            }
        } else {
            if (ImGui::Button("Add Script")) {
                PushUndoSnapshot();
                reg.emplace<ScriptComponent>(obj.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            }
        }
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Delete Entity", ImVec2(-1, 0))) DeleteSelected();
    ImGui::PopStyleColor();
    ImGui::End();
}

void EditorLayer::PickEntityAtViewportPos(float u, float v) {
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
    m_selectedId = bestId; // клик мимо всех объектов — снять выбор
}

void EditorLayer::DrawViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    m_viewportHovered = ImGui::IsWindowHovered();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x >= 8 && avail.y >= 8) {
        m_viewportW = (int)avail.x;
        m_viewportH = (int)avail.y;
    }
    ImVec2 imgPos = ImGui::GetCursorScreenPos();

    // Текстура OpenGL идёт снизу-вверх — переворот по V.
    ImTextureID tex = (ImTextureID)(std::intptr_t)m_sceneFbo->ColorTexture();
    ImGui::Image(tex, avail, ImVec2(0, 1), ImVec2(1, 0));

    ImGuiIO& io = ImGui::GetIO();
    float dt = sage::Application::Get().DeltaTime();

    // --- Камера: ПКМ — осмотр, ПКМ+WASDQE — полёт, Shift — ускорение ---
    bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if ((m_viewportHovered || m_cameraDriving) && rmb) {
        m_cameraDriving = true;
        m_camera.ProcessMouse(io.MouseDelta.x, -io.MouseDelta.y);
        float speed = m_camera.MovementSpeed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) m_camera.Position += m_camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) m_camera.Position -= m_camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) m_camera.Position -= m_camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) m_camera.Position += m_camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) m_camera.Position += m_camera.WorldUp * speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) m_camera.Position -= m_camera.WorldUp * speed;
    } else {
        m_cameraDriving = false;
    }
    if (m_viewportHovered && io.MouseWheel != 0.0f) {
        m_camera.Position += m_camera.Front * io.MouseWheel * 0.8f;
    }

    // --- Хоткеи гизмо (не во время полёта камеры и не в полях ввода) ---
    if (m_viewportHovered && !m_cameraDriving && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOp = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOp = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = (int)ImGuizmo::SCALE;
    }

    // --- ImGuizmo: манипулятор выбранной сущности. Сетка-ориентир больше НЕ
    // рисуется здесь: ImGuizmo::DrawGrid — 2D-оверлей без теста глубины,
    // просвечивавший сквозь объекты; теперь сетка — 3D-линии движкового
    // DebugDraw внутри RenderSceneToFramebuffer (заслоняется геометрией). ---
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgPos.x, imgPos.y, avail.x, avail.y);

    GameObject selected = m_scene->Get(m_selectedId);
    if (selected.Valid()) {
        Transform& tr = selected.GetTransform();
        glm::mat4 model = tr.GetMatrix();

        // Пока гизмо не тащат, но курсор над ним — запоминаем состояние «до»:
        // первый же кадр перетаскивания уже мутирует Transform, поэтому снапшот
        // должен быть взят раньше него.
        if (!ImGuizmo::IsUsing() && ImGuizmo::IsOver() && !InPlayMode()) {
            m_gizmoPendingSnapshot = SceneSerializer::SaveToString(*m_scene);
        }

        float snapT = 0.5f, snapR = 15.0f, snapS = 0.1f;
        float snapValues[3];
        auto op = (ImGuizmo::OPERATION)m_gizmoOp;
        float snapUnit = (op == ImGuizmo::ROTATE) ? snapR : (op == ImGuizmo::SCALE ? snapS : snapT);
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;

        if (ImGuizmo::Manipulate(glm::value_ptr(m_view), glm::value_ptr(m_proj),
                                 op, ImGuizmo::LOCAL, glm::value_ptr(model),
                                 nullptr, m_snap ? snapValues : nullptr)) {
            DecomposeToTransform(model, tr);
        }

        // Фронт «начали таскать»: одна запись undo на всё перетаскивание.
        bool usingNow = ImGuizmo::IsUsing();
        if (usingNow && !m_gizmoWasUsing && !InPlayMode() && !m_gizmoPendingSnapshot.empty()) {
            constexpr size_t kMaxUndoEntries = 100;
            if (m_undoStack.size() >= kMaxUndoEntries) m_undoStack.erase(m_undoStack.begin());
            m_undoStack.push_back(m_gizmoPendingSnapshot);
            m_redoStack.clear();
        }
        m_gizmoWasUsing = usingNow;
    } else {
        m_gizmoWasUsing = false;
    }

    // --- Пикинг ЛКМ (не по гизмо и не во время манипуляции) ---
    if (m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        ImVec2 mp = ImGui::GetMousePos();
        float u = (mp.x - imgPos.x) / avail.x;
        float v = (mp.y - imgPos.y) / avail.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) PickEntityAtViewportPos(u, v);
    }

    // --- Оверлей-тулбар в углу вьюпорта: режим гизмо + snap ---
    ImGui::SetNextWindowPos(ImVec2(imgPos.x + 8.0f, imgPos.y + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##viewport_toolbar", nullptr, overlayFlags)) {
        auto modeButton = [this](const char* label, ImGuizmo::OPERATION op) {
            bool active = m_gizmoOp == (int)op;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.46f, 0.80f, 1.0f));
            if (ImGui::SmallButton(label)) m_gizmoOp = (int)op;
            if (active) ImGui::PopStyleColor();
        };
        modeButton("Move (W)", ImGuizmo::TRANSLATE); ImGui::SameLine();
        modeButton("Rotate (E)", ImGuizmo::ROTATE); ImGui::SameLine();
        modeButton("Scale (R)", ImGuizmo::SCALE); ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_snap);

        // --- Play-режим ---
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (m_playState == PlayState::Editing) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
            if (ImGui::SmallButton("Play")) StartPlay();
            ImGui::PopStyleColor();
        } else {
            if (m_playState == PlayState::Playing) {
                if (ImGui::SmallButton("Pause")) PausePlay();
            } else {
                if (ImGui::SmallButton("Resume")) ResumePlay();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
            if (ImGui::SmallButton("Stop")) StopPlay();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(m_playState == PlayState::Playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                                                 : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                               m_playState == PlayState::Playing ? "PLAYING" : "PAUSED");
        }
    }
    ImGui::End();

    ImGui::End(); // Viewport
    ImGui::PopStyleVar();
}

void EditorLayer::DrawConsolePanel() {
    ImGui::Begin("Console");
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        m_console.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_consoleAutoScroll);
    ImGui::Separator();

    ImGui::BeginChild("##console_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        for (const ConsoleEntry& e : m_console) {
            ImVec4 color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            if (e.Level == LogLevel::Warn) color = ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
            if (e.Level == LogLevel::Error) color = ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
            if (e.Level <= LogLevel::Debug) color = ImVec4(0.55f, 0.60f, 0.65f, 1.0f);
            ImGui::TextColored(color, "[%s] %s", e.Category.c_str(), e.Message.c_str());
        }
    }
    if (m_consoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

namespace {

// Цветная плашка + короткий тег по типу файла — свой минимализм панели Assets
// вместо иконочного шрифта: ImDrawList::AddRectFilled с закруглением плюс
// 2-4 символа расширения по центру.
struct AssetStyle { ImVec4 Color; std::string Tag; };

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AssetStyle StyleForPath(const fs::path& path, bool isDir) {
    if (isDir) return { ImVec4(0.80f, 0.60f, 0.20f, 1.0f), "DIR" };
    std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return { ImVec4(0.25f, 0.45f, 0.85f, 1.0f), "SCENE" };
    if (ext == ".sagemat") return { ImVec4(0.75f, 0.45f, 0.20f, 1.0f), "MAT" };
    if (ext == ".lua") return { ImVec4(0.25f, 0.70f, 0.30f, 1.0f), "LUA" };
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return { ImVec4(0.55f, 0.35f, 0.80f, 1.0f), "MESH" };
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return { ImVec4(0.20f, 0.65f, 0.65f, 1.0f), ext.substr(1) };
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return { ImVec4(0.80f, 0.30f, 0.55f, 1.0f), "AUDIO" };
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return { ImVec4(0.50f, 0.50f, 0.55f, 1.0f), "GLSL" };
    std::string tag = ext.empty() ? "FILE" : ToLower(ext.substr(1));
    return { ImVec4(0.45f, 0.45f, 0.48f, 1.0f), tag };
}

// Обрезает строку многоточием посередине справа, чтобы уместиться в maxWidth.
std::string TruncateToWidth(const std::string& s, float maxWidth) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    const char* ellipsis = "...";
    float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    std::string out;
    for (size_t n = 1; n <= s.size(); ++n) {
        std::string candidate = s.substr(0, n);
        if (ImGui::CalcTextSize(candidate.c_str()).x + ellipsisW > maxWidth) {
            out = s.substr(0, n > 1 ? n - 1 : 1);
            break;
        }
        out = candidate;
    }
    return out + ellipsis;
}

constexpr float kTileW = 76.0f;
constexpr float kTileH = 64.0f;
constexpr float kTileSpacing = 10.0f;

} // namespace

void EditorLayer::DrawAssetsBreadcrumb() {
    fs::path root = m_project.Loaded() ? m_project.Dir().parent_path() : m_assetsCwd.root_path();

    // Собираем цепочку сегментов от текущей папки вверх до корня.
    std::vector<fs::path> chain;
    for (fs::path p = m_assetsCwd; p != root && p.has_parent_path(); p = p.parent_path()) {
        chain.push_back(p);
        if (p.parent_path() == p) break; // достигли корня файловой системы
    }
    std::reverse(chain.begin(), chain.end());

    for (size_t i = 0; i < chain.size(); ++i) {
        std::string seg = chain[i].filename().string();
        if (seg.empty()) seg = chain[i].string();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(seg.c_str())) m_assetsCwd = chain[i];
        ImGui::PopID();
        if (i + 1 < chain.size()) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/"); ImGui::SameLine(0, 2); }
    }
}

void EditorLayer::DrawAssetTile(const fs::path& path, bool isDir) {
    AssetStyle style = StyleForPath(path, isDir);
    std::string filename = path.filename().string();

    ImGui::PushID(filename.c_str());
    ImGui::BeginGroup();

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 swatchSize(kTileW, kTileW * 0.72f);
    ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
    bool hovered = ImGui::IsItemHovered();
    bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool isSelected = m_assetsSelected == path;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 fillColor = style.Color;
    if (!hovered) { fillColor.x *= 0.85f; fillColor.y *= 0.85f; fillColor.z *= 0.85f; }
    dl->AddRectFilled(cursor, ImVec2(cursor.x + swatchSize.x, cursor.y + swatchSize.y),
                       ImGui::ColorConvertFloat4ToU32(fillColor), 6.0f);
    if (isSelected) {
        dl->AddRect(cursor, ImVec2(cursor.x + swatchSize.x, cursor.y + swatchSize.y),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.9f)), 6.0f, 0, 2.0f);
    }
    std::string tagUpper = style.Tag;
    for (char& c : tagUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ImVec2 tagSize = ImGui::CalcTextSize(tagUpper.c_str());
    ImVec2 tagPos(cursor.x + (swatchSize.x - tagSize.x) * 0.5f, cursor.y + (swatchSize.y - tagSize.y) * 0.5f);
    dl->AddText(tagPos, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.95f)), tagUpper.c_str());

    std::string label = TruncateToWidth(filename, kTileW);
    ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 labelPos(cursor.x + (kTileW - labelSize.x) * 0.5f, cursor.y + swatchSize.y + 4.0f);
    dl->AddText(labelPos, ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f)), label.c_str());

    ImGui::EndGroup();

    if (clicked) m_assetsSelected = path;
    if (doubleClicked) {
        if (isDir) m_assetsCwd = path;
        else if (path.extension() == ".sage") LoadSceneFromFile(path);
    }
    if (ImGui::BeginPopupContextItem("##tile_ctx")) {
        m_assetsSelected = path;
        if (ImGui::MenuItem("Rename")) { m_assetsRenameTarget = path; m_dlgError.clear(); }
        if (ImGui::MenuItem("Delete")) { m_assetsDeleteTarget = path; }
        ImGui::EndPopup();
    }
    if (hovered && !filename.empty() && label != filename) ImGui::SetTooltip("%s", filename.c_str());

    ImGui::PopID();
}

void EditorLayer::DrawAssetsPanel() {
    ImGui::Begin("Assets");

    if (!m_project.Loaded()) {
        ImGui::TextDisabled("No project open.");
        ImGui::TextDisabled("File > New Project... to create one; browsing current dir:");
    }

    fs::path root = m_project.Loaded() ? m_project.Dir() : fs::path("/");
    bool canGoUp = m_assetsCwd.has_parent_path() && m_assetsCwd != root;
    if (canGoUp && ImGui::SmallButton("Up")) m_assetsCwd = m_assetsCwd.parent_path();
    ImGui::SameLine();
    DrawAssetsBreadcrumb();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##assets_search", "Search...", m_assetsSearch, sizeof(m_assetsSearch));
    ImGui::Separator();

    ImGui::BeginChild("##assets_scroll");
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& entry : fs::directory_iterator(m_assetsCwd, ec)) {
        (entry.is_directory(ec) ? dirs : files).push_back(entry);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    std::string filter = ToLower(m_assetsSearch);
    auto matches = [&](const fs::path& p) {
        if (filter.empty()) return true;
        return ToLower(p.filename().string()).find(filter) != std::string::npos;
    };

    float availWidth = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>(availWidth / (kTileW + kTileSpacing)));
    int col = 0;
    bool any = false;
    for (const auto& d : dirs) {
        if (!matches(d.path())) continue;
        any = true;
        if (col > 0) ImGui::SameLine(0, kTileSpacing);
        DrawAssetTile(d.path(), true);
        col = (col + 1) % columns;
    }
    for (const auto& f : files) {
        if (!matches(f.path())) continue;
        any = true;
        if (col > 0) ImGui::SameLine(0, kTileSpacing);
        DrawAssetTile(f.path(), false);
        col = (col + 1) % columns;
    }
    if (!any) ImGui::TextDisabled("(empty)");

    // Создание ассетов — ПКМ по пустому месту (не по тайлу: у тайлов своё
    // контекстное меню). Меню только запоминает вид создаваемого ассета —
    // модалка имени открывается в DrawDialogs (деферред-паттерн).
    if (ImGui::BeginPopupContextWindow("##assets_create_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        auto startCreate = [this](AssetCreateKind kind, const char* defaultName) {
            m_assetsCreateKind = kind;
            std::snprintf(m_assetsCreateName, sizeof(m_assetsCreateName), "%s", defaultName);
            m_dlgError.clear();
        };
        if (ImGui::MenuItem("New Folder")) startCreate(AssetCreateKind::Folder, "NewFolder");
        if (ImGui::MenuItem("New Script (.lua)")) startCreate(AssetCreateKind::Script, "new_script");
        if (ImGui::MenuItem("New Text File (.txt)")) startCreate(AssetCreateKind::TextFile, "notes");
        if (ImGui::MenuItem("New Material (.sagemat)")) startCreate(AssetCreateKind::Material, "NewMaterial");
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::End();
}

// Создаёт ассет вида m_assetsCreateKind с именем m_assetsCreateName в текущей
// папке панели. Расширение дописывается автоматически, если не указано.
// При ошибке возвращает false и кладёт причину в m_dlgError.
bool EditorLayer::CreatePendingAsset(fs::path& outCreatedPath) {
    std::string name = m_assetsCreateName;
    if (name.empty()) {
        m_dlgError = "Name must not be empty";
        return false;
    }

    const char* wantExt = nullptr;
    switch (m_assetsCreateKind) {
        case AssetCreateKind::Script:   wantExt = ".lua"; break;
        case AssetCreateKind::TextFile: wantExt = ".txt"; break;
        case AssetCreateKind::Material: wantExt = ".sagemat"; break;
        default: break;
    }
    if (wantExt && fs::path(name).extension() != wantExt) name += wantExt;

    fs::path target = m_assetsCwd / name;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        m_dlgError = "Already exists: " + name;
        return false;
    }

    if (m_assetsCreateKind == AssetCreateKind::Folder) {
        if (!fs::create_directory(target, ec) || ec) {
            m_dlgError = "Create folder failed: " + ec.message();
            return false;
        }
    } else {
        std::string content;
        if (m_assetsCreateKind == AssetCreateKind::Script) {
            content =
                "-- " + name + " — скрипт сущности SAGE.\n"
                "-- OnStart(entity) вызывается один раз при привязке скрипта,\n"
                "-- OnUpdate(entity, dt) — каждый кадр Play-режима/игры.\n"
                "\n"
                "function OnStart(entity)\n"
                "end\n"
                "\n"
                "function OnUpdate(entity, dt)\n"
                "    -- entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0\n"
                "end\n";
        } else if (m_assetsCreateKind == AssetCreateKind::Material) {
            content =
                "{\n"
                "    \"albedo\": [1.0, 1.0, 1.0],\n"
                "    \"emissive\": [0.0, 0.0, 0.0],\n"
                "    \"shininess\": 32.0,\n"
                "    \"texture\": \"\"\n"
                "}\n";
        }
        std::ofstream out(target);
        if (!out) {
            m_dlgError = "Create file failed: " + target.string();
            return false;
        }
        out << content;
    }

    LOG_INFO("Editor") << "Asset created: " << target.string();
    outCreatedPath = target;
    return true;
}
