#pragma once
#include <algorithm>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <set>

#include "ModelMaterialImport.h"
#include "sage/core/Layer.h"
#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/PostFX.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"
#include "sage/core/SystemScheduler.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/core/Config.h"
#include "sage/ecs/RenderBatch.h"

#include "sage/input/GlfwBridge.h"
#include "sage/input/InputSystem.h"
#include "sage/audio/AudioEngine.h"

#include "EditorHost.h"
#include "ProjectTemplates.h"

namespace sage { class Application; }
#include "EditorPlayInput.h"
#include "EditorSceneRenderer.h"
#include "Project.h"
#include "ProjectLauncher/ProjectDatabase.h"
#include "PluginAPI.h"
#include "PluginManager.h"
#include "panels/ConsolePanel.h"
#include "panels/ProfilerPanel.h"
#include "ConfirmDialog.h"
#include "panels/HierarchyPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/ViewportPanel.h"
#include "panels/GamePanel.h"
#include "panels/AssetsPanel.h"
#include "ProjectLauncher/ProjectLauncher.h"
#include "panels/EnvironmentPanel.h"
#include "panels/UIEditorPanel.h"
#include "panels/TopBarPanel.h"
#include "panels/SettingsPanel.h"
#include "panels/InputPanel.h"
#include "panels/TemplatesPanel.h"
#include "ui/CommandPalette.h"
#include "ui/Commands.h"
#include "panels/DialogsPanel.h"

