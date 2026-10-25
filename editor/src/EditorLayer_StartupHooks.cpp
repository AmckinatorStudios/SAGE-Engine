#include "sage/core/Problems.h"
#include "sage/app/CoreSystems.h"
#include "EditorLayer.h"
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

// ХУКИ ЗАПУСКА ДЛЯ ПРОГОНОВ БЕЗ ЧЕЛОВЕКА (SAGE_EDITOR_*): самопроверка, E2E,
// скриншоты окон, демонстрационные состояния. Отдельно от обычного запуска:
// человек, открывший редактор, ни одной из этих веток не проходит, и читать
// запуск редактора сквозь четыре сотни строк тестовых хуков не нужно.
// Все SAGE_EDITOR_* переменные запуска.
void EditorLayer::ApplyStartupHooks() {
    // ПОКАЗАТЬ ПОЛОСЫ ДОЛГОЙ РАБОТЫ БЕЗ САМОЙ РАБОТЫ. Карточка в углу и
    // модальное окно появляются только во время бейка, скачивания или перевода
    // папки — то есть посмотреть на них глазами в прогоне было нечем, а
    // проверять оформление по коду нельзя. С этой переменной обе полосы стоят
    // в кадре сразу и попадают на авто-снимок.
    if (std::getenv("SAGE_EDITOR_PROGRESS_DEMO")) {
        namespace progress = sage::editor::progress;
        const progress::Id bake = progress::Begin(progress::Kind::Background,
                                                  T("Baking GI"), "16 / 64");
        progress::Update(bake, 0.25f, "16 / 64");
        progress::Begin(progress::Kind::Background, T("Preparing asset thumbnails"),
                        T("Downloading and unpacking"));
        const progress::Id conv = progress::Begin(progress::Kind::Blocking,
                                                  T("Converting the folder"), "tower.fbx (37/120)");
        progress::Update(conv, 0.31f, "tower.fbx (37/120)");
    }

    if (std::getenv("SAGE_EDITOR_SELFTEST")) {
        RunSelfTest();
        // Мышь проверяется уже в кадрах, после самопроверки: до главного цикла
        // кадра нет, а щёлкать надо по нарисованному вьюпорту.
        m_probeStep = 0;
    }
    if (std::getenv("SAGE_EDITOR_E2E")) RunE2EGameTest();
    if (std::getenv("SAGE_EDITOR_OPEN_PROJECT")) RunHeadlessProjectSession();

    // ПРОЕКТ — ДО ОСТАЛЬНЫХ ХУКОВ, а не после.
    //
    // Работы без проекта больше нет (см. LauncherPanel.h), поэтому прогону без
    // человека проект создаётся сам. Сделать это в конце нельзя: создание
    // проекта загружает его сцену, а хуки ниже выбирают в сцене сущность и
    // ассет — выбор просто затирался бы новой сценой, и скриншот выходил бы
    // «ничего не выделено».
    {
        static const char* const kHeadless[] = {
            "SAGE_SCREENSHOT_AT_FRAME", "SAGE_EDITOR_SHOW_SETTINGS", "SAGE_EDITOR_SHOW_PROFILER",
            "SAGE_EDITOR_ICON_SHEET",   "SAGE_EDITOR_OPEN_DIALOG",   "SAGE_EDITOR_TEMPLATE",
            "SAGE_EDITOR_SHOW_ANIMATION", "SAGE_EDITOR_SHOW_INPUT", "SAGE_EDITOR_INPUT_DEMO",
            "SAGE_EDITOR_UI_EDITOR",    "SAGE_EDITOR_UI_PREVIEW",    "SAGE_EDITOR_COLLIDER_MODE",
            "SAGE_EDITOR_SELECT_ENTITY",
            "SAGE_EDITOR_SELECT_ASSET", "SAGE_EDITOR_SHOW_ABOUT",
            "SAGE_EDITOR_AUTOPLAY",       "SAGE_EDITOR_VARS_DEMO",
            "SAGE_EDITOR_TEMPLATE_SHOTS", "SAGE_EDITOR_SHOW_TWEEN",
            "SAGE_EDITOR_PALETTE",          "SAGE_EDITOR_LOAD_MODELS",
            "SAGE_EDITOR_SHOW_CONSOLE",  "SAGE_EDITOR_SHOW_PROBLEMS",
            "SAGE_EDITOR_SHOW_PLUGINS",
        };
        for (const char* name : kHeadless) {
            if (std::getenv(name)) { m_headlessProject = true; break; }
        }
        // Стартовое окно снимают НАМЕРЕННО (SAGE_SHOW_LAUNCHER) — тогда проект
        // создавать не надо, иначе снимать будет нечего.
        if (std::getenv("SAGE_SHOW_LAUNCHER")) m_headlessProject = false;
        if (m_headlessProject && !m_project.Loaded()) {
            std::string err;
            if (!CreateProject(".", "sage_headless_project", DefaultProjectTemplate(), err))
                LOG_ERROR("Editor") << "Не удалось создать проект для headless-прогона: " << err;
        }
    }

    // Окно «Плагины» с примером плагина в проекте — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_PLUGINS")) {
        std::string err;
        namespace lp = sage::editor::luaplugins;
        lp::Create(lp::ProjectDir(*this), "Расстановка деревьев", "SAGE", lp::Template::Both, err);
        lp::Rescan(*this);
        sage::editor::pluginswindow::Open();
        sage::editor::pluginswindow::Select("rasstanovka_derevev");
    }
    // Открыть окно Settings при старте (для скриншот-проверки/демо настроек).
    if (std::getenv("SAGE_EDITOR_SHOW_SETTINGS")) { m_headlessProject = true; m_panels[EditorPanel::Settings] = true; }
    if (std::getenv("SAGE_EDITOR_SHOW_PROFILER")) { m_headlessProject = true; m_panels[EditorPanel::Profiler] = true; }
    // Консоль впереди вкладки Assets — для скриншот-проверки её панели.
    if (std::getenv("SAGE_EDITOR_SHOW_CONSOLE")) m_console.RequestFocus();
    if (std::getenv("SAGE_EDITOR_SHOW_PROBLEMS")) m_console.ShowProblems();
    // Инструмент анимации при старте — для скриншот-проверок: линейка времени и
    // кривая проверяются глазами, и открыть панель в прогоне больше нечем.
    if (std::getenv("SAGE_EDITOR_SHOW_ANIMATION")) {
        m_headlessProject = true;
        m_panels[EditorPanel::Animation] = true;
        m_animation.RequestFocus(60);
    }
    if (std::getenv("SAGE_EDITOR_ICON_SHEET")) { m_headlessProject = true; m_showIconSheet = true; }
    // Окно управления при старте — для скриншот-проверок: раскладку смотрят
    // глазами, и открыть панель в прогоне больше нечем.
    if (std::getenv("SAGE_EDITOR_SHOW_INPUT")) {
        m_headlessProject = true;
        m_panels[EditorPanel::Input] = true;
        // Раскладка для снимка: пустое окно управления показывает только
        // подсказку «действий пока нет», а проверять надо СПИСОК.
        if (std::getenv("SAGE_EDITOR_INPUT_DEMO")) {
            sage::input::InputSystem& in = m_projectInput;
            sage::input::Action& move =
                in.Register("Move", sage::input::ActionType::Vector);
            move.BindVector("W", "S", "A", "D");
            sage::input::Action& look =
                in.Register("Look", sage::input::ActionType::Vector);
            look.Bind(sage::input::Binding::MouseAxisX());
            in.Register("Jump", sage::input::ActionType::Digital).Bind("SPACE");
            in.Register("Attack", sage::input::ActionType::Digital).Bind("MOUSE_LEFT");
            in.Register("Interact", sage::input::ActionType::Digital).Bind("E");
            sage::input::Context& ui = in.CreateContext("Inventory", 20);
            ui.Add("Close", sage::input::ActionType::Digital).Bind("ESCAPE");
            ui.Add("Take", sage::input::ActionType::Digital).Bind("E");
        }
    }
    // Редактор интерфейса открывается в прогоне самопроверки нарочно: он живёт
    // ОТДЕЛЬНЫМ окном системы (см. PanelWindows.h), а закрытая панель окна не
    // заводит — проверять в кадре было бы нечего. На снимок главного окна это
    // не влияет: окно у него своё.
    if (std::getenv("SAGE_EDITOR_SELFTEST")) m_panels[EditorPanel::InterfaceViewport] = true;
    if (const char* dlg = std::getenv("SAGE_EDITOR_OPEN_DIALOG")) {
        m_headlessProject = true;
        m_pendingDialog = dlg;   // откроется в кадре, на уровне окна-хоста
    }
    // Открыть редактор СРАЗУ на нужном шаблоне — чтобы на шаблон можно было
    // ПОСМОТРЕТЬ, а не поверить описанию. Проверять шаблоны числом сущностей
    // (это делает самопроверка) недостаточно: «пусто» и «пусто, но остался
    // скайбокс» дают одинаковый ноль, а выглядят по-разному.
    // ОБЛОЖКИ ШАБЛОНОВ — НАСТОЯЩИЕ СНИМКИ, а не рисунки.
    //
    // Обложка рисовалась кодом (десяток примитивов в ImDrawList) и обещала то,
    // чего в шаблоне может уже не быть: рисунок правят руками отдельно от
    // самого шаблона, и разъезжаются они молча. Снимок так не умеет — он
    // делается ИЗ ТОГО ЖЕ шаблона, который создаётся кнопкой.
    //
    // Пересоздаются одной командой:
    //   SAGE_EDITOR_TEMPLATE_SHOTS=<папка> SageEditor
    // и кладутся в editor/assets/templates/<id>.png. Хук, а не шаг сборки:
    // снимать кадр нужно с живым графическим контекстом и настоящей сценой, то
    // есть самим редактором, и делать это на каждой сборке — платить минутой
    // за картинку, которая меняется раз в полгода.
    if (const std::string outDir = sage::EnvString("SAGE_EDITOR_TEMPLATE_SHOTS");
        !outDir.empty()) {
        m_headlessProject = true;
        m_coverShotDir = outDir;
        std::error_code shotEc;
        std::filesystem::create_directories(m_coverShotDir, shotEc);
        LOG_INFO("Editor") << "Съёмка обложек шаблонов в " << m_coverShotDir;
    }

    if (const char* tplId = std::getenv("SAGE_EDITOR_TEMPLATE");
        tplId && m_coverShotDir.empty()) {
        m_headlessProject = true;
        // СОЗДАЁМ ПРОЕКТ, а не «пересобираем сцену по виду шаблона».
        //
        // Раньше здесь звался NewScene(tpl->Kind), и для готового проекта
        // (Kind::Copy) это не значило ничего: NewScene умеет строить сцену
        // кодом, а витрина — файлы на диске. Хук молча отдавал ПУСТУЮ сцену,
        // то есть проверял глазами не тот шаблон, который просили, — и именно
        // на этом «шаблон showcase не работает» и выглядело правдой.
        std::string tplErr;
        const std::string dir = std::string("sage_template_") + tplId;
        std::error_code rmEc;
        std::filesystem::remove_all(dir, rmEc);
        if (!CreateProject(".", dir, tplId, tplErr))
            LOG_ERROR("Editor") << "SAGE_EDITOR_TEMPLATE: " << tplErr;
        else
            LOG_INFO("Editor") << "SAGE_EDITOR_TEMPLATE: создан проект по шаблону '" << tplId
                               << "', сущностей " << m_scene->Count();
    }
    // Выбрать сущность по имени — для скриншот-проверок того, что рисуется
    // ТОЛЬКО при выделении: гизмо, аутлайн, габариты. Без этого проверить их
    // headless нечем: кликать во вьюпорте в CI некому.
    // Открыть редактор интерфейса при старте — для скриншот-проверок.
    if (std::getenv("SAGE_EDITOR_UI_EDITOR")) {
        m_headlessProject = true;
        SetWorkspace(EditorWorkspace::Interface);
    }
    if (const char* b = std::getenv("SAGE_EDITOR_UI_BACKDROP"))
        m_tools.UI.Backdrop = (float)std::atof(b);
    // Предпросмотр вкладкой лежит ЗА холстом, и снять его headless иначе
    // нечем: щёлкнуть по вкладке в прогоне некому, а проверять тут надо
    // именно картинку — «кадр не по центру» в состоянии не видно.
    if (std::getenv("SAGE_EDITOR_UI_PREVIEW")) {
        m_headlessProject = true;
        SetWorkspace(EditorWorkspace::Interface);
        m_uiPreview.RequestFocus();
    }
    if (std::getenv("SAGE_EDITOR_COLLIDER_MODE")) { m_headlessProject = true; m_tools.ColliderEdit = true; }
    // Окно Tween с готовым твином — для скриншот-проверки: карточки, подписи
    // «Начало/Конец» и шкалу проверяют глазами, а пустое окно показывает
    // только кнопку.
    if (std::getenv("SAGE_EDITOR_SHOW_TWEEN")) {
        m_headlessProject = true;
        GameObject cube = m_scene->CreateObject("TweenDemo");
        m_selection.SetPrimary(cube.Id());
        m_panels[EditorPanel::Tween] = true;
        OpenTween(cube.Id(), 0, TweenOpen::Edit);
        m_tween.RequestFocus(60);
        m_tween.AddProperty(*this, "object.position");
        m_tween.AddProperty(*this, "object.scale");
        m_tween.AddProperty(*this, "object.rotation");
        if (auto* tc = m_scene->Registry().try_get<sage::anim::TweenComponent>(cube.Entity()))
            if (!tc->Tweens.empty() && tc->Tweens[0].Tracks.size() == 3) {
                tc->Tweens[0].Tracks[2].To = tc->Tweens[0].Tracks[2].From;   // показать предупреждение «ничего не изменится»
                tc->Tweens[0].ArrangeSequence();
            }
    }
    if (const char* name = std::getenv("SAGE_EDITOR_SELECT_ENTITY")) {
        m_headlessProject = true;
        GameObject obj = m_scene->FindByName(name);
        if (obj.Valid()) m_selection.SetPrimary(obj.Id());
        else LOG_WARN("Editor") << "SAGE_EDITOR_SELECT_ENTITY: нет сущности с именем " << name;
        // Снимают ИНСПЕКТОР выбранного: вкладкой поверх у него по умолчанию
        // стоит Environment, и скриншот показывал бы не то.
        m_focusInspectorFrames = 3;
    }
    if (const char* a = std::getenv("SAGE_EDITOR_SELECT_ASSET")) {
        m_headlessProject = true;
        m_assets.Select(a);
        m_focusInspectorFrames = 3;
    }
    // Закрыть все панели — состояние, в которое человек попадал крестиками и из
    // которого раньше не было выхода. Проверять его иначе нечем: кликать по
    // крестикам в CI некому, а именно на этом кадре должна быть видна подсказка
    // «Все панели закрыты» с кнопкой возврата.
    if (std::getenv("SAGE_EDITOR_CLOSE_PANELS")) {
        m_headlessProject = true;
        m_panels[EditorPanel::Hierarchy] = m_panels[EditorPanel::Inspector] = m_panels[EditorPanel::Environment] = false;
        m_panels[EditorPanel::InterfaceViewport] = m_panels[EditorPanel::InterfaceHierarchy] = false;
        m_panels[EditorPanel::InterfaceInspector] = m_panels[EditorPanel::InterfacePreview] = false;
        m_panels[EditorPanel::Viewport] = m_panels[EditorPanel::Game] = m_panels[EditorPanel::Console] = m_panels[EditorPanel::Assets] = false;
        m_panels[EditorPanel::Profiler] = false;
    }
    // Сущность СО ВСЕМИ компонентами сразу — для жёсткой проверки инспектора.
    //
    // Каждая секция инспектора рисуется только когда нужный компонент есть, а
    // компоненты в демо-сцене раскиданы по разным объектам: проверка «открыли
    // редактор, ничего не сломалось» задевала от силы половину секций. Ошибки
    // вроде двух элементов с одинаковым ID, незакрытого Begin/End или падения
    // на пустом указателе живут именно в редко открываемых секциях. Здесь все
    // они подаются в одном кадре.
    if (std::getenv("SAGE_EDITOR_ALL_COMPONENTS")) {
        m_headlessProject = true;
        GameObject all = m_scene->CreateObject("All Components");
        entt::registry& reg = m_scene->Registry();
        const entt::entity e = all.Entity();
        reg.emplace_or_replace<MeshRendererComponent>(e);
        reg.emplace_or_replace<DecalComponent>(e);
        reg.emplace_or_replace<GIStaticComponent>(e);
        reg.emplace_or_replace<CameraComponent>(e);
        reg.emplace_or_replace<LightComponent>(e);
        reg.emplace_or_replace<RigidBodyComponent>(e);
        reg.emplace_or_replace<ColliderComponent>(e);
        reg.emplace_or_replace<JointComponent>(e);
        reg.emplace_or_replace<CharacterControllerComponent>(e);
        reg.emplace_or_replace<AnimationComponent>(e);
        reg.emplace_or_replace<IKComponent>(e);
        reg.emplace_or_replace<ReflectionProbeComponent>(e);
        reg.emplace_or_replace<ScriptComponent>(e, ScriptComponent{"assets/scripts/spin.lua"});
        sage::ui::ApplyPreset(reg, e, "Button");
        reg.emplace_or_replace<ParticleEmitterComponent>(e);
        m_selection.SetPrimary(all.Id());
        LOG_INFO("Editor") << "SAGE_EDITOR_ALL_COMPONENTS: сущность со всеми компонентами создана";
    }
    // Показать в инспекторе ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ и СВЯЗИ СОБЫТИЙ на живом
    // примере: дверь со скриптом, объявляющим переменные, ключ, на который она
    // ссылается, и кнопка, которая её открывает.
    //
    // Зачем хук, а не «сделайте руками»: проверить эти две секции глазами иначе
    // нечем — они появляются только у объекта, у которого уже есть скрипт с
    // объявлением и настроенная связь, а собрать такой объект в headless-прогоне
    // некому. Секция, которая молча не показывается (объявление не дошло,
    // список связей не нарисовался), не роняет ни один тест.
    if (std::getenv("SAGE_EDITOR_VARS_DEMO")) {
        m_headlessProject = true;
        const std::filesystem::path scriptPath = m_project.AssetsDir() / "door.lua";
        {
            std::ofstream f(scriptPath);
            f << "-- Дверь: что у неё настраивается снаружи\n"
              << "Vars = {\n"
              << "    speed  = { 2.5, min = 0.5, max = 10, label = \"Скорость\" },\n"
              << "    locked = true,\n"
              << "    title  = \"Ворота замка\",\n"
              << "    needs  = { kind = \"entity\", label = \"Нужен ключ\" },\n"
              << "    sound  = { kind = \"asset\", label = \"Звук\" },\n"
              << "}\n\n"
              << "-- Зовётся связью кнопки «On Click → Дверь.Open()»\n"
              << "function Open(entity, event)\n"
              << "    entity:Vars().locked = false\n"
              << "end\n";
        }
        GameObject key = m_scene->CreateObject("Ключ");
        GameObject door = m_scene->CreateObject("Дверь");
        entt::registry& reg = m_scene->Registry();
        reg.emplace_or_replace<ScriptComponent>(door.Entity(),
                                                ScriptComponent{m_project.AssetRef(scriptPath)});
        MergeScriptVars(door);
        // Переменные, объявленные скриптом, лежат в его компоненте — рядом с
        // файлом, который их объявил (см. ScriptComponent.h).
        if (ScriptComponent* sc = reg.try_get<ScriptComponent>(door.Entity()))
            sc->Fields.Set("needs", sage::vars::Value(sage::vars::EntityRef{key.Id()}));

        GameObject button = m_scene->CreateObject("Кнопка «Открыть»");
        sage::ui::Element t;
        t.Anchor = UIAnchor::Center;
        t.Position = {0.0f, 0.0f};
        t.Size = {220.0f, 56.0f};
        reg.emplace_or_replace<sage::ui::Element>(button.Entity(), t);
        reg.emplace_or_replace<sage::ui::Fill>(button.Entity());
        reg.emplace_or_replace<sage::ui::Interactable>(button.Entity());
        // «On Click → Дверь.Open()» — связь сигнала, как её заводят в инспекторе.
        sage::signals::Link open;
        open.Signal = sage::signals::kClicked;
        open.Target = sage::vars::EntityRef{door.Id()};
        open.Method = "Open";
        reg.emplace_or_replace<sage::signals::SignalLinksComponent>(button.Entity()).Links.push_back(open);

        const char* mode = std::getenv("SAGE_EDITOR_VARS_DEMO");

        // SAGE_EDITOR_VARS_DEMO=prefab — тот же набор, но пропущенный через
        // ЗАГОТОВКУ: дверь с кнопкой сохраняется префабом и ставится в сцену
        // второй раз. Смотреть надо на ссылку копии: она обязана вести в СВОЮ
        // дверь, а не в первую. Ошибка здесь молчит — в инспекторе ссылка
        // выглядит заполненной и даже указывает на существующий объект.
        if (mode && std::string(mode) == "prefab") {
            m_scene->SetParent(button.Entity(), door.Entity());
            const std::filesystem::path pf = m_project.AssetsDir() / "door.sageprefab";
            std::string perr;
            if (!sage::scene::SavePrefab(*m_scene, door.Entity(), pf.string(), perr)) {
                LOG_ERROR("Editor") << "SAGE_EDITOR_VARS_DEMO=prefab: префаб не сохранился: "
                                    << perr;
            } else {
                sage::scene::ClearPrefabCache();
                const int copyId = sage::scene::InstantiatePrefab(*m_scene, pf.string());
                GameObject copy = m_scene->Get(copyId);
                if (copy.Valid()) {
                    copy.SetName("Дверь (копия)");
                    // Кнопку копии переименовываем тоже: в дереве две «Кнопки»
                    // рядом неразличимы, а смотреть надо именно на копию.
                    if (const HierarchyComponent* h =
                            m_scene->Registry().try_get<HierarchyComponent>(copy.Entity())) {
                        for (entt::entity k : h->Children)
                            GameObject(&m_scene->Registry(), k).SetName("Кнопка (копия)");
                    }
                }
            }
        }

        // SAGE_EDITOR_VARS_DEMO=play — не просто показать настройку, а ПРОВЕРИТЬ
        // её глазами: запустить игру, щёлкнуть по кнопке и посмотреть в
        // инспекторе, что переменная двери изменилась.
        //
        // Это ровно то, что нельзя увидеть на статичном скриншоте: связь может
        // быть красиво нарисована в инспекторе и при этом никуда не доходить —
        // событие не послано, адресат не найден, скрипт не получил метод.
        // Здесь щёлкает не человек, но щёлкает НАСТОЯЩИЙ путь ввода интерфейса,
        // тот же, что и от мыши.
        if (mode && std::string(mode) == "play") {
            StartPlay();
            if (m_play.Scripts()) m_play.StepScripts(*m_scene, 0.016f);
            sage::ui::UIInputState down;
            down.Mouse = {640.0f, 360.0f};   // центр экрана — там стоит кнопка
            down.MouseDown = true;
            down.MousePressed = true;
            sage::ui::UpdateSceneUI(*m_scene, down, 1280, 720);
            sage::ui::UIInputState up;
            up.Mouse = down.Mouse;
            up.MouseReleased = true;
            sage::ui::UpdateSceneUI(*m_scene, up, 1280, 720);
            if (m_play.Scripts()) m_play.StepScripts(*m_scene, 0.016f);
        }

        // Выбор: дверь по умолчанию, но SAGE_EDITOR_SELECT_ENTITY сильнее — он
        // отрабатывает ВЫШЕ, когда этих объектов ещё нет, и без повтора здесь
        // посмотреть на кнопку было бы нельзя. Ищем ПО ИМЕНИ ещё раз: Play
        // пересоздаёт сцену из снимка, и прежние дескрипторы уже чужие.
        GameObject wanted;
        if (const char* pick = std::getenv("SAGE_EDITOR_SELECT_ENTITY"))
            wanted = m_scene->FindByName(pick);
        if (!wanted.Valid()) wanted = m_scene->FindByName("Дверь");
        if (wanted.Valid()) m_selection.SetPrimary(wanted.Id());
        LOG_INFO("Editor") << "SAGE_EDITOR_VARS_DEMO: дверь, ключ и кнопка со связями созданы";
    }

    // Выложить модели в сцену тем же путём, каким это делает перетаскивание
    // (AddAssetToScene): пути через ';'. Нужно для проверки на живых ассетах —
    // как модель встаёт, что с её материалом и развёрткой.
    if (const std::string list = sage::EnvString("SAGE_EDITOR_LOAD_MODELS"); !list.empty()) {
        m_headlessProject = true;
        std::string rest = list;
        float x = -6.0f;
        while (!rest.empty()) {
            const size_t sep = rest.find(';');
            std::string one = rest.substr(0, sep);
            rest = (sep == std::string::npos) ? std::string() : rest.substr(sep + 1);
            if (one.empty()) continue;
            if (!AddAssetToScene(one)) {
                LOG_ERROR("Editor") << "SAGE_EDITOR_LOAD_MODELS: не встала модель " << one;
                continue;
            }
            // Раскладываем в ряд: иначе всё оказывается в начале координат.
            GameObject obj = m_scene->Get(m_selection.Primary());
            if (obj.Valid()) {
                obj.GetTransform().Position = glm::vec3(x, 0.0f, 0.0f);
                LOG_INFO("Editor") << "загружено: " << one;
            }
            x += 3.0f;
        }
        // Камера подводится к последней поставленной модели. Без этого хук не
        // отвечал на вопрос, ради которого заведён («что с её материалом и
        // развёрткой»): модель вставала в ряд далеко от камеры и занимала в
        // кадре десяток пикселей, по которым не видно ни шва текстуры, ни позы.
        if (m_selection.Primary() >= 0) FocusSelected();
    }
    // Открыть окно About (версии подсистем) при старте — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_ABOUT")) { m_headlessProject = true; m_showAbout = true; }
    // Палитра команд — тем же способом: увидеть её иначе можно только руками
    // на своей машине, а значит и проверить, как она выглядит, будет нечем.
    if (const std::string q = sage::EnvString("SAGE_EDITOR_PALETTE"); !q.empty()) {
        m_headlessProject = true;
        m_palette.Open();
        m_paletteQuery = (q == "1") ? std::string() : q;
    }
    // Вывести вперёд панель Game (вид от игровой камеры) — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_GAME")) m_game.RequestFocus();

    // Авто-вход в Play при старте (визуальная проверка/CI): вешает spin.lua на
    // Green Cube демо-сцены и нажимает Play — на скриншоте куб будет повёрнут,
    // а в тулбаре гореть PLAYING. Launcher в этом режиме не показываем.
    if (std::getenv("SAGE_EDITOR_AUTOPLAY")) {
        m_headlessProject = true; // headless-прогон: проект нужен, но выбирать его некому
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            m_selection.SetPrimary(green.Id());
        }
        StartPlay();
    }

    // Подстраховка: хук мог поднять флаг уже после блока выше (например,
    // автоигра). Проект всё равно должен быть — работы без него нет.
    if (m_headlessProject && !m_project.Loaded()) {
        std::string err;
        if (!CreateProject(".", "sage_headless_project", DefaultProjectTemplate(), err))
            LOG_ERROR("Editor") << "Не удалось создать проект для headless-прогона: " << err;
    }
}
