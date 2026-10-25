#include "sage/core/Problems.h"
#include "sage/app/CoreSystems.h"
#include "EditorLayer.h"
#include "CursorCapture.h"
#include "LuaPlugins.h"
#include "PluginsWindow.h"
#include "sage/scene/Signals.h"
#include "sage/audio/AudioEngine.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>

#include "sage/events/Events.h"
#include "sage/vars/VarsComponent.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "TemplateStore.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Thumbnails.h"

#include "sage/assets/Quarantine.h"
#include "sage/render/DebugView.h"
#include "sage/app/Application.h"
#include "ScreenColor.h"
#include "ui/ColorPicker.h"
#include "sage/rhi/GraphicsDevice.h"
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
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "AssetExt.h"
#include "Localization.h"
#include "PanelWindowId.h"
#include "PanelWindows.h"
#include "Progress.h"



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

// ============================================================================
//  Жизненный цикл
// ============================================================================


// ============================================================================
//  РЕЕСТР КОМАНД
//
//  Одно действие — одна запись. До этого «сохранить сцену» жило в трёх местах
//  сразу (пункт меню, кнопка тулбара, обработчик Ctrl+S), и три копии одного
//  действия — это три возможности разойтись: у меню появилась проверка, у
//  горячей клавиши нет. Такие расхождения не ловятся тестом, а находятся
//  жалобой.
//
//  Наполняется один раз при запуске. Читают его палитра команд (Ctrl+K) и
//  обработчик горячих клавиш.
// ============================================================================
void EditorLayer::RegisterCommands() {
    using Sage::UI::Command;
    const auto scene = std::string(T("Scene"));
    const auto object = std::string(T("Object"));
    const auto window = std::string(T("Window"));
    const auto view = std::string(T("View"));

    auto hasProject = [this] { return m_project.Loaded(); };
    auto hasSelection = [this] { return m_selection.Primary() != 0; };

    m_commands.Add({"scene.save", T("Save Scene"), scene, "Ctrl+S", "scene", hasProject,
                    [this] { SaveCurrentScene(); }});
    m_commands.Add({"scene.new", T("New Scene"), scene, "", "scene", hasProject,
                    [this] { NewSceneWithPrompt(); }});
    m_commands.Add({"edit.undo", T("Undo"), scene, "Ctrl+Z", "refresh",
                    [this] { return m_history.CanUndo(); }, [this] { Undo(); }});
    m_commands.Add({"edit.redo", T("Redo"), scene, "Ctrl+Shift+Z", "refresh",
                    [this] { return m_history.CanRedo(); }, [this] { Redo(); }});

    m_commands.Add({"play.start", T("Play"), scene, "", "play",
                    [this] { return !m_play.Active(); },
                    [this] { StartPlay(); }});
    m_commands.Add({"play.stop", T("Stop"), scene, "", "stop",
                    [this] { return InPlayMode(); }, [this] { StopPlay(); }});
    m_commands.Add({"play.luaTests", T("Run Lua Tests"), scene, "", "script",
                    [this] { return m_project.Loaded() && !m_luaTests.Running; },
                    [this] { RunLuaTests(); }});

    m_commands.Add({"object.duplicate", T("Duplicate"), object, "Ctrl+D", "cube",
                    hasSelection, [this] { DuplicateSelected(); }});
    m_commands.Add({"object.delete", T("Delete"), object, "Del", "cube",
                    hasSelection, [this] { DeleteSelected(); }});

    // Панели — по одной команде на панель: «где включается консоль» перестаёт
    // быть поиском по меню. Команды даёт реестр — и для панелей, которые
    // зарегистрируют позже (модуль, плагин).
    m_panelRegistry.AttachCommands(m_commands, window);
    m_commands.Add({"window.settings", T("Game Settings..."), window, "", "project", {},
                    [this] { m_panels[EditorPanel::Settings] = true; }});
    m_commands.Add({"window.reset", T("Reset Layout"), window, "", "grid", {},
                    [this] { m_panelRegistry.Restore(); m_rebuildDockLayout = true; }});

    // Оформление — командами тоже: тема меняется без похода в меню, и это тот
    // случай, когда её меняют часто (свет в комнате не постоянен).
    for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
        const std::string id = theme.Id;
        m_commands.Add({"theme." + id, std::string(T("Theme:")) + " " + theme.Name, view, "",
                        "sun", {}, [id] { EditorTheme::SetTheme(id); }});
    }
    m_commands.Add({"view.grid", T("Show Grid"), view, "", "grid", {},
                    [this] { m_tools.ShowGrid = !m_tools.ShowGrid; }});
}

// История отката знакомится со сценой ЗДЕСЬ: она умеет снять снимок и
// применить снимок, а как именно это делается — знание сериализатора, не
// истории (см. EditorHistory.h).
EditorLayer::EditorLayer()
    : sage::Layer("Editor"),
      m_history([this] { return SceneSerializer::SaveToString(*m_scene); },
                [this](const std::string& snapshot) { return RestoreSceneFromString(snapshot); }) {}

void EditorLayer::OnAttach() {
    InitImGui();
    InitEditorServices();
    InitPlugins();
    ApplyStartupHooks();
    UpdateWindowTitle();
}