// ---------------------------------------------------------------------------
// EditorLayer — ядро редактора SAGE (архитектура v3).
//
// Оркестратор: владеет сценой/проектом/Play-режимом/undo и реализует контракт
// EditorHost, через который работают ПАНЕЛИ — независимые классы в panels/
// (Hierarchy, Inspector, Viewport, Game, Console, Assets, Launcher), каждая со
// своим UI-состоянием. Новая панель = новый файл + вызов Draw() в кадре; в ядро
// врастать не нужно. Весь ПРЕВЬЮ-РЕНДЕР (тени/Viewport/Game/PostFX/гизмо) вынесен
// в отдельный класс EditorSceneRenderer — EditorLayer только зовёт его в OnRender
// и показывает его текстуры в панелях (разгрузка god-object).
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
    // Файлы, брошенные в окно из проводника: проект/сцена открываются,
    // остальное вносится в проект. Разбирается в кадре, а не в колбэке GLFW.
    void HandleDroppedFiles();
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

    // --- EditorHost: сцена и выбор ---
    Scene& CurrentScene() override { return *m_scene; }
    int SelectedId() const override { return m_selectedId; }
    void SetSelectedId(int id) override; // одиночный выбор (сбрасывает набор)
    GameObject SelectedObject() override { return m_scene->Get(m_selectedId); }
    const std::vector<int>& Selection() const override { return m_selection; }
    bool IsSelected(int id) const override;
    void ToggleSelection(int id) override;
    void SetSelection(const std::vector<int>& ids, bool additive = false) override;

    // --- EditorHost: префабы ---
    bool SaveSelectedAsPrefab(const std::filesystem::path& path, std::string& err) override;
    int InstantiatePrefab(const std::filesystem::path& path) override;

    // --- EditorHost: проект и файлы сцен ---
    Project& CurrentProject() override { return m_project; }
    bool LoadSceneFromFile(const std::filesystem::path& path) override;
    bool SaveSceneToFile(const std::filesystem::path& path) override;
    bool CreateProject(const std::string& dir, const std::string& name,
                       const std::string& templateId, std::string& err) override;
    bool OpenProject(const std::string& path, std::string& err) override;
    // Упаковывает открытый проект в готовую к запуску игру: SagePlayer +
    // рантайм-ассеты + project/. false + err при ошибке.
    bool BuildGame(const std::filesystem::path& outputDir, std::string& err) override;
    std::filesystem::path& AssetsCwd() override { return m_assetsCwd; }

    // --- EditorHost: настройки и статус ---
    // ВНИМАНИЕ: возвращается ГЛОБАЛЬНЫЙ конфиг движка, а не отдельная копия
    // редактора. Копия здесь и была всей бедой — см. ApplyEngineSettings.
    sage::EngineConfig& Settings() override { return m_settings; }
    void SetStatusMessage(const std::string& message) override { m_pluginStatusMessage = message; }

    // --- EditorHost: undo/redo ---
    const std::string& TemplateNote() const override { return m_templateNote; }
    const std::string& RenderWarning() const override { return m_renderer.Warning(); }
    void ClearTemplateNote() override { m_templateNote.clear(); }
    void MergeScriptVars(GameObject object) override;
    // Сообщает в консоль заметку шаблона (см. ProjectTemplate::Note).
    void AnnounceTemplateNote(const ProjectTemplate& tpl);
    std::string m_templateNote;   // «что делать дальше» после создания проекта
    // Съёмка обложек шаблонов (SAGE_EDITOR_TEMPLATE_SHOTS): куда складывать,
    // что снимаем прямо сейчас и снялось ли. Живёт в слое, потому что снимок
    // делается ПОСЛЕ RenderGame, а шаблоны перебираются в кадре.
    std::string m_coverShotDir;
    std::string m_coverShotPath;
    bool m_coverShotDone = false;
    int m_coverShotIndex = -1;
    int m_coverShotWait = 0;
    void TickTemplateShots();
    // Снимок кадра по SAGE_SCREENSHOT_AT_FRAME. Отдельной функцией, потому что
    // кадр заканчивается в ДВУХ местах: со стартовым окном и с редактором.
    void TakeAutoScreenshot(sage::Application& app);
    void PushUndoSnapshot() override;
    bool CanUndo() const override { return !m_undoStack.empty(); }
    bool CanRedo() const override { return !m_redoStack.empty(); }
    void CapturePendingSnapshot() override;
    void CommitPendingSnapshot() override;
    void TrackLastImGuiItem() override;

    // --- EditorHost: сущности ---
    GameObject CreateCubeEntity(const std::string& name) override;
    GameObject CreatePrimitiveEntity(const std::string& name, MeshRef::Type type);
    int CreateCatalogObject(const std::string& id) override;
    GameObject DuplicateEntity(GameObject src); // копия одной сущности (для Duplicate/prefab)
    void DuplicateSelected() override;
    void DeleteSelected() override;

    // --- EditorHost: Play ---
    EditorPlayState GetPlayState() const override { return m_playState; }
    bool InPlayMode() const override { return m_playState != EditorPlayState::Editing; }
    void StartPlay() override;
    void PausePlay() override;
    void StepPlay() override;
    void ResumePlay() override;
    void StopPlay() override;
    // Ввод интерфейсу ИГРЫ в Play-режиме: курсор панели Game, переведённый в
    // координаты игрового кадра, плюс набранный текст (см. определение).
    void UpdatePlayUiInput(float dt);

    // --- EditorHost: общее состояние инструментов (тулбар + вьюпорт) ---
    int& GizmoOp() override { return m_gizmoOp; }
    bool& GizmoSnap() override { return m_snap; }
    EditorGizmoSpace& GizmoSpace() override { return m_gizmoSpace; }
    bool& ShowGrid() override { return m_showGrid; }
    EditorRenderMode& RenderMode() override { return m_renderMode; }
    float& SnapMove() override { return m_snapMove; }
    float& SnapRotate() override { return m_snapRotate; }
    float& SnapScale() override { return m_snapScale; }
    float SnapStepForCurrentOp() override;
    bool& ShowBounds() override { return m_showBounds; }
    UIToolSettings& UITools() override { return m_uiTools; }
    GameObject CreateUIEntity(const std::string& preset) override;
    bool& ColliderEditMode() override { return m_colliderEdit; }

    // --- EditorHost: инструменты над выделением ---
    void FocusSelected() override;
    void DropSelectedToSurface() override;
    void AlignSelection(int axis) override;
    bool SelectionBounds(glm::vec3& outMin, glm::vec3& outMax) override;

    // --- EditorHost: вьюпорт/камера ---
    Camera& EditorCamera() override { return m_camera; }
    const glm::mat4& ViewMatrix() const override { return m_view; }
    const glm::mat4& ProjMatrix() const override { return m_proj; }
    uint64_t SceneTexture() const override { return m_renderer.ViewportTexture(); }
    void SetViewportSize(int w, int h) override { m_renderer.SetViewportSize(w, h); }
    void SetViewRequests(const ViewRequest* requests, int count) override {
        m_viewCount = std::min(count, kMaxViews);
        for (int i = 0; i < m_viewCount; ++i) {
            m_viewRequests[i] = requests[i];
            if (m_viewRequests[i].Active) m_renderer.SetViewportSize(i, m_viewRequests[i].W,
                                                                     m_viewRequests[i].H);
        }
    }
    uint64_t ViewTexture(int slot) const override { return m_renderer.ViewportTexture(slot); }

    void RequestCameraPreview(int entityId, int w, int h) override {
        m_cameraPreviewId = entityId;
        if (entityId >= 0) m_renderer.SetCameraPreviewSize(w, h);
    }
    uint64_t CameraPreviewTexture() const override { return m_renderer.CameraPreviewTexture(); }
    bool OpenFileInSystemEditor(const std::filesystem::path& path, int line = 0) override;
    void PickAtViewport(float u, float v, bool additive = false) override;
    bool DropAssetAtViewport(const glm::mat4& view, const glm::mat4& proj, float u, float v,
                             const std::filesystem::path& asset) override;
    bool ApplyAssetToEntity(int entityId, const std::filesystem::path& asset) override;
    bool AddAssetToScene(const std::filesystem::path& asset) override;
    // Материал из НАБОРА карт, лежащих рядом с картинкой (см. TextureSet.h):
    // скачанный набор становится материалом целиком, а не одной albedo.
    std::string MaterialFromTextureSet(const std::filesystem::path& texture, std::string& status);
    void PickAtViewportWith(const glm::mat4& view, const glm::mat4& proj, float u, float v,
                            bool additive) override;
    void SelectInViewportRect(const glm::mat4& view, const glm::mat4& proj, float u0, float v0,
                              float u1, float v1, bool additive) override;

    // --- EditorHost: панель Game ---
    void ShowSettingsWindow() override { m_showSettings = true; }

    // --- EditorHost: раскладка управления ---
    sage::input::InputSystem& ProjectInput() override { return m_projectInput; }
    bool SaveProjectInput() override;
    bool ReloadProjectInput() override;
    // Где лежит раскладка открытого проекта (пусто, если проект не открыт).
    std::filesystem::path ProjectInputFile() const;
    // Переносит раскладку проекта в работающий ввод Play (зовётся из StartPlay
    // ПОСЛЕ того, как скрипты объявили свои умолчания).
    void ApplyProjectInputMapping();
    bool ProjectInputDirty() const override { return m_projectInputDirty; }
    void SetProjectInputDirty(bool dirty) override { m_projectInputDirty = dirty; }
    const std::vector<sage::input::InputEvent>& FrameInputEvents() const override {
        return m_playInput.FrameEvents();
    }
    bool& PanelVisible(EditorPanel panel) override {
        switch (panel) {
            case EditorPanel::Hierarchy:   return m_showHierarchy;
            case EditorPanel::Inspector:   return m_showInspector;
            case EditorPanel::Environment: return m_showEnvironment;
            case EditorPanel::Assets:      return m_showAssets;
            case EditorPanel::Console:     return m_showConsole;
            case EditorPanel::Profiler:    return m_showProfiler;
            case EditorPanel::Game:        return m_showGame;
            case EditorPanel::Viewport:    return m_showViewport;
            case EditorPanel::UIEditor:    return m_showUIEditor;
            case EditorPanel::Settings:    return m_showSettings;
            case EditorPanel::Input:       return m_showInput;
            default:                       return m_showViewport;
        }
    }
    std::string CurrentSceneName() const override {
        return m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    }
    bool SceneDirty() const override { return m_sceneDirty; }

    uint64_t GameTexture() const override { return m_renderer.GameTexture(); }
    void SetGameViewportSize(int w, int h) override { m_renderer.SetGameSize(w, h); }
    bool HasPrimaryCamera() override;

    // --- EditorHost: выбор в Assets ---
    const std::filesystem::path& SelectedAssetPath() const override { return m_assets.Selected(); }
    void ShowAssetInPanel(const std::filesystem::path& path) override;

    // Устройство заводится на старте редактора вместе с остальными системами
    // режима правки (см. EditorLayer::OnAttach) и живёт до конца.
    AudioEngine* Audio() override { return m_playAudio.get(); }

    // --- EditorHost: замок панели свойств (см. EditorHost.h) ---
    bool InspectorLocked() const override { return m_inspectorLocked; }
    void SetInspectorLocked(bool locked) override;
    GameObject InspectedObject() override;
    const std::filesystem::path& InspectedAssetPath() const override {
        return m_inspectorLocked ? m_lockedAssetPath : m_assets.Selected();
    }

