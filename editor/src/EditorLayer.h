#pragma once
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "sage/core/Layer.h"
#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

#include "EditorHost.h"
#include "Project.h"
#include "RecentProjects.h"
#include "PluginAPI.h"
#include "PluginManager.h"
#include "panels/ConsolePanel.h"
#include "panels/HierarchyPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/ViewportPanel.h"
#include "panels/GamePanel.h"
#include "panels/AssetsPanel.h"
#include "panels/LauncherPanel.h"

// ---------------------------------------------------------------------------
// EditorLayer — ядро редактора SAGE (архитектура v3).
//
// Оркестратор: владеет сценой/проектом/Play-режимом/undo/рендером превью и
// реализует контракт EditorHost, через который работают ПАНЕЛИ — независимые
// классы в panels/ (Hierarchy, Inspector, Viewport, Game, Console, Assets,
// Launcher), каждая со своим UI-состоянием. Новая панель = новый файл +
// вызов Draw() в кадре; в ядро врастать не нужно.
//
//   • Docking + multi-viewport: полноэкранный dockspace (DockBuilder-раскладка
//     по умолчанию, Window > Reset Layout); панели можно вытаскивать в
//     ОТДЕЛЬНЫЕ OS-ОКНА (ImGuiConfigFlags_ViewportsEnable).
//   • Viewport — редакторская камера; Game — «игровое окно» от Primary-камеры
//     сцены (CameraComponent), при Play фокус переходит на Game.
//   • Проекты: стартовый launcher (недавние/создать/открыть), File-меню,
//     подменю сцен проекта; сцены — .sage JSON в scenes/ проекта.
//   • Play-режим: снапшот сцены, скрипты сущностей тикают, Stop откатывает.
//   • Undo/Redo (Ctrl+Z / Ctrl+Y): снапшот-модель; dirty-маркер сцены в
//     заголовке окна и статус-баре.
// ---------------------------------------------------------------------------
class EditorLayer : public sage::Layer, public EditorHost {
public:
    EditorLayer() : sage::Layer("Editor") {}

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

    // --- EditorHost: сцена и выбор ---
    Scene& CurrentScene() override { return *m_scene; }
    int SelectedId() const override { return m_selectedId; }
    void SetSelectedId(int id) override { m_selectedId = id; }
    GameObject SelectedObject() override { return m_scene->Get(m_selectedId); }

    // --- EditorHost: проект и файлы сцен ---
    Project& CurrentProject() override { return m_project; }
    bool LoadSceneFromFile(const std::filesystem::path& path) override;
    bool SaveSceneToFile(const std::filesystem::path& path) override;
    bool CreateProject(const std::string& dir, const std::string& name, std::string& err) override;
    bool OpenProject(const std::string& path, std::string& err) override;
    std::filesystem::path& AssetsCwd() override { return m_assetsCwd; }

    // --- EditorHost: undo/redo ---
    void PushUndoSnapshot() override;
    void CapturePendingSnapshot() override;
    void CommitPendingSnapshot() override;
    void TrackLastImGuiItem() override;

    // --- EditorHost: сущности ---
    GameObject CreateCubeEntity(const std::string& name) override;
    void DuplicateSelected() override;
    void DeleteSelected() override;

    // --- EditorHost: Play ---
    EditorPlayState GetPlayState() const override { return m_playState; }
    bool InPlayMode() const override { return m_playState != EditorPlayState::Editing; }
    void StartPlay() override;
    void PausePlay() override { if (m_playState == EditorPlayState::Playing) m_playState = EditorPlayState::Paused; }
    void ResumePlay() override { if (m_playState == EditorPlayState::Paused) m_playState = EditorPlayState::Playing; }
    void StopPlay() override;

    // --- EditorHost: вьюпорт/камера ---
    Camera& EditorCamera() override { return m_camera; }
    const glm::mat4& ViewMatrix() const override { return m_view; }
    const glm::mat4& ProjMatrix() const override { return m_proj; }
    unsigned int SceneTexture() const override { return m_sceneFbo->ColorTexture(); }
    void SetViewportSize(int w, int h) override { m_viewportW = w; m_viewportH = h; }
    void PickAtViewport(float u, float v) override;