void EditorLayer::OnDetach() {
    // Список строк, которым не нашлось перевода. Без него новая панель молча
    // выходит по-английски, и узнаётся об этом от пользователя, а не от
    // проверки: SAGE_EDITOR_L10N_MISSING=<файл> выгружает их при выходе.
    if (const std::string path = sage::EnvString("SAGE_EDITOR_L10N_MISSING"); !path.empty()) {
        sage::editor::DumpMissingKeys(path);
    }

    // Чистое завершение — единственное доказательство, что падения не было:
    // файл автосохранения снимается, и следующий запуск не спросит про
    // восстановление. Файл аварийного сброса (sage-recovered.sage) НЕ трогаем —
    // его удаляет только сам пользователь в диалоге.
    {
        std::error_code ec;
        fs::remove("sage-autosave.sage", ec);
    }
    sage::CrashHandler::Uninstall();  // дальше рушить уже нечего, а GL-контекст уходит
    m_console.Detach();    // сток ссылается на панель — снять до разрушения
    // GPU-ресурсы панелей — ДО разрушения контекста. Превью материала держит
    // свой буфер и куб окружения; их деструктор сработал бы уже после смерти
    // контекста, и редактор падал бы при выходе — после всей работы, когда
    // списать падение уже не на что.
    m_inspector.Shutdown();
    m_assets.Shutdown();
    m_launcher.Shutdown();   // обложки проектов — тоже текстуры
    m_plugins.UnloadAll(); // ДО разрушения ImGui-контекста — плагины рисуют через тот же ImGui
    sage::editor::luaplugins::Shutdown();
    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    // Кэш префабов держит РАЗОБРАННЫЕ СЦЕНЫ, а в них — меши на GPU. Он
    // статический и умирает на exit(), то есть уже ПОСЛЕ гибели контекста:
    // деструктор геометрии зовёт драйвер, которого больше нет. Ровно на этом
    // редактор и падал при выходе — после всей работы, когда всё сохранено и
    // списать падение не на что.
    sage::scene::ClearPrefabCache();
    ResourceManager::Instance().Clear();
}

