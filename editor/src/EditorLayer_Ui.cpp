// ---------------------------------------------------------------------------
// EditorLayer — каркас окна: докинг, меню, строка состояния.
//
// Рама вокруг панелей: пространство докинга, главное меню, строка состояния,
// окно «о программе» и подсказка на пустом доке. Это не редактор как таковой, а
// его оболочка — она не знает ни про сцену, ни про ассеты, и меняется по совсем
// другим поводам.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "CreateAssetDialog.h"
#include "EditorLayer.h"
#include "LuaPlugins.h"
#include "PluginsWindow.h"
#include "FileBrowser.h"
#include "ModelImportDialog.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "CodeEditorApp.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "sage/core/Profiler.h"
#include "EditorIcons.h"
#include "HotkeyScope.h"
#include "ModelMaterialImport.h"
#include "sage/render/DebugView.h"
#include "sage/app/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"
#include "ObjectCatalog.h"
#include "PanelWindowId.h"
#include "PanelWindows.h"

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

} // namespace


// ПЕРЕКЛЮЧЕНИЕ ПРОСТРАНСТВА — не «режим», а смена состава панелей.
//
// Выделение при этом СБРАСЫВАЕТСЯ, и это не мелочь: в сцене выбирают объекты, в
// вёрстке — элементы, и выбранный куб, оставшийся выбранным после перехода к
// интерфейсу, означает инспектор, показывающий меш там, где ждут раскладку.
void EditorLayer::SetWorkspace(EditorWorkspace workspace) {
    if (workspace == m_workspace) return;
    // ВЫБОР ИНТЕРФЕЙСА — ДО СБРОСА ВЫДЕЛЕНИЯ. Самый частый путь в вёрстку:
    // выбрали в сцене объект интерфейса (или его элемент) и переключились. По
    // выделению и понятно, какой интерфейс человек собрался править; после
    // сброса эта подсказка потеряна.
    if (workspace == EditorWorkspace::Interface) ResolveCurrentInterface();
    m_workspace = workspace;
    m_selection.Clear();
    // Своя раскладка у каждого пространства строится при первом показе: узла с
    // таким именем ImGui ещё не знает, и BuildXxxDockLayout сработает сам.
    if (workspace == EditorWorkspace::Interface) {
        m_uiViewport.RequestFit();
        m_uiViewport.RequestFocus();
    } else {
        m_viewport.RequestFocus();
    }
}


// КАКОЙ ИНТЕРФЕЙС ВЕРСТАЕМ.
//
// Правило одно на редактор, и вот почему оно здесь, а не в панели: дерево,
// холст и инспектор обязаны показывать ОДИН И ТОТ ЖЕ интерфейс. Три ответа на
// этот вопрос в трёх панелях — это дерево от одного экрана поверх холста
// другого, и объяснить такую картинку нечем.
void EditorLayer::ResolveCurrentInterface() {
    auto valid = [&](int id) {
        if (id < 0) return false;
        if (id == 0) return true;  // «Без интерфейса» — строка, а не объект
        GameObject obj = m_scene->Get(id);
        return obj.Valid() && m_scene->Registry().all_of<sage::ui::InterfaceComponent>(obj.Entity());
    };

    // 1. Тот, что и был, если он ещё жив: переключение пространств туда-обратно
    // не должно терять место работы.
    if (valid(m_currentInterface)) return;

    // 2. Интерфейс ВЫДЕЛЕННОГО объекта: пришли из сцены, выбрав меню или его
    // кнопку, — значит правят именно его.
    if (m_selection.Primary() >= 0) {
        GameObject sel = m_scene->Get(m_selection.Primary());
        if (sel.Valid()) {
            const entt::entity owner = sage::ui::InterfaceOf(*m_scene, sel.Entity());
            if (owner != entt::null) {
                const IdComponent* id = m_scene->Registry().try_get<IdComponent>(owner);
                m_currentInterface = id ? id->Id : -1;
                if (valid(m_currentInterface)) return;
            }
        }
    }

    // 3. Первый в сцене — по тому же порядку, в каком они показываются.
    const std::vector<entt::entity> all = sage::ui::SortedInterfaces(*m_scene);
    if (!all.empty()) {
        const IdComponent* id = m_scene->Registry().try_get<IdComponent>(all.front());
        m_currentInterface = id ? id->Id : -1;
        return;
    }

    // 4. Интерфейсов нет вовсе. Если в сцене есть элементы без интерфейса (их
    // собрал код или скрипт), показываем их — иначе панель выглядела бы
    // сломанной при непустой сцене.
    m_currentInterface = sage::ui::InterfaceRoots(*m_scene, entt::null).empty() ? -1 : 0;
}

