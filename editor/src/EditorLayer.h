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
#include "EditorHistory.h"
#include "EditorSelection.h"
#include "EditorTools.h"
#include "ProjectTemplates.h"

namespace sage { class Application; }
#include "EditorPlaySession.h"
#include "EditorPanelVisibility.h"
#include "EditorRecovery.h"
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
#include "interface/InterfaceHierarchyPanel.h"
#include "interface/InterfaceInspectorPanel.h"
#include "interface/InterfacePreviewPanel.h"
#include "interface/InterfaceViewportPanel.h"
#include "panels/TopBarPanel.h"
#include "panels/SettingsPanel.h"
#include "panels/InputPanel.h"
#include "panels/TemplatesPanel.h"
#include "panels/AnimationPanel.h"
#include "panels/NineSlicePanel.h"
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
    // Определён в .cpp: история заводится с двумя функциями, а знание о том,
    // как снимается и применяется снимок сцены, принадлежит сериализатору —
    // тащить его заголовок сюда ради одной строки не за что.
    EditorLayer();

    void OnAttach() override;
    // Файлы, брошенные в окно из проводника: проект/сцена открываются,
    // остальное вносится в проект. Разбирается в кадре, а не в колбэке GLFW.
    void HandleDroppedFiles();
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

    // --- EditorHost: сцена и выбор ---
    Scene& CurrentScene() override { return *m_scene; }
    // Выделение, история и инструменты — отдельные объекты (см. их заголовки).
    // Слой их только держит и пересказывает панелям через EditorHost: правила
    // выделения и отката живут там, где их можно прочитать и проверить.
    EditorSelection& Selection() override { return m_selection; }
    GameObject SelectedObject() override { return m_scene->Get(m_selection.Primary()); }

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
    bool HasAnyScene() const override;
    std::filesystem::path& AssetsCwd() override { return m_assetsCwd; }

    // --- EditorHost: настройки и статус ---
    // ВНИМАНИЕ: возвращается ГЛОБАЛЬНЫЙ конфиг движка, а не отдельная копия
    // редактора. Копия здесь и была всей бедой — см. ApplyEngineSettings.
    sage::EngineConfig& Settings() override { return m_settings; }
    void SetStatusMessage(const std::string& message) override { m_pluginStatusMessage = message; }
    EditorWorkspace Workspace() const override { return m_workspace; }
    void SetWorkspace(EditorWorkspace workspace) override;

    int CurrentInterfaceId() const override { return m_currentInterface; }
    void SetCurrentInterface(int id) override { m_currentInterface = id; }
    sage::ui::UIScope UiScope() const override;
    // Выбирает интерфейс, с которым работать: прежний, если он ещё жив; иначе
    // тот, которому принадлежит выделенное; иначе первый в сцене. Зовётся при
    // входе в пространство вёрстки и при смене сцены.
    void ResolveCurrentInterface();

    void OpenAnimationClip(const std::string& clipPath) override {
        m_panels[EditorPanel::Animation] = true;
        m_animation.RequestFocus();
        m_animation.OpenClip(*this, clipPath);
    }
    void OpenNineSliceEditor(const NineSliceTarget& target) override {
        m_showNineSlice = true;
        m_nineSlice.OpenFor(target);
    }

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

    // --- Обложка сцены (см. SceneCover.h) ---
    // Заказ и съёмка разнесены: путь известен в момент сохранения, а кадр — в
    // конце кадра. Одна строка вместо машины состояний обложек шаблонов:
    // снимок здесь всегда один и никуда не переключает проект.
    std::string m_sceneShotPath;
    void RequestSceneShot();
    void TakeSceneShot();
    // Снимок кадра по SAGE_SCREENSHOT_AT_FRAME. Отдельной функцией, потому что
    // кадр заканчивается в ДВУХ местах: со стартовым окном и с редактором.
    void TakeAutoScreenshot(sage::Application& app);
    void PushUndoSnapshot() override;
    // Сцена изменена: звёздочка в заголовке окна и маркер несохранённого.
    void MarkSceneDirty();
    EditorHistory& History() override { return m_history; }
    void CommitPendingSnapshot();
    void TrackLastImGuiItem() override;

    // --- EditorHost: сущности ---
    GameObject CreateCubeEntity(const std::string& name) override;
    GameObject CreatePrimitiveEntity(const std::string& name, MeshRef::Type type);
    int CreateCatalogObject(const std::string& id) override;
    GameObject DuplicateEntity(GameObject src); // копия одной сущности (для Duplicate/prefab)
    void DuplicateSelected() override;
    void DeleteSelected() override;

    // --- EditorHost: Play ---
    // Play-режим — у EditorPlaySession; слой пересказывает его панелям и
    // добавляет то, чего сессия про редактор знать не должна: фокус окон и
    // заметку шаблона.
    EditorPlayState GetPlayState() const override { return m_play.State(); }
    bool InPlayMode() const override { return m_play.Active(); }
    PhysicsScene* PlayPhysics() override { return m_play.Active() ? m_play.Physics() : nullptr; }
    void SyncBodyToTransform(GameObject object) override;
    void StartPlay() override;
    void PausePlay() override { m_play.Pause(); }
    void StepPlay() override { m_play.RequestStep(); }
    void ResumePlay() override { m_play.Resume(); }
    void StopPlay() override;
    // Ввод интерфейсу ИГРЫ в Play-режиме: курсор панели Game, переведённый в
    // координаты игрового кадра, плюс набранный текст (см. определение).
    void UpdatePlayUiInput(float dt);
    // Что сессия получает от редактора на Start/Stop (см. EditorPlaySession.h).
    PlayContext MakePlayContext();

    // --- EditorHost: общее состояние инструментов (тулбар + вьюпорт) ---
    EditorTools& Tools() override { return m_tools; }
    GameObject CreateUIEntity(const std::string& preset) override;
    bool& ColliderEditMode() override { return m_tools.ColliderEdit; }

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
    void ShowSettingsWindow() override { m_panels[EditorPanel::Settings] = true; }

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
        return m_play.Input().FrameEvents();
    }
    bool& PanelVisible(EditorPanel panel) override { return m_panels[panel]; }
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
    AudioEngine* Audio() override { return &m_play.Audio(); }

    // --- EditorHost: замок панели свойств (см. EditorHost.h) ---
    bool InspectorLocked() const override { return m_selection.Locked(); }
    void SetInspectorLocked(bool locked) override;
    GameObject InspectedObject() override;
    const std::filesystem::path& InspectedAssetPath() const override {
        return m_selection.Locked() ? m_selection.LockedAssetPath() : m_assets.Selected();
    }