void EditorLayer::OnUpdate(float dt) {
    // --- Автосохранение ---
    //
    // Пишет НЕ поверх рабочей сцены, а в отдельный файл рядом. Поверх нельзя:
    // автосохранение — это страховка от падения и от собственной ошибки, а
    // страховка, затирающая оригинал, страховкой не является. Восстановление
    // предлагается при следующем запуске.
    //
    // Только в режиме правки: во время Play сцена живёт по игровым правилам, и
    // сохранять её состояние значило бы записывать середину игры вместо уровня.
    m_recovery.Tick(dt, m_sceneDirty && m_scene != nullptr, m_play.Active(),
                    [this](const std::string& path) {
                        try {
                            SceneSerializer::Save(*m_scene, path);
                            return true;
                        } catch (const std::exception& e) {
                            LOG_WARN("Editor") << "Автосохранение не удалось: " << e.what();
                            return false;
                        }
                    });

    // Логика правки — событийная, живёт в панелях. Единственный
    // "симуляционный" тик — Play: скрипты сущностей, пока не пауза.
    // Кадр ввода открывается ВСЕГДА, а не только в Play. Причина не в красоте:
    // мост шлёт события окна в очередь непрерывно, пока редактор открыт, и
    // очередь, которую никто не разбирает, растёт без предела — час правки
    // сцены это десятки тысяч движений мыши в памяти. Заодно это даёт панели
    // «Управление» живые события: назначить клавишу можно только поймав её.
    m_playInputBridge.PollGamepads();
    m_play.Input().BeginFrame();
    // Способ захвата курсора — настройка игрока из sage.cfg проекта: в Play
    // редактора курсор держится так же, как в собранной игре.
    sage::Application::Get().GetWindow().SetCursorLockMode(Settings().CursorLock);

    if (m_play.Playing()) {
        // Ввод игре — только пока в фокусе панель Game (см. EditorPlayInput).
        // В остальное время всё отпускается: клавиши уходят редактору, а игра
        // не остаётся идти вперёд сама. Простое «не считать действия» тут не
        // годится — они застыли бы нажатыми.
        // Идут Lua-тесты — игра «в фокусе», куда бы ни смотрел человек: тест
        // нажимает клавиши (Test.key), и без фокуса ввод отпускался бы каждый
        // кадр, то есть тест проверял бы не игру, а окно редактора.
        const bool gameFocused = m_game.Focused() || m_luaTests.Running;
        m_play.Cursor().SetGameFocused(gameFocused);
        // Захват — окну, в котором панель Game: вытащенная из дока, она живёт
        // в своём окне системы, и захват главного мышь в ней не держал.
        if (gameFocused && m_play.Cursor().CursorCaptured())
            sage::Application::Get().GetWindow().SetCursorCaptureTarget((GLFWwindow*)m_game.PlatformWindow());
        m_play.Cursor().SyncCapture();
        if (gameFocused) {
            // Тот же порядок, что в собранной игре: устройства -> интерфейс
            // сцены -> действия (см. InputSystem::BeginFrame).
            UpdatePlayUiInput(dt);
            m_play.Input().UpdateActions(dt);

            // ESC отпускает захваченный курсор, не выходя из Play: иначе из
            // игры от первого лица в редакторе было бы не выбраться мышью.
            //
            // По НАЖАТИЮ, и нажатие забирается у игры. Раньше курсор
            // отпускался всё время, пока ESC зажат, а скрипт («ESC —
            // переключить курсор») на том же нажатии захватывал его снова:
            // следующий ESC игра захватывала, редактор тут же отпускал, и
            // вернуть мышь в игру было нельзя — захват срабатывал один раз.
            m_play.Cursor().HandleEscape(m_play.Input());
        } else {
            m_play.Input().ReleaseAll();
            UpdatePlayUiInput(dt);
        }
    }
    // Время сцены для uTime собственных шейдеров + горячая перезагрузка
    // изменённых .vert/.frag: правка шейдера видна во вьюпорте сразу.
    m_renderer.Tick(dt);
    ResourceManager::Instance().ReloadChangedShaders();
    // И ресурсы: модель или материал могли поправить снаружи (3D-редактор, другой
    // редактор, скрипт). Без этого движок показывал бы прежние данные до
    // перезапуска — ровно то, что выглядит как «правка не применилась».
    // И О ПЕРЕЧИТЫВАНИИ ГОВОРИМ ВСЛУХ. Молчаливая перезагрузка неотличима от
    // её отсутствия: человек переэкспортировал модель, посмотрел в редактор и
    // не понял, новое он видит или старое, — а спрашивать об этом некого.
    //
    // ПОРЦИЯМИ: 32 файла за кадр. Опрос всех разом стоил по системному вызову
    // на каждый загруженный ресурс В КАЖДОМ КАДРЕ — после просмотра набора
    // ассетов их сотни, и FPS падал вдвое. При 60+ кадрах в секунду даже тысяча
    // ресурсов обходится меньше чем за секунду.
    if (const int n = ResourceManager::Instance().ReloadChangedAssets(32); n > 0) {
        SetStatusMessage(T("Files changed on disk — reloaded: ") + std::to_string(n));
    }
    // Обложки картинок, доехавшие из фонового потока, — в видеопамять. Здесь, а
    // не в файловом диалоге: кэш обложек один на редактор, и заливать готовое
    // обязан тот, у кого есть контекст видеокарты, независимо от того, какая
    // панель эти обложки заказала.
    thumbs::Pump();
    // И СКРИПТЫ. Их правят во внешнем редакторе (своего в SAGE нет намеренно), и
    // до сих пор правка не значила ничего, пока игру не перезапустят: цикл
    // «поправил число — посмотрел» стоил прохождения уровня заново. Зовётся и в
    // правке, и в Play: в правке привязанных скриптов просто нет, и обход
    // пустого списка ничего не стоит.
    if (m_play.Scripts()) {
        int n = m_play.Scripts()->ReloadChangedScripts();
        // Скрипты объектов ведёт система скриптинга, уровневые — прежний
        // движок. Перечитывать надо и то, и другое: человек правит файл, а не
        // «объектный скрипт» или «уровневый».
        if (m_play.Scripting()) n += m_play.Scripting()->ReloadChanged();
        if (n > 0) SetStatusMessage(T("Scripts reloaded: ") + std::to_string(n));
    }

    // Кадр — планировщиком. В правке это анимация и частицы (превью), в Play к
    // ним добавляются скрипты и физика, и порядок между ними задан один раз в
    // RegisterCoreSystems, а не переписан здесь по памяти.
    // Масштаб времени и пауза игры (sage.time.SetScale, sage.game.Pause)
    // действуют и в Play-режиме редактора: иначе замедление, настроенное для
    // игры, работало бы в собранной игре и не работало там, где его настраивают.
    //
    // ПАУЗА ОСТАНАВЛИВАЕТ КАДР ЦЕЛИКОМ, а не помечает флажок. Кнопка «Пауза»
    // до сих пор МЕНЯЛА ТОЛЬКО СОСТОЯНИЕ: планировщик звался каждый кадр
    // независимо от него, и в паузе продолжали идти скрипты, физика, анимация
    // и частицы — то есть пауза не делала ничего, кроме смены значка. Заметить
    // причину по коду было трудно: комментарий выше честно обещал «пока не
    // пауза», а строки, которая это обеспечивает, не было.
    //
    // dt = 0 тут не годится: OnUpdate скриптов при нулевом шаге всё равно
    // выполняется, и логика, не считающая время («каждый кадр прибавить
    // единицу»), продолжала бы идти. Пауза — это НЕ считать кадр вовсе.
    if (!m_play.Paused()) {
        const float scale = m_play.Scripts() ? m_play.Scripts()->FrameTimeScale() : 1.0f;
        m_systems.Run(*m_scene, dt * scale);
    } else if (const float step = m_play.TakePendingStep(); step > 0.0f) {
        // Заказанный шаг: ровно один кадр игры и снова стоп.
        m_systems.Run(*m_scene, step);
    }
    m_plugins.UpdateAll(dt);
    sage::editor::luaplugins::Tick(*this, dt);
    TickLuaTests();

    // Чего игра попросила за кадр. Здесь — после того, как все скрипты
    // отработали и ни один не находится на стеке.
    if (!ProcessScriptRequests()) return; // сцена под ногами заменилась или Play остановлен

    // Правка в окне Settings обязана быть видна В КАДРЕ, а не после
    // перезапуска: ползунок, который «сработает потом», невозможно настроить.
    ApplyEngineSettings();

    // Файлы, брошенные в окно с прошлого кадра (см. HandleDroppedFiles).
    HandleDroppedFiles();
}

