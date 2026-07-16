#include "EditorLayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

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

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();
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
    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene save/load, " << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}

void EditorLayer::OnDetach() {
    Log::SetSink(nullptr); // сток ссылается на this — снять до разрушения слоя
    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    ResourceManager::Instance().Clear();
}

void EditorLayer::OnUpdate(float /*dt*/) {
    // Вся логика редактора событийная и живёт в панелях (камера — в Viewport,
    // где известно состояние наведения); отдельного тика симуляции нет.
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
    try {
        m_scene = SceneSerializer::Load(path.string());
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
    GameObject copy = m_scene->CreateObject(src.Name() + " Copy");
    copy.GetTransform() = src.GetTransform();
    copy.GetTransform().Position.x += 0.5f; // сдвиг, чтобы копия не сливалась с оригиналом
    MeshRendererComponent& mr = copy.Renderer();
    mr = src.Renderer();
    m_selectedId = copy.Id();
}

void EditorLayer::DeleteSelected() {
    if (m_selectedId < 0) return;
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
        m_shader->SetVec3("uObjectColor", mr.Color);
        mr.MeshPtr->Draw();
    });

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
            bool hasSel = m_scene->Get(m_selectedId).Valid();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) DuplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Entity")) {
            if (ImGui::MenuItem("Create Empty")) {
                GameObject obj = m_scene->CreateObject("Empty");
                m_selectedId = obj.Id();
            }
            if (ImGui::MenuItem("Create Cube")) {
                GameObject obj = CreateCubeEntity("Cube");
                m_selectedId = obj.Id();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Reset Layout")) m_rebuildDockLayout = true;
            ImGui::MenuItem("Show Grid", nullptr, &m_showGrid);
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

    ImGui::End();

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_scenePath.empty()) SaveSceneToFile(m_scenePath);
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
        if (ImGui::MenuItem("Create Empty")) m_selectedId = m_scene->CreateObject("Empty").Id();
        if (ImGui::MenuItem("Create Cube")) m_selectedId = CreateCubeEntity("Cube").Id();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorLayer::DrawInspectorPanel() {
    ImGui::Begin("Inspector");
    GameObject obj = m_scene->Get(m_selectedId);
    if (!obj.Valid()) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) obj.SetName(buf);
    ImGui::TextDisabled("Id: %d", obj.Id());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& tr = obj.GetTransform();
        ImGui::DragFloat3("Position", &tr.Position.x, 0.05f);
        ImGui::DragFloat3("Rotation", &tr.Rotation.x, 0.5f);
        ImGui::DragFloat3("Scale", &tr.Scale.x, 0.05f, 0.01f, 100.0f);
    }

    if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        ImGui::ColorEdit3("Color", &mr.Color.x);

        const char* kinds[] = {"None", "Cube", "Model"};
        int kind = (int)mr.Ref.type;
        if (ImGui::Combo("Mesh", &kind, kinds, 3)) {
            mr.Ref.type = (MeshRef::Type)kind;
            if (mr.Ref.type == MeshRef::Type::Cube) { mr.Ref.path.clear(); mr.MeshPtr = m_cube; }
            if (mr.Ref.type == MeshRef::Type::None) { mr.Ref.path.clear(); mr.MeshPtr = nullptr; }
            // Model — путь задаётся ниже и грузится по кнопке Load.
        }
        if (mr.Ref.type == MeshRef::Type::Model) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", mr.Ref.path.c_str());
            if (ImGui::InputText("Path", pathBuf, sizeof(pathBuf))) mr.Ref.path = pathBuf;
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

    // --- ImGuizmo: сетка-ориентир и манипулятор выбранной сущности ---
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgPos.x, imgPos.y, avail.x, avail.y);

    if (m_showGrid) {
        glm::mat4 identity(1.0f);
        ImGuizmo::DrawGrid(glm::value_ptr(m_view), glm::value_ptr(m_proj), glm::value_ptr(identity), 12.0f);
    }

    GameObject selected = m_scene->Get(m_selectedId);
    if (selected.Valid()) {
        Transform& tr = selected.GetTransform();
        glm::mat4 model = tr.GetMatrix();

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

void EditorLayer::DrawAssetsPanel() {
    ImGui::Begin("Assets");

    if (!m_project.Loaded()) {
        ImGui::TextDisabled("No project open.");
        ImGui::TextDisabled("File > New Project... to create one; browsing current dir:");
    }

    // Навигация: не даём подняться выше корня проекта (если проект открыт).
    fs::path root = m_project.Loaded() ? m_project.Dir() : fs::path("/");
    ImGui::TextDisabled("%s", m_assetsCwd.string().c_str());
    bool canGoUp = m_assetsCwd.has_parent_path() && m_assetsCwd != root;
    if (canGoUp && ImGui::SmallButton("[..] Up")) m_assetsCwd = m_assetsCwd.parent_path();
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

    for (const auto& d : dirs) {
        std::string label = "[dir] " + d.path().filename().string();
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_assetsCwd = d.path();
        }
    }
    for (const auto& f : files) {
        bool isScene = f.path().extension() == ".sage";
        std::string label = (isScene ? "[scene] " : "") + f.path().filename().string();
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && isScene) {
            LoadSceneFromFile(f.path());
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