private:
    // --- undo/redo (вызываются меню и хоткеями) ---
    void Undo() override;
    void Redo() override;

    // --- построение кадра UI ---
    void DrawDockspaceAndMenu();
    void BuildDefaultDockLayout(unsigned int dockspaceId);
    // Раскладка пространства «Интерфейс». Своя, а не общая: у него другой
    // состав панелей и другой главный — холст, а не вьюпорт сцены.
    void BuildInterfaceDockLayout(unsigned int dockspaceId);
    // Рисует панели текущего пространства.
    void DrawWorkspacePanels();
    void DrawStatusBar(float height);
    void DrawAboutWindow(); // Help > About: версия движка + версии подсистем (v1)

    // --- сцена / рендер (превью-рендер вынесен в EditorSceneRenderer) ---
    // Новая сцена по ШАБЛОНУ (см. ProjectTemplates.h). Empty — пустая; она же
    // используется пунктом «Новая сцена» и самопроверкой.
    void NewScene(ProjectTemplateKind content);
    // Новая сцена по просьбе человека: пустая, но со своим солнцем-объектом.
    void NewSceneForUser();
    // Смена проекта: прошлый исчезает целиком — сцена, кэш ресурсов, частицы,
    // обложки, префабы, выделение. Подробности — в EditorLayer_Project.cpp.
    void ForgetPreviousProject();

    // Материал шаблона — ФАЙЛОМ в assets/materials, а не полем Color.
    //
    // Ровно та жалоба, с которой пришли: «формы разного цвета, это может быть
    // материал — нет, на свойствах самих объектов материалов НЕТ». Шаблон
    // красил кубы полем Color компонента, и человек, открывший инспектор
    // красного куба, видел пустой слот материала и цвет неизвестно откуда.
    // Шаблон обязан показывать, КАК В ДВИЖКЕ ДЕЛАЮТ: материал — это файл,
    // объект на него ссылается, правка файла перекрашивает всех, кто на него
    // ссылается.
    //
    // Возвращает ссылку на ассет (assets/materials/<имя>.sagemat) или пусто,
    // если проекта нет: демо-сцена стартового редактора живёт без проекта, и
    // писать её материалы некуда — там объект остаётся при своём цвете.
    std::string TemplateMaterial(const std::string& name, const glm::vec3& albedo,
                                 float metallic = 0.0f, float roughness = 0.6f);

    // Покрасить объект шаблона: завести ему материал и назначить. Без проекта
    // (демо-сцена стартового редактора) остаётся цвет объекта — иначе шаблон
    // отдавал бы белые формы там, где файл материала писать некуда.
    void PaintWithMaterial(GameObject object, const std::string& name, const glm::vec3& albedo,
                           float metallic = 0.0f, float roughness = 0.6f);

    // Диалог, который надо открыть в ближайшем кадре (SAGE_EDITOR_OPEN_DIALOG).
    // Открывать сразу нельзя: OpenPopup обязан звучать на уровне окна-хоста.
    const char* m_pendingDialog = nullptr;