// ЧЕГО ИГРА ПОПРОСИЛА ЗА КАДР: выйти, перейти на другой уровень, начать заново.
//
// Отдельной функцией, а не строками внутри кадра, по той же причине, по которой
// отдельно живёт StepScripts: это проверяют — и проверять обязаны ТОТ ЖЕ путь,
// которым идёт настоящий кадр, а не его копию в тесте.
//
// false — дальше этот кадр доигрывать нечем: сцена заменена или Play остановлен.
bool EditorLayer::ProcessScriptRequests() {
    if (m_play.Scripts()) {
        if (m_play.Scripts()->TakeQuitRequest()) {
            // В редакторе «выйти из игры» — это остановить Play, а не закрыть
            // редактор: у человека несохранённая сцена, и закрывать её по
            // просьбе скрипта нельзя.
            LOG_INFO("Editor") << "скрипт попросил выйти из игры — останавливаю Play";
            StopPlay();
            return false;
        }
        // ПЕРЕХОД МЕЖДУ УРОВНЯМИ — ПРЯМО ЗДЕСЬ, а не «проверяйте в сборке».
        //
        // Два источника запроса: прежний движок (sage.scene.Load уровневых
        // скриптов) и система скриптинга (scene:Load скриптов объектов).
        // Спрашиваем оба в одном месте — иначе переход работал бы из одного
        // скрипта и молчал из другого.
        //
        // Документ человека при этом не трогается: Play работает с копией, а
        // Stop вернёт открытую сцену вместе с несохранённой правкой.
        std::string sceneName;
        const bool restart = m_play.Scripts()->TakeRestartRequest();
        bool wantScene = m_play.Scripts()->TakeSceneRequest(sceneName);
        if (!wantScene && m_play.Scripting())
            wantScene = m_play.Scripting()->TakeSceneRequest(sceneName);
        if (wantScene || restart) {
            PlayContext ctx = MakePlayContext();
            if (!m_play.SwitchScene(ctx, sceneName)) StopPlay();
            return false; // сцена под ногами заменилась — этот кадр доигрывать нечем
        }
    }
    return true;
}


// ============================================================================
//  Плагины редактора — реализация facade'а EditorPluginContext
// ============================================================================

void EditorLayer::PluginContextImpl::Log(const char* message) {
    LOG_INFO("Plugin") << message;
}

const char* EditorLayer::PluginContextImpl::SelectedEntityName() const {
    GameObject obj = m_owner.m_scene->Get(m_owner.m_selection.Primary());
    m_selectedNameBuf = obj.Valid() ? obj.Name() : "";
    return m_selectedNameBuf.c_str();
}

void EditorLayer::PluginContextImpl::SetStatusMessage(const char* message) {
    m_owner.m_pluginStatusMessage = message ? message : "";
}

// Настройки движка из окна Settings — В САМ ДВИЖОК.
//
// ЧЕГО НЕ ХВАТАЛО. Окно Settings правило m_settings — СВОЮ копию EngineConfig, —
// а весь рендер читает глобальный EngineConfig::Get(). Эти два объекта не были
// связаны ничем, и потому окно настроек в редакторе не делало НИЧЕГО: тени,
// каскады, сглаживание, AO, свечение, пост-обработка оставались на значениях по
// умолчанию, что ни выбирай. Пресет «Ultra» честно записывал 4096 и MSAA 4x —
// и они никуда не уходили. В собранной игре те же настройки работали (рантайм
// читает файл и зовёт EngineConfig::Set), поэтому со стороны это выглядело как
// «в редакторе картинка хуже, чем в игре», а не как потерянная связь.
//
// Переносим по ОТПЕЧАТКУ, а не каждый кадр: EngineConfig — простая структура
// без уведомлений, сравнивать её посимвольно дешевле, чем копировать вслепую,
// и заодно видно, когда настройки правда менялись (пересоздание карт теней и
// буферов сцены цепляется за это же).
void EditorLayer::ApplyEngineSettings() {
    const std::string stamp = m_settings.ToJsonString();
    if (stamp == m_settingsStamp) return;
    m_settingsStamp = stamp;
    sage::EngineConfig::Set(m_settings);
}

// ============================================================================
//  Файлы, брошенные в окно из проводника системы
// ============================================================================
//
// ЧТО ЭТО ЗА ОПЕРАЦИЯ. «Перетащить в редактор» — первое, что человек пробует со
// своей моделью, и до сих пор окно на это не отзывалось никак: файл надо было
// класть в папку проекта мимо редактора, а потом искать в панели. Здесь
// брошенное разбирается по смыслу, а не сваливается в одну кучу.
//
// РАЗБОР ПО СМЫСЛУ. Проект и сцена — не ассеты, их не «вносят», их ОТКРЫВАЮТ:
// бросить .sageproj и получить его копию в assets/ было бы бессмысленно.
// Остальное копируется в текущую папку панели Assets вместе со спутниками
// (см. AssetsPanel::ImportAsset) — модель приезжает со своими .mtl и
// текстурами, а не голым файлом, который потом не грузится.
std::function<void(const std::vector<std::string>&)> EditorLayer::s_dropSink;

