#include "EditorLayer.h"

#include <cstdint>
#include <string>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/scene/Components.h"

#include <cstdlib>

void EditorLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // --- ImGui (docking) ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // ветка docking — панели стыкуются
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;

    // --- Ресурсы рендера сцены ---
    m_shader.emplace("assets/shaders/editor_basic.vert", "assets/shaders/editor_basic.frag");
    m_sceneFbo.emplace(m_viewportW, m_viewportH);
    m_cube = ResourceManager::Instance().GetCube();

    // --- Демо-сцена: несколько сущностей через ту же ECS, что и у игр ---
    struct Def { const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale; };
    Def defs[] = {
        {"Ground",   {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}},
        {"Red Cube", {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}},
        {"Green Cube", {0.0f, 0.3f, 0.0f}, {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}},
        {"Blue Cube", {1.6f, 0.3f, 0.0f},  {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}},
        {"Tower",    {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}},
    };
    for (const Def& d : defs) {
        GameObject obj = m_scene.CreateObject(d.name);
        obj.GetTransform().Position = d.pos;
        obj.GetTransform().Scale = d.scale;
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = m_cube;
        mr.Color = d.color;
    }

    // Камера смотрит на центр сцены из угла (кубы вокруг начала координат).
    m_camera.Position = {6.0f, 5.0f, 6.0f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -30.0f;
    m_camera.ProcessMouse(0.0f, 0.0f); // пересчитать Front/Right/Up

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);

    LOG_INFO("Editor") << "SAGE Editor запущен (сущностей в сцене: " << m_scene.Count() << ")";
}

void EditorLayer::OnDetach() {
    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    ResourceManager::Instance().Clear();
}

void EditorLayer::OnUpdate(float /*dt*/) {
    // Медленный авто-облёт сцены — чтобы превью читалось как живой 3D-рендер, а
    // не статичная картинка. Позиция пересчитывается вокруг центра сцены.
    float t = (float)glfwGetTime();
    float radius = 8.5f;
    m_camera.Position = {std::cos(t * 0.25f) * radius, 5.0f, std::sin(t * 0.25f) * radius};
    // Смотрим в центр: направление на (0,1,0).
    glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) - m_camera.Position);
    m_camera.Yaw = glm::degrees(std::atan2(dir.z, dir.x));
    m_camera.Pitch = glm::degrees(std::asin(dir.y));
    m_camera.ProcessMouse(0.0f, 0.0f);
}

void EditorLayer::RenderSceneToFramebuffer() {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    m_sceneFbo->Resize(m_viewportW, m_viewportH);
    m_sceneFbo->Bind();
    device.SetClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    device.Clear();

    glm::mat4 view = m_camera.GetViewMatrix();
    glm::mat4 proj = m_camera.GetProjectionMatrix((float)m_viewportW / (float)m_viewportH);

    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);
    sage::ecs::ForEachRenderable(m_scene, [&](Transform& tr, MeshRendererComponent& mr) {
        m_shader->SetMat4("uModel", tr.GetMatrix());
        m_shader->SetVec3("uObjectColor", mr.Color);
        mr.MeshPtr->Draw();
    });

    device.BindDefaultFramebuffer();
}

void EditorLayer::OnRender() {
    sage::Application& app = sage::Application::Get();

    // 1) Рендер сцены в offscreen-буфер (его текстуру покажем в панели Viewport).
    RenderSceneToFramebuffer();

    // 2) Кадр ImGui: очищаем экран, рисуем панели.
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    DrawDockspaceAndMenu();
    DrawHierarchyPanel();
    DrawInspectorPanel();
    DrawViewportPanel();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Авто-скриншот для headless-проверки (снимает уже нарисованный кадр UI).
    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        Window& win = app.GetWindow();
        SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
        app.Close();
    }
}

void EditorLayer::DrawDockspaceAndMenu() {
    // Полноэкранный dockspace — все панели можно свободно стыковать/двигать.
    // Без аргументов — по главному вьюпорту (совместимо с версиями docking-ветки).
    ImGui::DockSpaceOverViewport();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit")) sage::Application::Get().Close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Entity")) {
            if (ImGui::MenuItem("Add Cube")) {
                GameObject obj = m_scene.CreateObject("New Cube");
                MeshRendererComponent& mr = obj.Renderer();
                mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
                mr.MeshPtr = m_cube;
                m_selectedId = obj.Id();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void EditorLayer::DrawHierarchyPanel() {
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Entities: %zu", m_scene.Count());
    ImGui::Separator();

    auto view = m_scene.Registry().view<IdComponent, NameComponent>();
    for (auto e : view) {
        int id = view.get<IdComponent>(e).Id;
        const std::string& name = view.get<NameComponent>(e).Name;
        std::string label = name + "##" + std::to_string(id);
        if (ImGui::Selectable(label.c_str(), m_selectedId == id)) {
            m_selectedId = id;
        }
    }
    ImGui::End();
}

void EditorLayer::DrawInspectorPanel() {
    ImGui::SetNextWindowPos(ImVec2(1330, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");
    GameObject obj = m_scene.Get(m_selectedId);
    if (!obj.Valid()) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }

    // Имя (редактируемое).
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) {
        obj.SetName(buf);
    }
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
        ImGui::TextDisabled("Mesh: %s", mr.Ref.type == MeshRef::Type::Cube ? "Cube"
                                       : mr.Ref.type == MeshRef::Type::Model ? mr.Ref.path.c_str() : "None");
    }
    ImGui::End();
}

void EditorLayer::DrawViewportPanel() {
    ImGui::SetNextWindowPos(ImVec2(280, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 700), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x, h = (int)avail.y;
    if (w > 0 && h > 0 && (w != m_viewportW || h != m_viewportH)) {
        m_viewportW = w;
        m_viewportH = h;
    }

    // Текстура OpenGL идёт снизу-вверх — переворачиваем по V (uv0=(0,1), uv1=(1,0)).
    ImTextureID tex = (ImTextureID)(std::intptr_t)m_sceneFbo->ColorTexture();
    ImGui::Image(tex, avail, ImVec2(0, 1), ImVec2(1, 0));

    ImGui::End();
    ImGui::PopStyleVar();
}