    // --- EditorHost: панель Game ---
    unsigned int GameTexture() const override { return m_gameFbo->ColorTexture(); }
    void SetGameViewportSize(int w, int h) override { m_gameW = w; m_gameH = h; }
    bool HasPrimaryCamera() override;

    // --- EditorHost: выбор в Assets ---
    const std::filesystem::path& SelectedAssetPath() const override { return m_assets.Selected(); }

private:
    // --- undo/redo (вызываются меню и хоткеями) ---
    void Undo();
    void Redo();

    // --- построение кадра UI ---
    void DrawDockspaceAndMenu();
    void BuildDefaultDockLayout(unsigned int dockspaceId);
    void DrawStatusBar(float height);
    void DrawDialogs(); // модалки New Project / Open Project / Save Scene As / Open Scene

    // --- сцена / рендер ---
    void RenderSceneToFramebuffer(); // редакторская камера + DebugDraw (Viewport)
    void RenderGameToFramebuffer(); // Primary-камера сцены, без гизмо (Game)
    void NewScene(bool withDemoContent);
    void UpdateWindowTitle();
    void RunSelfTest(); // SAGE_EDITOR_SELFTEST=1 (для CI)

    bool RestoreSceneFromString(const std::string& snapshot);

    // --- проект ---
    Project m_project;
    RecentProjects m_recent;
    std::filesystem::path m_scenePath;   // куда сохранена/откуда открыта текущая сцена
    std::filesystem::path m_assetsCwd;   // текущая папка панели Assets
    bool m_sceneDirty = false;           // есть несохранённые правки (маркер '*')
    std::string m_windowTitle;           // кэш заголовка (не дёргать GLFW каждый кадр)

    // --- сцена и рендер превью ---
    std::unique_ptr<Scene> m_scene;
    Camera m_camera;
    std::optional<Shader> m_shader;
    std::optional<Framebuffer> m_sceneFbo;
    std::optional<Framebuffer> m_gameFbo;
    std::optional<DebugDraw> m_debugDraw;
    std::shared_ptr<Mesh> m_cube;

    // --- Play-режим ---
    EditorPlayState m_playState = EditorPlayState::Editing;
    std::string m_playSnapshot;                    // сцена на момент Play — восстанавливается по Stop
    std::unique_ptr<ScriptEngine> m_playScripts;   // живёт только в Play-режиме

    // --- Undo/Redo ---
    std::vector<std::string> m_undoStack; // JSON-снапшоты «состояние до мутации»
    std::vector<std::string> m_redoStack;
    std::string m_pendingEditSnapshot;    // состояние на момент Capture (виджет/гизмо)

    // --- выбор/вьюпорты ---
    int m_selectedId = -1;
    int m_viewportW = 1280, m_viewportH = 720;
    int m_gameW = 1280, m_gameH = 720;
    glm::mat4 m_view{1.0f}, m_proj{1.0f}; // последние view/proj кадра (гизмо/пикинг)

    // --- docking ---
    bool m_rebuildDockLayout = false; // форс-перестройка (Window > Reset Layout)

    // --- панели (архитектура v3: каждая — независимый класс) ---
    ConsolePanel m_console;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ViewportPanel m_viewport;
    GamePanel m_game;
    AssetsPanel m_assets;
    LauncherPanel m_launcher;
    bool m_launcherRequested = false; // Window > Project Launcher

    // --- состояние модалок File-меню (буферы полей ввода) ---
    char m_dlgProjectName[128] = "MyGame";
    char m_dlgProjectDir[512] = "";
    char m_dlgOpenPath[512] = "";
    char m_dlgSceneName[128] = "level1";
    std::string m_dlgError;

    // --- плагины редактора (v1, см. PluginAPI.h/PluginManager.h) ---
    class PluginContextImpl : public EditorPluginContext {
    public:
        explicit PluginContextImpl(EditorLayer& owner) : m_owner(owner) {}
        void Log(const char* message) override;
        const char* SelectedEntityName() const override;
        void SetStatusMessage(const char* message) override;

    private:
        EditorLayer& m_owner;
        mutable std::string m_selectedNameBuf;
    };
    PluginManager m_plugins;
    PluginContextImpl m_pluginCtx{*this};
    std::string m_pluginStatusMessage;

    bool m_imguiReady = false;

    // --- авто-скриншот для headless-проверки/CI (SAGE_SCREENSHOT_*) ---
    std::string m_screenshotPath = "editor.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