// ============================================================================
//  Кадр UI
// ============================================================================

void EditorLayer::OnRender() {
    sage::Application& app = sage::Application::Get();

    // Итоговое освещение кадра: окружение сцены + света-сущности; один
    // shadow-проход на кадр, Viewport и Game сэмплируют общую карту.
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    m_renderer.PrepareReflections(*m_scene, env);      // карта окружения до всех проходов
    m_renderer.RenderShadow(*m_scene, env, m_camera); // общая карта теней (Viewport + Game)
    m_renderer.SetShowBounds(m_tools.ShowBounds);
    m_renderer.SetColliderGizmo(m_tools.ColliderGizmo);
    // Отладочная графика игры (Debug:DrawLine) — только пока игра идёт. В
    // режиме правки скриптов нет, и показывать нечего.
    m_renderer.SetScriptDebugLines(m_play.Scripting() ? &m_play.Scripting()->Debug() : nullptr);
    // Игровой интерфейс во ВЬЮПОРТЕ больше не рисуется: холст вёрстки — это
    // окно «Интерфейс», где показан игровой кадр в разрешении игры. Вьюпорт
    // остался вьюпортом, а не наполовину холстом.

    // ГЛАВНЫЙ слот тоже уважает свой вид. Раньше он рисовался безусловно
    // перспективой, а переопределение применялось только к слотам 1..3 — из-за
    // чего выпадающий список вида в одиночной раскладке (Top/Front/Side) не
    // делал НИЧЕГО: человек выбирал «вид сбоку», а вьюпорт оставался
    // перспективным. Ортогональные виды работали только там, где их и так
    // видно рядом с перспективой, то есть в раскладке на два и четыре окна.
    EditorViewOverride primaryOv;
    {
        const ViewRequest& r0 = m_viewRequests[0];
        if (r0.Active && r0.Ortho) {
            primaryOv.Use = true;
            primaryOv.View = r0.View;
            primaryOv.Proj = r0.Proj;
            primaryOv.EyePos = r0.EyePos;
        }
    }
    m_renderer.RenderViewport(*m_scene, m_camera, env, m_selection.Primary(), m_selection.All(),
                              m_tools.RenderMode, m_tools.ShowGrid,
                              cfg, m_view, m_proj, 0, primaryOv);

    // Дополнительные виды раскладки (сверху/спереди/сбоку). Каждый — полный
    // проход сцены, поэтому рисуются ТОЛЬКО те, что панель попросила: одиночный
    // вьюпорт, самый частый случай, не платит за раскладку, которой не
    // пользуются.
    // Съёмка обложек шаблонов идёт ДО кадра: она меняет проект и сцену, и делать
    // это посреди сбора кадра значило бы рисовать половину одной сцены и
    // половину другой.
    TickTemplateShots();

    for (int i = 1; i < m_viewCount; ++i) {
        const ViewRequest& r = m_viewRequests[i];
        if (!r.Active || r.W < 8 || r.H < 8) continue;
        EditorViewOverride ov;
        ov.Use = r.Ortho;
        ov.View = r.View;
        ov.Proj = r.Proj;
        ov.EyePos = r.EyePos;
        glm::mat4 v, p;
        m_renderer.RenderViewport(*m_scene, m_camera, env, m_selection.Primary(), m_selection.All(),
                                  m_tools.RenderMode, m_tools.ShowGrid, cfg, v, p, i, ov);
    } // отдаёт view/proj для гизмо/пикинга
    // ПОДЛОЖКА — ТОЛЬКО В РЕДАКТОРЕ ИНТЕРФЕЙСА. Это его инструмент показа, а не
    // свойство игры: панель Game обещает «как увидит игрок», и приглушённая
    // сцена в ней означала бы, что редактор врёт о картинке.
    m_renderer.SetUIBackdrop(m_workspace == EditorWorkspace::Interface ? m_tools.UI.Backdrop : 0.0f,
                             m_tools.UI.BackdropColor);
    // И ТОЛЬКО ТОТ ИНТЕРФЕЙС, КОТОРЫЙ ВЕРСТАЮТ — по той же причине: чужое меню
    // поверх редактируемого мешает не меньше подложки. В пространстве сцены
    // область пустая, то есть «всё, как в игре» (см. EditorLayer::UiScope).
    m_renderer.SetUIScope(UiScope());
    m_renderer.RenderGame(*m_scene, env, cfg);      // Primary-камера сцены (если есть)

    // Превью ВЫБРАННОЙ камеры — последним и только по запросу вьюпорта: это
    // ещё один полный проход сцены, и платить за него, когда карточки на
    // экране нет, не за что.
    if (m_cameraPreviewId >= 0) {
        GameObject cam = m_scene->Get(m_cameraPreviewId);
        if (cam.Valid()) m_renderer.RenderCameraPreview(*m_scene, env, cfg, cam.Entity());
        m_cameraPreviewId = -1;
    }

    // Снимок обложки шаблона делается ЗДЕСЬ и только здесь: игровой кадр
    // существует ровно после RenderGame, а до неё это прошлогодняя картинка.
    if (!m_coverShotPath.empty()) {
        if (m_renderer.SaveGameFrame(m_coverShotPath))
            LOG_INFO("Editor") << "Обложка шаблона снята: " << m_coverShotPath;
        else
            LOG_ERROR("Editor") << "Обложка шаблона не снялась: игрового кадра нет";
        m_coverShotPath.clear();
        m_coverShotDone = true;
    }

    // Обложка СЦЕНЫ — здесь же и по той же причине: игровой кадр существует
    // ровно после RenderGame (см. EditorLayer::TakeSceneShot).
    TakeSceneShot();

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    cursorcapture::BeforeImGuiFrame();
    ImGui_ImplGlfw_NewFrame();
    // Проверка мыши В ЖИВОМ КАДРЕ (SAGE_EDITOR_SELFTEST) кладёт свои события
    // ПОСЛЕ бэкенда и ДО NewFrame: бэкенд в своём NewFrame тоже трогает
    // положение курсора, и синтетические события обязаны прийти последними.
    TickInputProbe();
    TickCursorLockProbe();
    PinMouseWhileCaptured();
    ImGui::NewFrame();
    if (!app.GetWindow().CursorCaptured()) m_freeMousePos = ImGui::GetIO().MousePos;
    ImGuizmo::BeginFrame();
    EditorTheme::ApplyGizmoColors();

    // ПОКА ПРОЕКТА НЕТ, РЕДАКТОРА НЕТ — только стартовое окно.
    //
    // Раньше панели рисовались и без проекта, и каждая несла свою ветку «а
    // если проекта нет»: панель ассетов показывала корень диска, слоты
    // сохраняли абсолютные пути, сборка игры отказывала, шаблоны были
    // недоступны. Это и было состояние «без проекта» — не режим, а набор
    // оговорок, размазанный по всему редактору.
    //
    // Проект теперь есть всегда, а «всегда» стоит ровно столько, сколько стоит
    // это условие: до открытия проекта показывать нечего, потому что и работать
    // не с чем.
    if (!m_project.Loaded()) {
        m_launcher.Draw(*this, m_projects);
        m_dialogs.Draw(*this);   // диалог обзора папок открывается отсюда же
        // Долгая работа видна И В СТАРТОВОМ ОКНЕ: шаблон качают именно отсюда,
        // и без полосы окно просто замирает.
        sage::editor::progress::Draw();
        // Сторож: окно, открытое без того, кто его рисует, обездвиживает
        // редактор (см. CloseGhostPopups). В стартовом окне тоже.
        CloseGhostPopups();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Окна системы — и здесь тоже (см. PresentExtraViewports): диалог
        // создания проекта в стартовом окне становится отдельным окном, если
        // не влезает в главное.
        PresentExtraViewports();
        // СНИМОК ДЕЛАЕТСЯ И ЗДЕСЬ. Ранний выход пропускал счётчик кадров вместе
        // со скриншотом, и стартовое окно оказывалось единственной частью
        // редактора, которую нельзя проверить иначе как открыв её глазами на
        // своей машине — при том, что SAGE_SHOW_LAUNCHER заведён ровно для
        // этого. Ровно поэтому список шаблонов и прожил с карточкой, которая не
        // смотрела, установлен ли шаблон: смотреть на неё в прогоне было нечем.
        TakeAutoScreenshot(app);
        return;
    }

    DrawDockspaceAndMenu(); // включая модалки (m_dialogs) и окно настроек (m_settingsPanel)
    // Панель подаётся в кадр только когда она открыта: закрытая вкладка иначе
    // возвращалась бы сама собой на следующем кадре, и крестик не работал бы.
    //
    // Вокруг каждой панели — panelwindows (см. PanelWindows.h): она решает,
    // живёт панель вкладкой в главном окне или ОТДЕЛЬНЫМ окном системы, и
    // сверяет своё состояние с тем, что человек сделал мышью. Обёртка общая и
    // одна на все панели намеренно: панель, которую забыли обернуть, молча
    // теряет право на своё окно, и объяснить это человеку будет нечем.
    // Какие панели в каком пространстве — решает реестр (PanelSpec::Spaces):
    // сцену расставляют и интерфейс верстают разными панелями, а общие
    // (консоль, ассеты) рисуются в обоих своим окном каждая.
    m_panelRegistry.DrawPanels(*this, m_workspace);
    m_plugins.ImGuiAll();
    sage::editor::pluginswindow::Draw(*this, {&m_plugins, &m_pluginCtx, m_nativePluginsDir});

    // Полосы долгой работы — ПОВЕРХ панелей и до стартового окна: карточка в
    // углу и модальное окно обязаны лежать выше всего, что рисует редактор
    // (см. Progress.h).
    sage::editor::progress::Draw();

    // Стартовое окно по просьбе (Window > Стартовое окно): проект уже открыт,
    // но человек хочет открыть другой.
    if (m_launcherRequested) {
        const std::string was = m_project.Dir().string();
        // Draw() отвечает «показывать ли дальше»: у окна поверх открытого
        // проекта есть выход назад, и закрыть его надо по этому ответу.
        if (!m_launcher.Draw(*this, m_projects)) m_launcherRequested = false;
        if (m_project.Dir().string() != was) m_launcherRequested = false;
    }

    // Пипетка — поверх всего: её ловушка щелчка обязана лежать выше панелей.
    Sage::UI::eyedropper::Frame();
    CloseGhostPopups();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    // ПИКСЕЛЬ ПОД КУРСОРОМ — ИЗ ОТРИСОВАННОГО КАДРА, до показа: так пипетка
    // над своим окном видит ровно то, что на экране (3D-вьюпорт, панели), и не
    // зависит от того, умеет ли система отдавать чужие окна (Wayland не умеет).
    if (int fx = 0, fy = 0; Sage::UI::eyedropper::FramebufferPoint(&fx, &fy)) {
        const ImGuiIO& fio = ImGui::GetIO();
        const int fbW = (int)(fio.DisplaySize.x * fio.DisplayFramebufferScale.x);
        const int fbH = (int)(fio.DisplaySize.y * fio.DisplayFramebufferScale.y);
        if (fx >= 0 && fy >= 0 && fx < fbW && fy < fbH) {
            unsigned char px[3] = {0, 0, 0};
            sage::rhi::GraphicsDevice::Get().ReadPixelsRGB(fx, fbH - 1 - fy, 1, 1, px);
            const float c[3] = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
            Sage::UI::eyedropper::SetFramebufferColor(c);
        }
    }

    PresentExtraViewports();

    // Многооконность проверяется здесь, а не в RunSelfTest: там нет кадра, а
    // окно системы заводит платформа — после UpdatePlatformWindows.
    CheckMultiWindowFrame();
    CheckWorkspaceDockFrame();
    CheckPreviewResolutionFrame();
    CheckUiInspectorFrame();
    CheckEnvironmentFrame();
    CheckTweenFrame();
    CheckCreateDialogFrame();
    CheckLuaTestsFrame();

    TakeAutoScreenshot(app);
}