public:
    void RequestDialog(const char* id) override { m_pendingDialog = id; }
    void SaveCurrentScene() override {
        if (m_scenePath.empty()) RequestDialog("Save Scene As");
        else SaveSceneToFile(m_scenePath);
    }
    void NewSceneWithPrompt() override {
        AskUnsaved([this] { NewSceneForUser(); });
    }

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
    // Общие панели переживают переход между пространствами (см. .cpp).
    void CheckWorkspaceDockFrame();
    // Разрешение предпросмотра правит кадр, а не проект (см. .cpp).
    void CheckPreviewResolutionFrame();
    // Инспектор каждого типа интерфейса и окно девятины (в том числе узкое)
    // рисуются без единой претензии ImGui (см. .cpp).
    void CheckUiInspectorFrame();
    // Закрыть всплывающее окно, которое открыли, но никто не рисует: такое
    // окно обездвиживает редактор целиком (см. EditorLayer.cpp).
    void CloseGhostPopups();
    bool m_multiWindowChecked = false; // проверка одноразовая: кадров много
    bool m_workspaceDockChecked = false;
    int m_wsDockStep = 0;              // шаг проверки общих панелей
    int m_wsDockWait = 0;              // кадры на устаканивание раскладки
    unsigned int m_wsDockHome = 0;     // где ассеты стояли в пространстве сцены
    bool m_previewResChecked = false;
    int m_previewResStep = 0;
    int m_previewResWait = 0;
    int m_previewResGameW = 0, m_previewResGameH = 0;   // размер игры ДО проверки
    bool m_uiInspectorChecked = false;
    int m_uiInspectorStep = 0;
    int m_uiInspectorWait = 0;
    int m_uiInspectorErrors = 0;       // ошибок в консоли до проверки
    int m_uiInspectorElement = 0;      // элемент, на котором открыто окно девятины

    // --- Мышь В ЖИВОМ КАДРЕ (SAGE_EDITOR_SELFTEST=1) -------------------------
    //
    // Щелчок по вьюпорту проходит через ImGui, ImGuizmo, захват мыши слотом,
    // гизмо осей и рамку выделения. Проверить это, вызывая внутренние функции
    // напрямую, нельзя: как раз в этих воротах всё и ломается — а каждая
    // функция по отдельности при этом работает. Поэтому проверка ЩЁЛКАЕТ
    // по-настоящему: кладёт события мыши в очередь ImGui и смотрит, что стало
    // со сценой.
    //
    // Шаги разнесены по кадрам, потому что нажатие и отпускание — это разные
    // кадры и для ImGui, и для редактора (выбор происходит на отпускании).
    void TickInputProbe();
    // Какое окно выводить вперёд по «Играть» (см. EditorLayer_Play.cpp).
    void FocusPlayTarget();
    int m_probeStep = -1;     // -1 — не запущена
    int m_probeWait = 0;      // кадров подождать перед следующим шагом
    int m_probeVictimId = -1; // объект, на котором проверяется, что Delete не утёк из панели
    int m_probeObjectsBefore = 0; // сколько было объектов до Ctrl+D
    float m_probeYaw = 0.0f;  // угол камеры до щелчка по гизмо
    bool m_probeFailed = false;
    ImVec2 m_probePos{0.0f, 0.0f};  // куда «поставлен» курсор проверки
    int m_probeFrames = 0;    // кадров с начала проверки (сторож от зависания)
    // SAGE_EDITOR_E2E=1: полная игра через редактор — проект + Lua-логика +
    // Play + Build Game (собранный бинарник затем гоняет smoke-тест).
    void RunE2EGameTest();
    // SAGE_EDITOR_OPEN_PROJECT=<путь>: headless-прогон ЧУЖОГО проекта теми же
    // операциями редактора — открыть, загрузить сцену, отыграть N секунд в
    // Play, при желании собрать exe. Так CI игры проверяет игру НАСТОЯЩИМ
    // редактором, не заводя копию её сцен и скриптов внутри движка.
    void RunHeadlessProjectSession();

    // Чего игра попросила за кадр (выйти, сменить уровень, начать заново).
    // false — сцена заменена или Play остановлен, и этот кадр доигрывать нечем.
    bool ProcessScriptRequests();

    bool RestoreSceneFromString(const std::string& snapshot);
    // Подменить ИГРАЕМУЮ сцену во время Play (scene:Load из скрипта), не
    // трогая документ человека. Подробности — в .cpp.
    bool LoadSceneForPlay(const std::string& name);

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
    // Что осталось от прошлого запуска и как не потерять этот: автосохранение,
    // файл восстановления, отчёт о падении (см. EditorRecovery.h). Рисование
    // окон осталось здесь — модалки это ImGui и переводы.
    EditorRecovery m_recovery;

    // --- ВОПРОС О НЕСОХРАНЁННОЙ СЦЕНЕ ---------------------------------------
    //
    // Закрыть редактор с несохранённой сценой можно было молча: крестик окна —
    // и работа исчезла. Undo здесь не спасает, он живёт в той же программе,
    // которую закрывают. Тот же вопрос нужен и там, где сцена ЗАМЕНЯЕТСЯ:
    // новая сцена, открытие другой сцены или проекта.
    bool m_unsavedPrompt = false;
    bool m_closeAfterPrompt = false;          // спросили из-за закрытия окна
    std::function<void()> m_afterUnsaved;     // что сделать, когда ответят
    // Окно попросили закрыть (крестик, Alt+F4). Несохранённая сцена — повод
    // задержать закрытие и спросить; ответ «не сохранять»/«сохранено» закроет
    // приложение сам (см. DrawUnsavedPrompt).
    bool OnCloseRequest() override;
    void AskUnsaved(std::function<void()> action);
    void DrawUnsavedPrompt();

    // Достаёт материал из файла модели и назначает его меш-рендеру (см. .cpp).
    // Сообщает человеку, чем кончился импорт материалов модели (строка
    // состояния + лог). Сам импорт делает SetEntityMesh — см. ModelMaterialImport.h.
    void ReportModelMaterials(const ModelMaterialImportResult& r);
    void DrawRecoveryPrompt();
    // Окно отчёта о ПРОШЛОМ падении: показывается один раз при запуске, если
    // рядом лежит sage-crash-*.txt (см. EditorRecovery::ScanOnStartup).
    void DrawCrashReport();
    std::string m_windowTitle;           // кэш заголовка (не дёргать GLFW каждый кадр)

    // --- сцена и рендер превью ---
    std::unique_ptr<Scene> m_scene;
    Camera m_camera;                       // редакторская камера (Viewport)
    EditorSceneRenderer m_renderer;        // весь превью-рендер (теней/Viewport/Game/PostFX/гизмо)

    // --- общее состояние инструментов (тулбар + вьюпорт делят через host) ---
    EditorTools m_tools;

    // --- Play-режим (см. EditorPlaySession.h) ---
    //
    // Игра, запущенная внутри редактора: снимок сцены, скрипты, физика, звук,
    // ввод, захват курсора, пауза и шаг. Полтора десятка полей и порядок их
    // гашения в Stop — там, а не здесь: это единственное место редактора, где
    // сцена живёт не как документ, и смешивать его с редактированием опасно.
    EditorPlaySession m_play;
    // Сцену на время Stop заменяет сессия, поэтому ей отдаётся указатель НА
    // указатель. Отдельное поле, а не &m_scene: unique_ptr — не Scene*.
    Scene* m_scenePtr = nullptr;
    // Мост к окну — ОДИН на всё время работы редактора, а не новый на каждый
    // Play: подписка на события окна снимается только вместе с окном, и второй
    // мост означал бы два комплекта событий на одно нажатие.
    sage::input::GlfwBridge m_playInputBridge;

    // Раскладка управления ПРОЕКТА — документ, а не работающий ввод: кадры
    // ей никто не считает. Живёт в <проект>/input.sageinput, правится панелью
    // «Управление» и применяется к m_playInput при старте Play — ровно так же,
    // как её применяет собранная игра. Иначе превью игралось бы не тем
    // управлением, что игра, и разницу находили бы уже после сборки.
    sage::input::InputSystem m_projectInput;
    bool m_projectInputDirty = false;
    // --- Undo/Redo (см. EditorHistory.h) ---
    //
    // Историю со сценой знакомит слой: она умеет снять снимок и применить
    // снимок, а КАК это делается — знание сериализатора и сцены, не истории.
    EditorHistory m_history;

    // --- выбор и замок инспектора (см. EditorSelection.h); размеры окон
    //     живут в m_renderer ---
    EditorSelection m_selection;
    glm::mat4 m_view{1.0f}, m_proj{1.0f}; // последние view/proj кадра (гизмо/пикинг)

    // --- docking ---
    bool m_rebuildDockLayout = false; // форс-перестройка (Window > Reset Layout)

    // Общих окон между пространствами БОЛЬШЕ НЕТ: у ассетов, консоли и
    // анимации своё окно в каждом (см. PanelWindowId.h). Вместе с ними ушла и
    // возня с запоминанием узлов: спорить за окно стало некому.

    // Какие панели сейчас на экране (см. EditorPanelVisibility.h): массивом по
    // перечислению, а не одиннадцатью полями bool — иначе каждая новая панель
    // требует правки в трёх местах сразу, и забытая строка в «показать все»
    // означает панель, которую нельзя вернуть.
    EditorPanelVisibility m_panels;
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
    bool m_showIconSheet = false; // страница со всеми иконками (Window > Icon sheet)
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ViewportPanel m_viewport;
    GamePanel m_game;
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
    // --- пространство «Интерфейс» (см. EditorTypes.h) ---
    //
    // Четыре докуемые панели вместо одного окна с колонками внутри. Раньше
    // внутри редактора с доккингом жил кусок без доккинга: дерево, холст и
    // свойства нельзя было ни перетащить, ни отстыковать, ни положить на
    // второй монитор.
    InterfaceHierarchyPanel m_uiHierarchy;
    InterfaceViewportPanel m_uiViewport;
    InterfaceInspectorPanel m_uiInspector;
    InterfacePreviewPanel m_uiPreview;
    EditorWorkspace m_workspace = EditorWorkspace::Scene;
    // Номер объекта-интерфейса, который сейчас верстают. -1 — ни одного;
    // 0 — особая строка «Без интерфейса» (элементы, собранные кодом).
    int m_currentInterface = -1;
    TopBarPanel m_topBar;
    SettingsPanel m_settingsPanel; // окно гибких настроек движка (host.Settings())
    InputPanel m_inputPanel;       // раскладка управления проекта (input.sageinput)
    TemplatesPanel m_templatesPanel; // установка/скачивание шаблонов проектов
    bool m_showTemplates = false;
    // Редактор девятины (см. panels/NineSlicePanel.h) — отдельный инструмент:
    // нарезку подбирают подолгу и по самой картинке, а не по числам вслепую.
    NineSlicePanel m_nineSlice;

    // Инструмент анимации (см. panels/AnimationPanel.h). Панель раскладки, а не
    // инструмент под задачу: анимацию правят рядом со сценой и рядом с
    // вёрсткой, поэтому она докуется в обоих пространствах.
    AnimationPanel m_animation;
    bool m_showNineSlice = false;

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