sage::ui::UIScope EditorLayer::UiScope() const {
    // В пространстве сцены — всё как в игре: там интерфейс не правят, а видят.
    if (m_workspace != EditorWorkspace::Interface) return sage::ui::UIScope::All();
    if (m_currentInterface < 0) return sage::ui::UIScope::All();
    if (m_currentInterface == 0) return sage::ui::UIScope::Only(entt::null);
    GameObject obj = m_scene->Get(m_currentInterface);
    if (!obj.Valid()) return sage::ui::UIScope::All();
    return sage::ui::UIScope::Only(obj.Entity());
}

// Панель, живущая своим окном системы, в док не заводится: DockBuilder втянул
// бы её обратно вкладкой, отменив то, о чём человека не спрашивали.
void EditorLayer::DockRegisteredPanels(EditorWorkspace workspace, unsigned int left, unsigned int right,
                                       unsigned int bottom, unsigned int center) {
    using sage::editor::PanelDock;
    m_panelRegistry.ForEachDocked(workspace, [&](const std::string& id, PanelDock slot) {
        if (panelwindows::Detached(id.c_str())) return;
        const ImGuiID node = slot == PanelDock::Left ? left
                           : slot == PanelDock::Right ? right
                           : slot == PanelDock::Bottom ? bottom
                                                       : center;
        ImGui::DockBuilderDockWindow(id.c_str(), node);
    });
}

void EditorLayer::BuildInterfaceDockLayout(unsigned int dockspaceId) {
    // У вёрстки другой главный: ХОЛСТ, а не вьюпорт сцены. Всё остальное
    // стоит вокруг него так же, как в пространстве сцены, — привычка к
    // расположению панелей важнее, чем оригинальность раскладки.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26f, nullptr, &center);

    // Кто куда — из реестра (PanelSpec::InterfaceDock). Общие панели
    // докаются СВОИМИ окнами этого пространства («AssetsUI»): пока окно было
    // одно на оба, раскладка вёрстки забирала его у раскладки сцены (см.
    // PanelWindowId.h). Холст в центре — первым, он вкладка по умолчанию.
    DockRegisteredPanels(EditorWorkspace::Interface, left, right, bottom, center);
    ImGui::DockBuilderFinish(dockspaceId);

    // ПО ИДЕНТИФИКАТОРУ, а не по имени: имя панели переводится («Canvas» /
    // «Холст»), и SetWindowFocus по строке в русском редакторе не находил
    // ничего — молча.
    if (ImGuiWindow* canvas = ImGui::FindWindowByID(ImHashStr("InterfaceViewport")))
        ImGui::FocusWindow(canvas);
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

    // Сброс раскладки возвращает и состав ОКОН, а не только расстановку
    // вкладок: окно, уехавшее на монитор, которого больше нет, иначе не
    // вернуть ничем (см. PanelWindows.h).
    panelwindows::ResetToDefaults();
    // Панель, живущая своим окном системы, в док не заводится: DockBuilder
    // втянул бы её обратно вкладкой, отменив то, о чём человека не спрашивали.
    // Кто куда — из реестра (PanelSpec::SceneDock). Viewport в центре —
    // ПЕРВЫМ: первая добавленная к узлу вкладка становится выбранной, и при
    // Game первым редактор открывался на игровом окне без пикинга и гизмо
    // («выделение не работает»). Game выходит вперёд при входе в Play.
    DockRegisteredPanels(EditorWorkspace::Scene, left, right, bottom, center);
    // Редактора интерфейса здесь больше нет вовсе: вёрстка — ОТДЕЛЬНОЕ рабочее
    // пространство со своими панелями и своей раскладкой (см. EditorTypes.h).
    ImGui::DockBuilderFinish(dockspaceId);

    ImGui::SetWindowFocus("Viewport");
}