private:
    // --- undo/redo (вызываются меню и хоткеями) ---
    void Undo() override;
    void Redo() override;

    // --- построение кадра UI ---
    void DrawDockspaceAndMenu();
    void BuildDefaultDockLayout(unsigned int dockspaceId);
    void DrawStatusBar(float height);
    void DrawAboutWindow(); // Help > About: версия движка + версии подсистем (v1)

    // --- сцена / рендер (превью-рендер вынесен в EditorSceneRenderer) ---
    // Новая сцена по ШАБЛОНУ (см. ProjectTemplates.h). Empty — пустая; она же
    // используется пунктом «Новая сцена» и самопроверкой.
    void NewScene(ProjectTemplateKind content);

    // Диалог, который надо открыть в ближайшем кадре (SAGE_EDITOR_OPEN_DIALOG).
    // Открывать сразу нельзя: OpenPopup обязан звучать на уровне окна-хоста.
    const char* m_pendingDialog = nullptr;

public:
    void RequestDialog(const char* id) override { m_pendingDialog = id; }

private:
    void UpdateWindowTitle();
    void RunSelfTest();
    // Блоки самопроверки по областям. Возвращают свой итог и выполняются ВСЕ,
    // независимо друг от друга: прогон обязан показать все поломки разом, а не
    // остановиться на первой. Раньше это была одна функция на две тысячи строк
    // с общим флагом, и после первой ошибки половина проверок молча
    // пропускалась.
    bool SelfTestProjectAndAssets();
    bool SelfTestSceneAndPlay();
    bool SelfTestSystems();
    bool SelfTestSelection();
    bool SelfTestTools(); // SAGE_EDITOR_SELFTEST=1 (для CI)
    bool SelfTestRenderStability(); // кадр вьюпорта не «уплывает» от повторов
    // Многооконность — проверка В ЖИВОМ КАДРЕ: окно системы заводит платформа,
    // а не флаг, и этот последний шаг ломается молча (см. EditorSelfTest.cpp).
    void CheckMultiWindowFrame();
    bool m_multiWindowChecked = false; // проверка одноразовая: кадров много
    // SAGE_EDITOR_E2E=1: полная игра через редактор — проект + Lua-логика +
    // Play + Build Game (собранный бинарник затем гоняет smoke-тест).
    void RunE2EGameTest();
    // SAGE_EDITOR_OPEN_PROJECT=<путь>: headless-прогон ЧУЖОГО проекта теми же
    // операциями редактора — открыть, загрузить сцену, отыграть N секунд в
    // Play, при желании собрать exe. Так CI игры проверяет игру НАСТОЯЩИМ
    // редактором, не заводя копию её сцен и скриптов внутри движка.
    void RunHeadlessProjectSession();

    bool RestoreSceneFromString(const std::string& snapshot);

    // --- проект ---
    Project m_project;
    // База проектов стартового окна: что человек открывал, создавал или
    // принёс сам. Пришла на смену списку недавних (см. ProjectDatabase.h).
    Sage::Launcher::ProjectDatabase m_projects;
    std::filesystem::path m_scenePath;   // куда сохранена/откуда открыта текущая сцена
    std::filesystem::path m_assetsCwd;   // текущая папка панели Assets
    // Состав и порядок кадра (см. sage/core/SystemScheduler.h).
    sage::SystemScheduler m_systems;

    bool m_sceneDirty = false;           // есть несохранённые правки (маркер '*')
    // Автосохранение и восстановление после падения. Оба пишут в ОТДЕЛЬНЫЕ
    // файлы рядом с редактором, а не поверх сцены (см. OnUpdate).
    float m_autosaveInterval = 60.0f;    // 0 — выключено
    float m_autosaveTimer = 0.0f;
    std::string m_lastAutosave;
    // Найденный при запуске файл восстановления: показать предложение один раз.
    std::string m_recoveryFile;
    std::string m_crashReportPath;   // отчёт прошлого падения, пусто — не падали
    std::string m_crashReportText;   // он же целиком, для показа и копирования
    size_t m_crashReportCount = 0;   // сколько отчётов лежит рядом
    bool m_crashPrompt = false;
    bool m_recoveryPrompt = false;

    // Достаёт материал из файла модели и назначает его меш-рендеру (см. .cpp).
    // Сообщает человеку, чем кончился импорт материалов модели (строка
    // состояния + лог). Сам импорт делает SetEntityMesh — см. ModelMaterialImport.h.
    void ReportModelMaterials(const ModelMaterialImportResult& r);
    void DrawRecoveryPrompt();
    // Окно отчёта о ПРОШЛОМ падении: показывается один раз при запуске, если
    // рядом лежит sage-crash-*.txt (см. FindCrashReport).
    void FindCrashReport();
    void DrawCrashReport();
    std::string m_windowTitle;           // кэш заголовка (не дёргать GLFW каждый кадр)

    // --- сцена и рендер превью ---
    std::unique_ptr<Scene> m_scene;
    Camera m_camera;                       // редакторская камера (Viewport)
    EditorSceneRenderer m_renderer;        // весь превью-рендер (теней/Viewport/Game/PostFX/гизмо)

    // --- общее состояние инструментов (тулбар + вьюпорт делят через host) ---
    int m_gizmoOp = 0;                                          // ImGuizmo::OPERATION (TRANSLATE)
    bool m_snap = false;
    // Оси по умолчанию — МИРОВЫЕ. В осях объекта стрелка «вправо» у повёрнутого
    // предмета ведёт вбок и вглубь одновременно, и человек, который об этом не
    // знает (а по умолчанию не знает никто), видит просто «гизмо тянет не
    // туда». Мир одинаков для всех объектов сцены, поэтому он и стоит первым;
    // локальные оси включаются осознанно — подписанным переключателем в строке
    // инструментов.
    EditorGizmoSpace m_gizmoSpace = EditorGizmoSpace::World;
    bool m_showGrid = true;
    EditorRenderMode m_renderMode = EditorRenderMode::Shaded;
    // Шаг привязки. Перенос — 1.0: движок строит примитивы размером в единицу,
    // и для постройки из блоков это единственный шаг, при котором блоки встают
    // вплотную без щелей и нахлёстов.
    float m_snapMove = 1.0f;
    float m_snapRotate = 15.0f;
    float m_snapScale = 0.1f;
    bool m_showBounds = false;
    UIToolSettings m_uiTools;   // сетка и привязки вёрстки (см. UIToolSettings.h)
    bool m_colliderEdit = false; // гизмо тянет коллайдер, а не объект

    // --- Play-режим ---
    EditorPlayState m_playState = EditorPlayState::Editing;
    std::string m_playSnapshot;                    // сцена на момент Play — восстанавливается по Stop
    std::unique_ptr<ScriptEngine> m_playScripts;   // живёт только в Play-режиме
    std::unique_ptr<PhysicsScene> m_playPhysics;   // симуляция физики только в Play-режиме
    // Ввод игры в Play: те же именованные действия и та же мышь, что в
    // собранной игре, но отдаются игре только при фокусе панели Game
    // (см. EditorPlayInput). Живут всё время работы редактора — действия
    // объявляют скрипты при старте Play, карта пересоздаётся вместе с ними.
    sage::input::InputSystem m_playInput;
    // Мост к окну — ОДИН на всё время работы редактора, а не новый на каждый
    // Play: подписка на события окна снимается только вместе с окном, и второй
    // мост означал бы два комплекта событий на одно нажатие.
    sage::input::GlfwBridge m_playInputBridge;
    EditorPlayInput m_playCursor;   // захват курсора по фокусу панели Game

    // Раскладка управления ПРОЕКТА — документ, а не работающий ввод: кадры
    // ей никто не считает. Живёт в <проект>/input.sageinput, правится панелью
    // «Управление» и применяется к m_playInput при старте Play — ровно так же,
    // как её применяет собранная игра. Иначе превью игралось бы не тем
    // управлением, что игра, и разницу находили бы уже после сборки.
    sage::input::InputSystem m_projectInput;
    bool m_projectInputDirty = false;
    // Звук Play-режима. Без него PlaySound из Lua падал бы в редакторе и
    // работал в собранной игре — превью обязано звучать так же, как игра.
    std::unique_ptr<AudioEngine> m_playAudio;

    // --- Undo/Redo ---
    std::vector<std::string> m_undoStack; // JSON-снапшоты «состояние до мутации»
    std::vector<std::string> m_redoStack;
    std::string m_pendingEditSnapshot;    // состояние на момент Capture (виджет/гизмо)

    // --- выбор/вьюпорты (размеры окон живут в m_renderer) ---
    int m_selectedId = -1;              // «первичная» (последняя кликнутая)

    // Замок панели свойств: что она показывает, пока заперта (см. EditorHost.h).
    bool m_inspectorLocked = false;
    int m_lockedEntityId = -1;
    std::filesystem::path m_lockedAssetPath;
    std::vector<int> m_selection;       // весь набор выбранных (включает первичную)
    // Длительность ЗАКАЗАННОГО шага на паузе (0 — шага нет). Заказ, а не прямой
    // прогон: кнопку нажимают посреди рисования интерфейса, а кадр игры обязан
    // считаться там же, где считается всегда, — иначе системы пошли бы дважды
    // за один кадр редактора.
    float m_pendingStep = 0.0f;
    glm::mat4 m_view{1.0f}, m_proj{1.0f}; // последние view/proj кадра (гизмо/пикинг)

    // --- docking ---
    bool m_rebuildDockLayout = false; // форс-перестройка (Window > Reset Layout)

    // Видимость панелей. У каждой докнутой панели ImGui рисует крестик на
    // вкладке, и закрытая панель раньше исчезала НАВСЕГДА: в меню Window её не
    // было, а раскладка сохранялась в sage_editor_imgui.ini — то есть закрытое
    // окно не возвращалось и после перезапуска. Закрыв вкладки одну за другой,
    // человек оставался с пустым серым прямоугольником и делал вывод, что
    // «свернул весь редактор» и сломал его. Флаг на панель + пункт в меню
    // Window делают закрытие обратимым, а ShowAllPanels() — «Reset Layout» и
    // подсказка на пустом доке — возвращают всё одним действием.
    bool m_showHierarchy = true;
    bool m_showInspector = true;
    bool m_showEnvironment = true;
    // Редактор интерфейса — по умолчанию закрыт: это отдельный инструмент под
    // отдельную задачу, и открывают его, когда садятся верстать.
    bool m_showUIEditor = false;
    bool m_showViewport = true;
    bool m_showGame = true;
    bool m_showConsole = true;
    bool m_showAssets = true;
    void ShowAllPanels();         // вернуть все панели на экран
    bool AnyPanelVisible() const; // осталась ли на экране хоть одна панель
    // Подсказка на пустом доке. Прямоугольник передаётся числами, а не ImVec2:
    // imgui.h в этот заголовок не входит, и тянуть его сюда ради двух точек —
    // значит навязать его всем, кто включает EditorLayer.h.
    void DrawEmptyDockHint(float minX, float minY, float maxX, float maxY);
    // Показать окна, которые ImGui вынес в отдельные окна системы. Зовётся в
    // КАЖДОМ кадре после ImGui::Render() — см. EditorLayer.cpp.
    void PresentExtraViewports();

    // --- панели (архитектура v3: каждая — независимый класс) ---
    ConsolePanel m_console;
    ProfilerPanel m_profiler;
    ConfirmDialog m_confirm;
    // Запросы мультивьюпорта: панель раскладывает, рендер исполняет в начале
    // следующего кадра.
    ViewRequest m_viewRequests[kMaxViews];
    // Чью камеру показывать карточкой превью в этом кадре (-1 — ничью).
    // Сбрасывается сразу после отрисовки: запрос живёт один кадр (см.
    // EditorHost::RequestCameraPreview).
    int m_cameraPreviewId = -1;
    int m_viewCount = 1;
    bool m_showProfiler = false;
    bool m_showIconSheet = false; // страница со всеми иконками (Window > Icon sheet)
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ViewportPanel m_viewport;
    GamePanel m_game;
    // Левая кнопка на прошлом кадре: из «удерживается» и «удерживалась»
    // получаются «нажата» и «отпущена», а без них щелчка не существует.
    bool m_playUiMouseWasDown = false;
    AssetsPanel m_assets;
    // Накопитель брошенных путей: колбэк окна складывает сюда, кадр разбирает.
    std::vector<std::string> m_droppedFiles;
    // Окна, которым уже повесили приём файлов (панели, вытащенные из дока).
    std::set<void*> m_dropWindows;
    // Приёмник для колбэка GLFW: он статический по природе (C-функция), а
    // редактор в процессе один.
    static std::function<void(const std::vector<std::string>&)> s_dropSink;
    Sage::Launcher::ProjectLauncher m_launcher;
    EnvironmentPanel m_environment;
    UIEditorPanel m_uiEditor;
    TopBarPanel m_topBar;
    SettingsPanel m_settingsPanel; // окно гибких настроек движка (host.Settings())
    InputPanel m_inputPanel;       // раскладка управления проекта (input.sageinput)
    TemplatesPanel m_templatesPanel; // установка/скачивание шаблонов проектов
    bool m_showTemplates = false;

    // Реестр команд и палитра (Ctrl+K). Реестр наполняется один раз в
    // OnAttach: команда описывается ОДИН раз, а меню, тулбар, палитра и
    // обработчик клавиш читают этот же список (см. ui/Commands.h).
    Sage::UI::CommandRegistry m_commands;
    Sage::UI::CommandPalette m_palette;
    std::string m_paletteQuery;   // начальный запрос (SAGE_EDITOR_PALETTE)
    void RegisterCommands();
    DialogsPanel m_dialogs;        // модалки File-меню (New/Open Project, Save/Open Scene, Build)
    bool m_launcherRequested = false; // Window > Project Launcher
    // Прогон без человека (скриншот, самопроверка, автоигра) попросил не
    // показывать стартовое окно. Раз работы без проекта больше нет, это значит
    // «создай рабочий проект сам» — см. конец OnAttach.
    bool m_headlessProject = false;

    // --- гибкие настройки движка (редактируются панелью Settings, сохраняются
    //     в <проект>/sage.cfg; Build Game кладёт их в собранную игру). Буферы
    //     полей модалок File-меню теперь живут внутри DialogsPanel. ---
    sage::EngineConfig m_settings;
    // Отпечаток m_settings, по которому видно, что настройки правили: конфиг —
    // простая структура без сигналов, а перекладывать её в глобальную каждый
    // кадр значило бы копировать её .
    std::string m_settingsStamp;
    // Переносит m_settings в глобальный EngineConfig, если они разошлись.
    void ApplyEngineSettings();
    bool m_showSettings = false;
    bool m_showInput = false;
    bool m_showAbout = false; // Help > About SAGE (версии подсистем)

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