// ---------------------------------------------------------------------------
// Окна, живущие ОТДЕЛЬНЫМИ окнами системы (multi-viewport): панели, вытащенные
// из дока, и всплывающие окна, не поместившиеся в главное.
//
// ВЫЗЫВАТЬ КАЖДЫЙ КАДР, В КОТОРОМ БЫЛ ImGui::Render(). Пропущенный вызов — не
// «окно не показалось»: ImGui заводит для такого окна вьюпорт и ждёт, что
// платформа создаст ему настоящее окно, а следующий кадр бэкенда спрашивает у
// несозданного окна фокус и падает в glfwGetWindowAttrib. Ровно так редактор
// и падал, стоило открыть в стартовом окне диалог, который не влезает в
// главное окно: ветка стартового окна рисовала кадр и выходила раньше этого
// блока.
// ============================================================================
//  СТОРОЖ: ОТКРЫТО ОКНО, КОТОРОЕ НИКТО НЕ РИСУЕТ
//
//  ImGui::OpenPopup только помечает окно открытым; показывает его тот, кто
//  позовёт BeginPopup* С ТЕМ ЖЕ ИМЕНЕМ. Разойдутся имена — и окно остаётся в
//  стеке навсегда: нарисовать его некому, а значит и закрыть некому.
//
//  Само по себе это «диалог не показался». Беда в том, что тот, кто его
//  открыл, обычно просит КАЖДЫЙ КАДР (пока не сбросится флаг, а сбросить его
//  может только сам диалог), а каждый OpenPopup закрывает всё, что человек
//  открыл выше по стеку (ImGui::OpenPopupEx -> ClosePopupToLevel). Меню «Файл»
//  захлопывается в том же кадре, в котором открылось; ни одно меню и ни один
//  диалог больше не открываются. Снаружи это ровно «редактор завис, кнопки не
//  нажимаются» — при живой картинке, крутящемся вьюпорте и пустом логе.
//
//  Так и случилось: у окна восстановления сцены OpenPopup звался с ПЕРЕВЕДЁННЫМ
//  именем без «###», и по-русски редактор после аварийного завершения переставал
//  слушаться вовсе (см. DrawRecoveryPrompt).
//
//  Поэтому здесь не проверка, а ЛЕЧЕНИЕ: такое окно закрывается, и в лог уходит
//  строка с причиной. Редактор обязан оставаться управляемым даже когда в нём
//  ошибка — иначе человеку нечем даже сохранить работу. Имена сторожит
//  scripts/check_popup_ids.py; это — последний рубеж на случай имени, собранного
//  в рантайме.
// ============================================================================
void EditorLayer::CloseGhostPopups() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    for (int i = g->OpenPopupStack.Size - 1; i >= 0; --i) {
        const ImGuiPopupData& popup = g->OpenPopupStack[i];
        if (popup.Window != nullptr) continue;   // рисуется — всё в порядке
        // Между OpenPopup и первым BeginPopup* законно проходит кадр: ждём
        // несколько, чтобы не закрыть окно, которое вот-вот нарисуют.
        if (g->FrameCount - popup.OpenFrameCount < 3) continue;
        LOG_ERROR("Editor") << "Всплывающее окно 0x" << std::hex << (unsigned)popup.PopupId
                            << std::dec << " открыто, но его никто не рисует — закрываю. "
                            << "Имена у OpenPopup и BeginPopup* разошлись "
                            << "(см. scripts/check_popup_ids.py)";
        ImGui::ClosePopupToLevel(i, false);
        return;   // по одному за кадр: стек после закрытия перестроен
    }
}