void EditorLayer::DrawDockspaceAndMenu() {
    // Заявки на клавиши — ровно на один кадр, и сбрасываются ДО панелей: иначе
    // забранный вчера Delete молчал бы сегодня (см. HotkeyScope.h).
    sage::editor::hotkeys::NewFrame();

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
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        // ОКНУ-ХОСТУ ПРОКРУТКА НЕ ПОЛОЖЕНА, И БЕЗ ЭТОГО ФЛАГА ОНА ПОЯВЛЯЛАСЬ.
        //
        // Хост — это весь редактор: тулбар, док-пространство и статус-бар,
        // растянутые ровно по окну. Прокручивать его некуда, но ImGui об этом
        // не знает: содержимое на волосок выше окна (полоса статуса плюс
        // отступ строки) — и он заводит вертикальную полосу прокрутки. На
        // экране это серая лента во всю высоту у правого края, отнимающая
        // место у инспектора, причём ездить по ней некуда.
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##SageEditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Верхняя панель — полоса сразу под меню-баром: Play/Pause/Stop и кнопки
    // окон. Рисуется ДО dockspace, чтобы занять свою полосу.
    //
    // ИНСТРУМЕНТОВ здесь больше нет: гизмо, привязка, показ и раскладка видов
    // переехали виджетом ПОВЕРХ вьюпорта (ViewportTools.cpp) — туда, где ими
    // работают. Раньше за сеткой и шагом привязки мышь ездила от объекта к
    // верхнему краю окна и обратно.
    m_topBar.Draw(*this, TopBarPanel::kHeight);

    // У КАЖДОГО ПРОСТРАНСТВА СВОЁ ДОК-ПРОСТРАНСТВО, и это не деталь
    // реализации. Общее означало бы, что переключение пересобирает раскладку
    // заново — то есть настроенное расположение панелей теряется при каждом
    // переходе «сцена — интерфейс». Разные узлы ImGui запоминает каждый свой,
    // в том же imgui.ini, и обе раскладки переживают перезапуск.
    const bool interfaceSpace = m_workspace == EditorWorkspace::Interface;
    ImGuiID dockspaceId =
        ImGui::GetID(interfaceSpace ? "SageInterfaceDock" : "SageDockSpace");
    // ВТОРОЕ ДОК-ПРОСТРАНСТВО ДЕРЖИМ ЖИВЫМ, хоть и не показываем.
    //
    // Не подать его ImGui значит сказать «этого дока больше нет»: узлы, в
    // которых не осталось ни одного окна, убираются, а окна из неподанного
    // дока выпадают наружу. Именно так панель ассетов и оказывалась висящей
    // посреди экрана: раскладка вёрстки забирала её себе, нижний узел
    // раскладки сцены пустел и исчезал, а вернуться ей было уже некуда.
    // KeepAliveOnly — это ровно «узлы живы, окна свои не отпускают».
    const ImGuiID otherDockspaceId =
        ImGui::GetID(interfaceSpace ? "SageDockSpace" : "SageInterfaceDock");
    if (ImGui::DockBuilderGetNode(otherDockspaceId) != nullptr)
        ImGui::DockSpace(otherDockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);

    // Куда возвращать панель, у которой сняли галочку «в отдельном окне».
    panelwindows::SetHomeDock(dockspaceId);
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        if (interfaceSpace) BuildInterfaceDockLayout(dockspaceId);
        else BuildDefaultDockLayout(dockspaceId);
    }

    // Док-пространство занимает всё между тулбаром и статус-баром.
    const ImVec2 dockMin = ImGui::GetCursorScreenPos();
    // КРЕСТИК У ПАНЕЛИ ОДИН. ImGui рисует их два: свой у каждой вкладки и ещё
    // один в правом углу узла — для вкладки, выбранной сейчас. Второй ничего
    // не добавляет (он закрывает ту же панель, что и первый), а спрашивают о
    // нём как об ошибке: два одинаковых крестика подряд читаются как «один из
    // них закроет что-то другое».
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -kStatusBarHeight),
                     ImGuiDockNodeFlags_NoCloseButton);
    const ImVec2 dockMax = ImGui::GetItemRectMax();
    DrawStatusBar(kStatusBarHeight);

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        if (EditorIcons::BeginMenu("file", T("File"))) {
            if (EditorIcons::MenuItem("project", T("New Project..."))) openDialog = "New Project";
            if (EditorIcons::MenuItem("open", T("Open Project..."))) openDialog = "Open Project";
            if (EditorIcons::MenuItem("list", T("Project Launcher..."))) m_launcherRequested = true;
            ImGui::Separator();
            if (EditorIcons::MenuItem("scene", T("New Scene"))) NewSceneForUser();
            if (EditorIcons::MenuItem("open", T("Open Scene..."))) openDialog = "Open Scene";

            // Сцены открытого проекта — прямой доступ без файлового диалога.
            if (EditorIcons::BeginMenu("scene", T("Project Scenes"))) {
                std::error_code ec;
                std::vector<fs::path> scenes;
                for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
                    if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
                }
                std::sort(scenes.begin(), scenes.end());
                if (scenes.empty()) ImGui::TextDisabled("%s", T("(no scenes yet)"));
                for (const fs::path& scenePath : scenes) {
                    bool current = scenePath == m_scenePath;
                    if (EditorIcons::MenuItemSelected("scene", scenePath.filename().string().c_str(),
                                                      current)) {
                        LoadSceneFromFile(scenePath);
                    }
                }
                ImGui::EndMenu();
            }

            if (EditorIcons::MenuItem("save", T("Save Scene"), "Ctrl+S")) SaveCurrentScene();
            if (EditorIcons::MenuItem("save", T("Save Scene As..."))) openDialog = "Save Scene As";
            ImGui::Separator();
            if (EditorIcons::MenuItem("build", T("Build Game..."))) {
                openDialog = "Build Game";
            }
            ImGui::Separator();
            if (EditorIcons::MenuItem("exit", T("Exit"))) {
                m_closeAfterPrompt = true;
                AskUnsaved(nullptr);
                if (!m_unsavedPrompt) sage::Application::Get().Close();
            }
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("pencil", T("Edit"))) {
            if (EditorIcons::MenuItem("undo", T("Undo"), "Ctrl+Z",
                                      m_history.CanUndo() && !InPlayMode())) Undo();
            if (EditorIcons::MenuItem("redo", T("Redo"), "Ctrl+Y",
                                      m_history.CanRedo() && !InPlayMode())) Redo();
            ImGui::Separator();
            bool hasSel = m_scene->Get(m_selection.Primary()).Valid();
            if (EditorIcons::MenuItem("copy", T("Duplicate"), "Ctrl+D", hasSel)) DuplicateSelected();
            if (EditorIcons::MenuItem("trash", T("Delete"), "Del", hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("play", T("Play"))) {
            if (EditorIcons::MenuItem("play", T("Play"), nullptr, !m_play.Active())) StartPlay();
            if (EditorIcons::MenuItem("pause", T("Pause"), nullptr, m_play.Playing())) PausePlay();
            if (EditorIcons::MenuItem("play", T("Resume"), nullptr, m_play.Paused())) ResumePlay();
            if (EditorIcons::MenuItem("stop", T("Stop"), nullptr, InPlayMode())) StopPlay();
            ImGui::Separator();
            // Тесты проекта (*.test.lua) — в настоящем Play, итог в консоли.
            if (EditorIcons::MenuItem("script", T("Run Lua Tests"), nullptr,
                                      m_project.Loaded() && !m_luaTests.Running))
                RunLuaTests();
            ImGui::EndMenu();
        }
        // ОБЪЕКТ — ОДИН КАТАЛОГ НА ВЕСЬ РЕДАКТОР.
        //
        // Здесь был свой список пунктов, а под правой кнопкой в иерархии —
        // свой, вдвое короче. Теперь оба рисуют общий каталог
        // (editor/src/ObjectCatalog.h), и разойтись им нечем.
        if (EditorIcons::BeginMenu("cube", T("Object"))) {
            if (const char* pick = sage::editor::objectcatalog::DrawMenu()) CreateCatalogObject(pick);
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("layout", T("Window"))) {
            // --- МЕНЮ РАЗБИТО ПО СМЫСЛУ, А НЕ ВЫСЫПАНО СПИСКОМ --------------
            //
            // Панелей полтора десятка, и плоским столбцом они читаются как
            // случайный набор: рядом стояли «Иерархия» и «Элементы» — почти
            // одинаковые слова про совершенно разные вещи (объекты сцены и
            // элементы интерфейса), — а между ними «Ассеты» и «Консоль»,
            // которые есть и там и там. Найти нужное можно было только прочитав
            // все пятнадцать строк подряд.
            //
            // Разделы — те же, что и рабочие места редактора: сцена, интерфейс,
            // общие панели, инструменты. Человек ищет окно в том разделе, в
            // котором он сейчас работает, и читает четыре строки вместо
            // пятнадцати.
            // Пункты — из реестра панелей (PanelRegistry.h): панель, которую
            // зарегистрировали, появляется здесь сама, в своём разделе.
            namespace pe = sage::editor;
            Sage::UI::MenuSection(T("Scene"), true);
            m_panelRegistry.DrawMenuSection(pe::PanelSection::Scene);
            Sage::UI::MenuSection(T("Interface"));
            m_panelRegistry.DrawMenuSection(pe::PanelSection::Interface);
            // Общие стоят в ОБОИХ рабочих местах — потому и отдельным разделом.
            Sage::UI::MenuSection(T("Shared"));
            m_panelRegistry.DrawMenuSection(pe::PanelSection::Shared);
            // Инструменты открывают ПОД ЗАДАЧУ и закрывают: они не участвуют в
            // раскладке по умолчанию и не живут на экране постоянно.
            Sage::UI::MenuSection(T("Tools"));
            m_panelRegistry.DrawMenuSection(pe::PanelSection::Tools);
            EditorIcons::MenuItemToggle("nineslice", T("9-slice editor"), &m_showNineSlice);
            EditorIcons::MenuItemToggle("grid", T("Icon sheet"), &m_showIconSheet);
            // Окна плагинов — своим разделом, только если они есть.
            {
                bool any = false;
                for (const pe::PanelSpec* spec : m_panelRegistry.All())
                    any = any || spec->Section == pe::PanelSection::Plugins;
                if (any) {
                    Sage::UI::MenuSection(T("Plugins"));
                    m_panelRegistry.DrawMenuSection(pe::PanelSection::Plugins);
                }
            }
            // --- РАСКЛАДКА: то, что относится к самому окну редактора -------
            //
            // Внизу, а не первой строкой, как было: «сбросить раскладку»
            // нажимают раз в месяц, а стояло оно там, куда взгляд падает
            // первым, — перед списком окон, ради которого меню и открывают.
            Sage::UI::MenuSection(T("Layout"));
            // Сброс раскладки возвращает и сами панели: закрытая вкладка иначе
            // не восстанавливалась «сбросом», хотя именно этого от него ждут.
            if (EditorIcons::MenuItem("refresh", T("Reset Layout"))) {
                m_panelRegistry.Restore();
                m_rebuildDockLayout = true;
            }
            EditorIcons::MenuItemToggle("grid", T("Show Grid"), &m_tools.ShowGrid);

            // --- Панель ОТДЕЛЬНЫМ ОКНОМ СИСТЕМЫ -----------------------------
            //
            // Меню, а не только перетаскивание мышью: вытащить панель в своё
            // окно раньше можно было единственным жестом — дотащить её за
            // вкладку за край главного окна, — а вернуть обратно нечем, кроме
            // сброса всей раскладки. И главное, вытащенное окно слипалось с
            // главным обратно, стоило его туда надвинуть (см. PanelWindows.h).
            if (EditorIcons::BeginMenu("window", T("Separate window"))) {
                ImGui::TextDisabled("%s", T("Panel becomes a window of the system"));
                ImGui::Separator();
                m_panelRegistry.DrawDetachMenu(m_workspace);
                ImGui::Separator();
                // Путь назад для случая «окно уехало на второй монитор, а
                // монитора больше нет»: оттуда его не достать ни мышью, ни
                // галочкой — окна не видно, чтобы за него взяться.
                if (EditorIcons::MenuItem("layout", T("Bring all windows back")))
                    panelwindows::AttachAll();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            EditorIcons::MenuItemToggle("gear", T("Game Settings..."), &PanelVisible(EditorPanel::Settings));
            // Управление — рядом с настройками игры: это тоже содержимое
            // проекта, которое уезжает в собранную игру, а не настройка
            // редактора.
            EditorIcons::MenuItemToggle("keyboard", T("Controls..."), &PanelVisible(EditorPanel::Input));

            // Язык интерфейса. Здесь, а не в окне Settings: то окно правит
            // настройки ИГРЫ и сохраняется в проект, а язык — настройка
            // редактора и живёт в профиле пользователя. Смешать их значило бы
            // получить проект, навязывающий язык всем, кто его откроет.
            // Оформление — рядом с языком и по той же причине: это настройка
            // РЕДАКТОРА, она живёт в профиле человека, а не в проекте. Проект,
            // навязывающий свою тему каждому, кто его открыл, — не то, чего
            // ждут от проекта.
            if (EditorIcons::BeginMenu("theme", T("Appearance"))) {
                for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
                    const bool active = theme.Id == EditorTheme::CurrentId();
                    // Название темы не переводится — как и название языка: по
                    // нему тему находят в файле настроек и в themes/*.json.
                    if (EditorIcons::MenuItemSelected("theme", theme.Name.c_str(), active) && !active) {
                        EditorTheme::SetTheme(theme.Id);
                        SetStatusMessage(T("Theme changed"));
                    }
                }
                ImGui::Separator();
                // Масштаб интерфейса — здесь же: на экране 4K шрифт в 16 точек
                // читается лупой, и «сделайте покрупнее» до сих пор решалось
                // только сменой разрешения всей системы.
                ImGui::TextDisabled("%s", T("Interface scale"));
                float scale = EditorTheme::UiScale();
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat("##uiscale", &scale, 0.75f, 2.0f, "%.2fx"))
                    EditorTheme::SetUiScale(scale);
                if (EditorIcons::MenuItem("refresh", T("Reset scale"), nullptr, scale != 1.0f))
                    EditorTheme::SetUiScale(1.0f);
                ImGui::Separator();
                // Выгрузка темы файлом — вот ради чего вся система: взять
                // текущую за основу, поправить цвета в текстовом редакторе и
                // положить обратно в themes/. Пересборка не нужна.
                if (EditorIcons::MenuItem("save", T("Export theme to themes/..."))) {
                    const std::string dir = EditorTheme::ThemesDir();
                    const std::string path = dir + "/" + EditorTheme::Current().Id + "-copy.json";
                    if (EditorTheme::ExportTheme(EditorTheme::Current(), path))
                        SetStatusMessage(std::string(T("Theme saved:")) + " " + path);
                    else
                        SetStatusMessage(T("Could not write the theme file"));
                }
                ImGui::EndMenu();
            }
            // Чем открывать код. Здесь же, где язык и оформление, и по той же
            // причине: это настройка РЕДАКТОРА, она живёт в профиле человека.
            // Проект, навязывающий соавтору свою IDE, — не то, чего ждут от
            // проекта.
            if (EditorIcons::BeginMenu("code", T("Code editor"))) {
                namespace codeapp = sage::editor::codeapp;
                const std::string current = codeapp::CurrentId();
                // Системная ассоциация — всегда первая и всегда доступна: это
                // единственный вариант, который не может «не установиться».
                if (EditorIcons::MenuItemSelected("gear", T("As the system opens it"), current.empty()))
                    codeapp::SetCurrent("");
                const std::vector<codeapp::App>& apps = codeapp::Available();
                if (!apps.empty()) ImGui::Separator();
                for (const codeapp::App& app : apps) {
                    // Имя программы НЕ переводится — как название темы и языка:
                    // по нему её узнают в меню «Пуск» и в своей же панели задач.
                    if (EditorIcons::MenuItemSelected("code", app.Name.c_str(), app.Id == current))
                        codeapp::SetCurrent(app.Id);
                }
                ImGui::Separator();
                if (apps.empty()) {
                    // Пустой список — не поломка, и сказать об этом надо прямо:
                    // иначе меню из одного пункта читается как «выбор сломан».
                    ImGui::TextDisabled("%s", T("No code editors found on this machine"));
                }
                ImGui::TextDisabled("%s", T("Only what is actually installed is listed"));
                if (EditorIcons::MenuItem("search", T("Search again"))) {
                    codeapp::Rescan();
                    SetStatusMessage(T("Code editors: ") +
                                     std::to_string(codeapp::Available().size()));
                }
                ImGui::EndMenu();
            }
            if (EditorIcons::BeginMenu("language", T("Language"))) {
                for (const sage::editor::LanguageInfo& lang : sage::editor::AvailableLanguages()) {
                    const bool active = lang.Code == sage::editor::CurrentLanguageCode();
                    // Название языка НЕ переводится: человек, случайно
                    // переключивший интерфейс на незнакомый, должен найти
                    // дорогу назад по слову «English», а не по переводу.
                    if (EditorIcons::MenuItemSelected("language", lang.Name.c_str(), active) && !active) {
                        sage::editor::SetLanguage(lang.Code);
                        SetStatusMessage(T("Interface language changed"));
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        // ПЛАГИНЫ — своим меню: пункты плагинов появляются здесь же, под
        // управлением, и искать «куда плагин добавил кнопку» не приходится.
        if (EditorIcons::BeginMenu("gear", T("Plugins"))) {
            if (EditorIcons::MenuItem("gear", T("Manage plugins..."))) sage::editor::pluginswindow::Open();
            if (EditorIcons::MenuItem("plus", T("Create plugin..."))) sage::editor::pluginswindow::OpenCreate();
            ImGui::Separator();
            sage::editor::luaplugins::DrawMenuItems(*this);
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("question", T("Help"))) {
            EditorIcons::MenuItemToggle("info", T("About SAGE..."), &m_showAbout);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре.
        std::string status = std::string(T("Project:")) + " " + m_project.Name();
        float w = ImGui::CalcTextSize(status.c_str()).x + 16.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status.c_str());

        ImGui::EndMenuBar();
    }

    // Модалки диалогов и окно настроек — самостоятельные панели, но рисуются
    // ЗДЕСЬ, на уровне окна-хоста: модалка ImGui совпадает с OpenPopup по
    // ID-стеку окна, поэтому открытие и отрисовку нельзя разносить по окнам.
    // Открыть диалог по имени из переменной окружения — тем же путём, каким его
    // открывает пункт меню. Нужно затем же, зачем SAGE_EDITOR_ICON_SHEET:
    // модалку нельзя увидеть headless, а значит, нечем проверить, как она
    // выглядит; открывалась она только мышью.
    // Не в первом кадре: на первых кадрах редактор перестраивает раскладку
    // доков, а это фокусирует окна панелей — и ImGui закрывает всплывающие
    // окна, открытые над окном-хостом. Модалка, открытая в кадре 1, исчезала
    // ровно поэтому, и на скриншоте её не было.
    if (!openDialog && m_pendingDialog && m_frameCounter > 4) {
        openDialog = m_pendingDialog;
        m_pendingDialog = nullptr;
    }
    // Окно настроек импорта модели — не из набора диалогов, у него своя
    // очередь моделей; для снимка ему дают вымышленную модель из набора.
    if (openDialog && std::strcmp(openDialog, "model-import") == 0) {
        sage::editor::modelimport::Ask({std::filesystem::path("Stylized Nature MegaKit/FBX/CommonTree_1.fbx"),
                                        std::filesystem::path("Stylized Nature MegaKit/FBX/Bush_Common.fbx")});
        openDialog = nullptr;
    }
    // Диалог файлов — тоже для снимка: его макет (строка навигации, места,
    // таблица, подвал) проверяется только глазами.
    if (openDialog && std::strcmp(openDialog, "file-dialog") == 0) {
        static char path[512] = "";
        FileBrowser::Config c;
        c.Title = T("Open scene");
        c.Filters = {".sage"};
        c.FilterLabel = T("Scenes (*.sage)");
        c.StartDir = m_project.Loaded() ? m_project.Dir() : std::filesystem::current_path();
        m_dialogs.Browse(c, path, sizeof(path));
        openDialog = nullptr;
    }
    if (openDialog) m_dialogs.Open(openDialog);
    m_dialogs.Draw(*this);
    // Отчёт о падении — ПЕРЕД предложением восстановить сцену: сначала «что
    // случилось», потом «что с этим делать».
    DrawCrashReport();
    DrawRecoveryPrompt();
    // Вопрос о несохранённой сцене. Сюда он попадает и от пункта «Выход», и от
    // крестика окна — тот приходит через EditorLayer::OnCloseRequest (см.
    // Layer.h): проверять флаг GLFW прямо здесь бесполезно, цикл выходит
    // раньше, чем этот код выполнится.
    DrawUnsavedPrompt();
    m_settingsPanel.Draw(*this, m_panels[EditorPanel::Settings]);
    m_inputPanel.Draw(*this, m_panels[EditorPanel::Input]);
    m_assets.Tick(*this);           // пакетная конвертация идёт по кадрам, а не одним куском
    m_nineSlice.Draw(*this, m_showNineSlice);
    // Анимация, Tween и профилировщик рисуются вместе с остальными панелями —
    // из реестра (EditorLayer_Panels.cpp).
    if (m_showIconSheet) EditorIcons::DrawSheet(&m_showIconSheet);
    m_confirm.Draw();
    // Окно настроек импорта модели (ModelImportDialog.h): его заказывают
    // панели, когда модель впервые попадает в проект или в сцену.
    sage::editor::modelimport::Draw();
    sage::editor::createasset::Draw(*this);
    DrawAboutWindow();

    ImGui::End();

    // Все панели закрыты — на месте редактора пустой прямоугольник. Молчать
    // здесь нельзя: человек видит серое поле и не знает, что перед ним
    // результат его же крестиков, а не поломка. Подсказка рисуется ПОСЛЕ
    // окна-хоста и отдельным окном: внутри хоста её накрывает фон пустого
    // док-узла, который ImGui кладёт в фоновый канал списка отрисовки.
    if (!m_panelRegistry.AnyVisible()) DrawEmptyDockHint(dockMin.x, dockMin.y, dockMax.x, dockMax.y);

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    // Палитра команд — ДО проверки на ввод текста: Ctrl+K обязан открывать её
    // и из поля поиска ассетов, иначе «горячая клавиша, которая иногда не
    // работает» хуже отсутствующей.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) m_palette.Open(m_paletteQuery);
    m_palette.Draw(m_commands);

    if (!io.WantTextInput) {
        // Delete — только если его не забрала панель со своим смыслом этой
        // клавиши (анимация убирает ключ, ассеты — файл, список элементов —
        // элемент). Без этой проверки срабатывали ОБА: человек убирал ключ в
        // линейке времени и вместе с ним терял объект из сцены (HotkeyScope.h).
        //
        // БЕЗ АВТОПОВТОРА. ImGui::IsKeyPressed по умолчанию повторяет нажатие,
        // пока клавишу держат, — и для «камера вперёд» это правильно, а для
        // «удалить» и «дублировать» означает, что задержавшаяся на четверть
        // секунды клавиша делает работу дважды. Ровно так Ctrl+D давал две
        // копии, одну поверх другой: вторую замечали не сразу, а потом
        // находили в сцене лишний объект неизвестно откуда.
        namespace hk = sage::editor::hotkeys;
        if (!hk::Claimed(hk::Key::Delete) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            DeleteSelected();
        if (!hk::Claimed(hk::Key::Duplicate) && io.KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_D, false))
            DuplicateSelected();
        // Ctrl+S работает ВСЕГДА, а не только у сцены с именем: у новой сцены
        // имени нет, и «горячая клавиша молча ничего не делает» — это ровно то,
        // как выглядит потерянная работа.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveCurrentScene();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) Redo();
    }
}
