#pragma once
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <filesystem>

#include "sage/core/Layer.h"
#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"
#include "Project.h"

// ---------------------------------------------------------------------------
// EditorLayer — редактор SAGE поверх движка.
//
//   • Docking: полноэкранный dockspace; раскладка по умолчанию строится через
//     DockBuilder (Hierarchy слева, Inspector справа, Console/Assets снизу,
//     Viewport в центре) — панели состыкованы из коробки и свободно
//     перетаскиваются/передокиваются. Window > Reset Layout возвращает дефолт.
//   • Гизмо (ImGuizmo): перемещение/поворот/масштаб выбранной сущности прямо
//     во вьюпорте (горячие клавиши W/E/R, привязка к сетке — Snap).
//   • Камера вьюпорта: ПКМ — осмотреться, ПКМ+WASDQE — полёт, колесо — наезд.
//   • Пикинг: клик ЛКМ по объекту во вьюпорте выбирает его (луч в ECS-сцену).
//   • Проекты: File > New/Open Project (папка + project.sageproj + scenes/ +
//     assets/); сцены сохраняются/грузятся в scenes/ проекта (.sage JSON).
//   • Панели: Hierarchy (создание/дублирование/удаление сущностей), Inspector
//     (имя/Transform/цвет/меш), Console (живой сток лога движка), Assets
//     (браузер файлов проекта, двойной клик по .sage — загрузка сцены).
// ---------------------------------------------------------------------------
class EditorLayer : public sage::Layer {
public:
    EditorLayer() : sage::Layer("Editor") {}

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    // --- построение кадра UI ---
    void DrawDockspaceAndMenu();
    void BuildDefaultDockLayout(unsigned int dockspaceId);
    void DrawHierarchyPanel();
    void DrawInspectorPanel();
    void DrawViewportPanel();
    void DrawConsolePanel();
    void DrawAssetsPanel();
    void DrawDialogs(); // модалки New Project / Open Project / Save Scene As / Open Scene

    // --- сцена / рендер ---
    void RenderSceneToFramebuffer();
    void NewScene(bool withDemoContent);
    bool LoadSceneFromFile(const std::filesystem::path& path);
    bool SaveSceneToFile(const std::filesystem::path& path);
    GameObject CreateCubeEntity(const std::string& name);
    void DuplicateSelected();
    void DeleteSelected();
    void RunSelfTest(); // SAGE_EDITOR_SELFTEST=1: проект + сохранение/загрузка сцены (для CI)
    void PickEntityAtViewportPos(float u, float v); // u,v в [0..1] внутри вьюпорта

    // --- проект ---
    Project m_project;
    std::filesystem::path m_scenePath;   // куда сохранена/откуда открыта текущая сцена
    std::filesystem::path m_assetsCwd;   // текущая папка панели Assets

    // --- сцена и рендер превью ---
    std::unique_ptr<Scene> m_scene;
    Camera m_camera;
    std::optional<Shader> m_shader;
    std::optional<Framebuffer> m_sceneFbo;
    std::shared_ptr<Mesh> m_cube;

    // --- выбор/гизмо/вьюпорт ---
    int m_selectedId = -1;
    int m_gizmoOp = 0;           // значение ImGuizmo::OPERATION (int — чтобы не тащить ImGuizmo.h сюда)
    bool m_snap = false;
    bool m_showGrid = true;
    int m_viewportW = 1280, m_viewportH = 720;
    bool m_viewportHovered = false;
    bool m_cameraDriving = false; // ПКМ-полёт активен (перехватывает WASD у хоткеев гизмо)

    // последние view/proj кадра — для гизмо и пикинга
    glm::mat4 m_view{1.0f}, m_proj{1.0f};

    // --- docking ---
    bool m_rebuildDockLayout = false; // форс-перестройка (Window > Reset Layout)

    // --- консоль (сток лога движка) ---
    struct ConsoleEntry { LogLevel Level; std::string Category; std::string Message; };
    std::vector<ConsoleEntry> m_console;
    std::mutex m_consoleMutex;
    bool m_consoleAutoScroll = true;

    // --- состояние модалок (буферы полей ввода) ---
    char m_dlgProjectName[128] = "MyGame";
    char m_dlgProjectDir[512] = "";
    char m_dlgOpenPath[512] = "";
    char m_dlgSceneName[128] = "level1";
    std::string m_dlgError;

    bool m_imguiReady = false;

    // --- авто-скриншот для headless-проверки/CI (SAGE_SCREENSHOT_*) ---
    std::string m_screenshotPath = "editor.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