// ПОКА КУРСОР ЗАХВАЧЕН, МЫШЬ ИНТЕРФЕЙСА СТОИТ НА МЕСТЕ.
//
// В захвате GLFW отдаёт «виртуальное» положение курсора, которое уезжает
// сколько угодно далеко — за край панели, за край окна. Интерфейс принимал
// его за настоящее: при облёте камеры правой кнопкой «мышь» уходила с
// вьюпорта на соседнюю панель, и облёт срывался; в игре от первого лица
// щелчок (выстрел) приходился по Inspector или пустому месту, панель Game
// теряла фокус, и захват отпускался. Снаружи это выглядело как «курсор
// спрятан, но гуляет и не держится».
//
// Поэтому на время захвата положение мыши интерфейса держится там, где захват
// начался: наведение и фокус остаются у той панели, которая курсор захватила.
// Само смещение мыши игра и камера берут не отсюда, а из событий окна.
void EditorLayer::PinMouseWhileCaptured() {
    const bool captured = sage::Application::Get().GetWindow().CursorCaptured();
    if (!captured) {
        m_mousePinned = false;
        return;
    }
    if (!m_mousePinned) {
        m_mousePin = m_freeMousePos;
        m_mousePinned = true;
    }
    ImGui::GetIO().AddMousePosEvent(m_mousePin.x, m_mousePin.y);
}

