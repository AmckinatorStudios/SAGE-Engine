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
#include "sage/render/DebugDraw.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"
#include "Project.h"
#include "PluginAPI.h"
#include "PluginManager.h"

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
//     (имя/Transform/цвет/меш/скрипт), Console (живой сток лога движка), Assets
//     (браузер файлов проекта, двойной клик по .sage — загрузка сцены).
//   • Play-режим: кнопка Play снапшотит сцену и запускает рантайм — скрипты
//     сущностей (ScriptComponent, .lua) привязываются к ScriptEngine и тикают
//     каждый кадр; Pause замораживает, Stop откатывает сцену к снапшоту.
//   • Undo/Redo (Ctrl+Z / Ctrl+Y): снапшот-модель — состояние сцены (JSON)
//     запоминается ПЕРЕД каждой мутацией: создание/удаление/дублирование,
//     завершённое перетаскивание гизмо, завершённая правка в Inspector.
//   • Плагины (v1, только редактор — см. PluginAPI.h): .so/.dll из plugins/
//     рядом с бинарником грузятся при старте (PluginManager), рисуют свои
//     ImGui-панели каждый кадр через узкий EditorPluginContext facade.
// ---------------------------------------------------------------------------
class EditorLayer : public sage::Layer {
public:
    EditorLayer() : sage::Layer("Editor") {}

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    // Реализация facade'а, который видят плагины — сознательно узкая (см.
    // PluginAPI.h): не пробрасывает Scene&/entt::registry& через границу
    // динамической библиотеки.
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

    // --- построение кадра UI ---
    void DrawDockspaceAndMenu();
    void BuildDefaultDockLayout(unsigned int dockspaceId);
    void DrawHierarchyPanel();
    void DrawInspectorPanel();
    void DrawMaterialEditor(); // редактор .sagemat, выбранного в Assets (внутри Inspector)
    void DrawViewportPanel();
    void DrawConsolePanel();
    void DrawAssetsPanel();
    void DrawAssetsBreadcrumb();
    void DrawAssetTile(const std::filesystem::path& path, bool isDir); // одна плитка сетки
    void DrawDialogs(); // модалки New Project / Open Project / Save Scene As / Open Scene / Delete Asset

    // --- сцена / рендер ---
    void RenderSceneToFramebuffer();
    void NewScene(bool withDemoContent);
    bool LoadSceneFromFile(const std::filesystem::path& path);
    bool SaveSceneToFile(const std::filesystem::path& path);
    GameObject CreateCubeEntity(const std::string& name);
    void DuplicateSelected();
    void DeleteSelected();
    void RunSelfTest(); // SAGE_EDITOR_SELFTEST=1: проект/сцена + undo/redo + Play (для CI)

    // --- Play-режим ---
    enum class PlayState { Editing, Playing, Paused };
    void StartPlay();
    void PausePlay() { if (m_playState == PlayState::Playing) m_playState = PlayState::Paused; }
    void ResumePlay() { if (m_playState == PlayState::Paused) m_playState = PlayState::Playing; }
    void StopPlay();
    bool InPlayMode() const { return m_playState != PlayState::Editing; }

    // --- Undo/Redo (снапшот-модель) ---
    // Зафиксировать текущее состояние сцены как «до мутации» (вершина undo-стека).
    // Каждая фиксация сбрасывает redo-ветку. В Play-режиме не работает.
    void PushUndoSnapshot();
    void Undo();
    void Redo();
    bool RestoreSceneFromString(const std::string& snapshot);
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
    std::optional<DebugDraw> m_debugDraw; // сетка/подсветка выбора — 3D-линии с depth-тестом
    std::shared_ptr<Mesh> m_cube;

    // --- Play-режим ---
    PlayState m_playState = PlayState::Editing;
    std::string m_playSnapshot;                    // сцена на момент Play — восстанавливается по Stop
    std::unique_ptr<ScriptEngine> m_playScripts;   // живёт только в Play-режиме

    // --- Undo/Redo ---
    std::vector<std::string> m_undoStack; // JSON-снапшоты «состояние до мутации»
    std::vector<std::string> m_redoStack;
    bool m_gizmoWasUsing = false;         // фронт «начали таскать гизмо» -> снапшот
    std::string m_pendingEditSnapshot;    // состояние на момент активации виджета Inspector
    std::string m_gizmoPendingSnapshot;   // состояние до первого кадра перетаскивания гизмо

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

    // --- панель Assets (сетка тайлов + поиск + rename/delete + создание) ---
    char m_assetsSearch[128] = "";
    std::filesystem::path m_assetsSelected; // выделенный тайл (не обязательно открытый)
    std::filesystem::path m_assetsRenameTarget; // файл, который переименовываем (пусто — не активно)
    char m_assetsRenameBuf[256] = "";
    std::filesystem::path m_assetsDeleteTarget; // ждёт подтверждения в модалке Delete

    // Создание нового ассета (ПКМ по пустому месту панели): вид + имя.
    // Kind != None => модалка "Create Asset" открывается в DrawDialogs
    // (деферред-паттерн, как у Rename/Delete).
    enum class AssetCreateKind { None, Folder, Script, TextFile, Material };
    AssetCreateKind m_assetsCreateKind = AssetCreateKind::None;
    char m_assetsCreateName[128] = "";
    bool CreatePendingAsset(std::filesystem::path& outCreatedPath); // false + m_dlgError при ошибке

    bool m_imguiReady = false;

    // --- плагины редактора (v1, см. PluginAPI.h/PluginManager.h) ---
    PluginManager m_plugins;
    PluginContextImpl m_pluginCtx{*this};
    std::string m_pluginStatusMessage;

    // --- авто-скриншот для headless-проверки/CI (SAGE_SCREENSHOT_*) ---
    std::string m_screenshotPath = "editor.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
