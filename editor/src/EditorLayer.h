#pragma once
#include <optional>
#include <memory>
#include <string>

#include "sage/core/Layer.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// EditorLayer — сам редактор SAGE поверх движка. Рендерит ECS-сцену в offscreen
// кадровый буфер и показывает его в ImGui-панели Viewport; панели Hierarchy и
// Inspector правят сущности/компоненты живьём через ту же ECS, что и игры. Это
// доказывает разделение: редактор — обычное приложение на движке (Application +
// слой), работает с движковой Scene, ничего не зная о конкретной игре.
// ---------------------------------------------------------------------------
class EditorLayer : public sage::Layer {
public:
    EditorLayer() : sage::Layer("Editor") {}

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    void RenderSceneToFramebuffer();
    void DrawDockspaceAndMenu();
    void DrawHierarchyPanel();
    void DrawInspectorPanel();
    void DrawViewportPanel();

    Scene m_scene;
    Camera m_camera;
    std::optional<Shader> m_shader;
    std::optional<Framebuffer> m_sceneFbo;
    std::shared_ptr<Mesh> m_cube;

    int m_selectedId = -1;              // id выбранной сущности (-1 — нет)
    int m_viewportW = 1280;             // текущий размер offscreen-буфера сцены
    int m_viewportH = 720;
    bool m_imguiReady = false;          // ImGui успешно инициализирован (для OnDetach)

    // Авто-скриншот для headless-проверки/CI (как в игре): по SAGE_SCREENSHOT_*.
    std::string m_screenshotPath = "editor.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