void EditorLayer::PresentExtraViewports() {
    // Multi-viewport: панели, вытащенные за пределы главного окна, живут в
    // собственных OS-окнах — их нужно обновить и отрисовать отдельно.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();

        // Приём файлов из проводника — на КАЖДОЕ окно, а не только на главное.
        //
        // Панель, вытащенную из дока, ImGui показывает в отдельном OS-окне, и
        // GLFW-колбэк перетаскивания на него никто не вешал: ни бэкенд ImGui
        // (он ставит фокус, курсор, кнопки и клавиши — но не drop), ни мы. Файл,
        // брошенный на плавающую панель Assets, просто пропадал — «ничего не
        // происходит» в самом чистом виде, потому что событие не доходило даже
        // до редактора.
        for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports) {
            if (!vp->PlatformHandle) continue;
            GLFWwindow* w = (GLFWwindow*)vp->PlatformHandle;
            if (m_dropWindows.insert(w).second) {
                // Своя копия приёма файлов поверх бэкенда ImGui: тот drop не
                // ставит, так что цепочку здесь продолжать не с чем.
                glfwSetDropCallback(w, [](GLFWwindow*, int count, const char** paths) {
                    if (!s_dropSink || count <= 0 || !paths) return;
                    std::vector<std::string> list;
                    list.reserve((size_t)count);
                    for (int i = 0; i < count; ++i)
                        if (paths[i]) list.emplace_back(paths[i]);
                    if (!list.empty()) s_dropSink(list);
                });
            }
        }

        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
}

void EditorLayer::TakeAutoScreenshot(sage::Application& app) {
    ++m_frameCounter;
    if (m_autoScreenshotFrame < 0 || m_frameCounter < m_autoScreenshotFrame) return;
    // ПРОВЕРКА МЫШИ ДОЛЖНА УСПЕТЬ. Она идёт кадрами (см. TickInputProbe), а
    // smoke-тест просит снимок на десятом кадре и на нём же закрывает редактор —
    // то есть выход случался раньше первого щелчка, и проверка не давала
    // вердикта вообще. Поэтому снимок и выход ждут её конца, а не номера кадра.
    if (m_probeStep >= 0) return;
    // То же с проверками, что идут после мыши (инспектор UI, окружение,
    // Lua-тесты в Play): выход на кадре мыши обрывал их, и CI не видел их
    // вердикта вовсе — провал Lua-теста проходил сборку молча. Ждём конца
    // последней в цепочке, но не вечно: зависшая проверка не должна вешать CI,
    // её отсутствие в логе поймает сам smoke-тест.
    if (std::getenv("SAGE_EDITOR_SELFTEST") && (m_luaSelfTestStep >= 0 || m_cursorProbeStep >= 0)) {
        using Clock = std::chrono::steady_clock;
        if (!m_shotWaitStarted) { m_shotWaitStart = Clock::now(); m_shotWaitStarted = true; }
        if (Clock::now() - m_shotWaitStart < std::chrono::minutes(5)) return;
        LOG_ERROR("Editor") << "SELFTEST: поздние проверки не закончились за 5 минут — выходим без них";
    }
    Window& win = app.GetWindow();
    SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
    app.Close();
}
