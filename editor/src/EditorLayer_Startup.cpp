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

// ЗАПУСК РЕДАКТОРА по этапам (OnAttach зовёт их по порядку).

// ImGui, докинг, окна-вьюпорты, шрифт и тема.
void EditorLayer::InitImGui() {
    sage::Application& app = sage::Application::Get();

    // Копии геометрии на стороне процессора — до первого запроса любого меша,
    // иначе примитивы успеют осесть в кэше без них. Нужны для точного выбора
    // объекта мышью: без копии попадание считается по коробке, и клик в
    // просвет между деталями модели выбирает модель. В собранной игре флаг не
    // взводится — там эти мегабайты ни на что не работают.
    ResourceManager::Instance().SetKeepMeshCpuData(true);
    // Карты материалов — фоном: модель, брошенная в сцену, появляется сразу, а
    // её текстуры доезжают за несколько кадров. Синхронно редактор замирал на
    // декод каждой 2048² прямо в кадре (см. SetStreamMaterialTextures).
    ResourceManager::Instance().SetStreamMaterialTextures(true);

    // --- Ввод игры в Play-режиме -------------------------------------------
    //
    // Мост к окну ставится ОДИН раз, здесь, а не на каждый Play: подписка на
    // события окна снимается только вместе с окном, и второй мост означал бы
    // два одинаковых события на одно нажатие. Раскладка при этом на каждый Play
    // своя — её сбрасывает ClearActions (см. StartPlay).
    // Захватом курсора распоряжается не мост, а условие «панель Game в фокусе»
    // (см. EditorPlayInput): иначе игра, попросившая обзор от первого лица,
    // отняла бы у редактора мышь насовсем. Всё это связывает сессия.
    m_play.Attach(m_playInputBridge, app.GetWindow());
    // Вид гизмо коллайдеров — привычка человека, а не сцены: из настроек редактора.
    m_tools.ColliderGizmo.Load();

    // --- ImGui: docking + multi-viewport (панели можно вытаскивать в
    // отдельные OS-окна — «плавающие» панели становятся полноценными окнами) ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = "sage_editor_imgui.ini"; // своё имя, чтобы не пересекаться с другими ImGui-приложениями
    // Панель таскается только за заголовок/вкладку. По умолчанию ImGui двигает
    // окно перетаскиванием ЛЮБОГО пустого места в нём — а в редакторе пустого
    // места полно: поля Инспектора, пустая область Иерархии, промежутки между
    // плитками Assets. Промах мимо виджета на пару пикселей выдёргивал панель
    // из дока, и раскладка «разъезжалась» сама собой, без единого осознанного
    // действия.
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // ПИПЕТКА ПОЛЕЙ ЦВЕТА — экран системы за пределами окна (ScreenColor.h),
    // кнопки мыши и Esc где угодно, и «курсор над своим окном» — тогда цвет
    // берётся из своего кадра (см. чтение пикселя после отрисовки ниже).
    {
        namespace sc = sage::editor::screencolor;
        Sage::UI::eyedropper::Backend dropper;
        dropper.SampleScreen = [](float rgb[3]) { return sc::SampleAtCursor(rgb); };
        dropper.GlobalMouseDown = [](int button) { return sc::GlobalMouseDown(button); };
        dropper.GlobalEscapeDown = [] { return sc::GlobalEscapeDown(); };
        dropper.OverOwnWindow = [] {
            return glfwGetWindowAttrib(sage::Application::Get().GetWindow().Handle(), GLFW_HOVERED) != 0;
        };
        Sage::UI::eyedropper::SetBackend(dropper);
        LOG_INFO("Editor") << "Пипетка: " << (sc::GlobalAvailable() ? "весь экран" : "только окно редактора");
    }

    // ОТДЕЛЬНОЕ ОКНО — НАСТОЯЩЕЕ ОКНО СИСТЕМЫ, С РАМКОЙ И КНОПКАМИ.
    //
    // По умолчанию ImGui создаёт вторичные окна БЕЗ украшений системы и рисует
    // заголовок сам. Для человека это не окно: нет кнопок «свернуть» и
    // «развернуть», окно не разворачивается двойным щелчком по заголовку, не
    // прилипает к краю экрана и не ведёт себя как остальные окна Windows —
    // хотя выглядит как окно и стоит отдельно от редактора.
    //
    // С этой строкой панель, вынесенная в своё окно, получает обычную рамку
    // системы: свернуть, развернуть, закрыть, растянуть за край, разложить
    // рядом с другими окнами. Свой заголовок ImGui при этом гасится у таких
    // панелей (см. panelwindows::WindowFlags) — иначе заголовка было бы два.
    io.ConfigViewportsNoDecoration = false;

    // Претензии самого ImGui — в наш лог, то есть в консоль редактора.
    //
    // ImGui умеет ловить незакрытые Begin/End, PushID без Pop, лишний
    // BeginDisabled — но по умолчанию пишет об этом только во внутренний
    // отладочный лог, которого в редакторе никто не видит. А последствие у
    // такой ошибки не косметическое: кадр с незакрытым окном уходит в
    // отрисовку обрубленным, и человек получает ЧЁРНЫЙ прямоугольник вместо
    // редактора — ровно так ломалось создание проекта из стартового окна.
    // Пусть в следующий раз в консоли будет написано, ЧТО и В КАКОМ окне.
    ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext* ctx, void*, const char* msg) {
        const ImGuiWindow* w = ctx->CurrentWindow;
        LOG_ERROR("ImGui") << "кадр интерфейса собран неверно — окно '"
                           << (w ? w->Name : "<нет>") << "': " << msg;
    };

    // Кто из панелей живёт своим окном системы — помнится между запусками
    // (см. PanelWindows.h). Читается ДО первого кадра: в кадре это уже поздно,
    // панель успеет нарисоваться в доке и уехать оттуда рывком.
    panelwindows::Load();

    EditorTheme::LoadFont();
    // Init, а не Apply: он собирает список тем (встроенные + themes/*.json) и
    // применяет ту, что человек выбрал в прошлый раз, вместе с масштабом.
    EditorTheme::Init();
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;
}

// Консоль, восстановление, рендер, демо-сцена, камера, настройки, бросок файлов.
void EditorLayer::InitEditorServices() {
    // Панели — первыми: всё дальше (раскладка, хуки прогонов, плагины) может
    // спрашивать о них реестр.
    RegisterPanels();
    // --- Console первой: сток лога ловит все сообщения запуска ---
    m_console.Attach();

    // --- Готовность к падению ---
    //
    // До этого падение выглядело так: окно исчезло. Ни файла, ни строки, ни
    // намёка — и вместе с окном исчезала несохранённая сцена. Для редактора это
    // худший из отказов: пользователь теряет работу и не может даже сказать, на
    // чём сломалось.
    //
    // Ставится РАНО, сразу после лога: падения на загрузке проекта или на
    // компиляции шейдера — самые частые, и обработчик, установленный в конце
    // OnAttach, их бы не застал.
    {
        sage::CrashHandler::Config cfg;
        cfg.AppName = "SAGE Editor";
        cfg.Version = kSageEngineVersion;
        cfg.ReportDir = ".";
        cfg.Context = [this]() {
            // Отчёт о падении НЕ переводится, и это не недоделка. Его читает
            // разработчик, часто чужой и не знающий языка пользователя; отчёт
            // на незнакомом языке — это отчёт, который не прочтут. Язык
            // интерфейса тут ни при чём: в отчёте важно, чтобы строки
            // совпадали с тем, что ищут по исходникам.
            std::string ctx;
            ctx += std::string("Project: ") +
                   m_project.Dir().string() + "\n";
            ctx += std::string("Scene:  ") +
                   (m_scenePath.empty() ? "<no file>" : m_scenePath.string()) +
                   (m_sceneDirty ? "  (NOT SAVED)\n" : "\n");
            ctx += std::string("Mode:   ") +
                   (!m_play.Active() ? "Editing" : "Play") + "\n";
            if (m_scene) ctx += std::string("Entities: ") + std::to_string(m_scene->Count()) + "\n";
            // Что шло не так ДО падения: часто это и есть ответ (скрипт
            // зависал, шейдер не собрался, модель была битой).
            const std::string problems = sage::Problems::CrashSummary();
            if (!problems.empty()) ctx += "Problems before the crash:\n" + problems;
            return ctx;
        };
        cfg.EmergencySave = [this]() -> std::string {
            // Сцену сбрасываем в ОТДЕЛЬНЫЙ файл, а не поверх исходного: процесс
            // уже сломан, и записать поверх рабочей сцены повреждённые данные
            // хуже, чем не записать ничего. Восстановление предлагается при
            // следующем запуске.
            if (!m_scene) return {};
            try {
                const std::string path = "sage-recovered.sage";
                SceneSerializer::Save(*m_scene, path);
                return path;
            } catch (...) {
                return {};
            }
        };
        sage::CrashHandler::Install(cfg);
    }

    // КАРАНТИН — ДО ПЕРВОЙ ЗАГРУЗКИ ЧЕГО-ЛИБО и до открытия проекта.
    //
    // «Если с одним проектом что-то не пойдёт, я целый редактор открыть НЕ
    // СМОГУ» — и это не преувеличение: открыл проект, редактор сам грузит его
    // сцену, в сцене ссылка на битую модель, падение; запустил снова, открыл тот
    // же проект (он в списке первым) — то же падение. Выйти из кольца нечем:
    // редактор не доживает до состояния, в котором можно что-то нажать.
    //
    // Метка, пережившая запуск, называет файл, на котором процесс умер. Такой
    // файл в карантине: редактор открывается, проект открывается, сцена
    // открывается — не появляется одна модель, и про неё сказано прямым текстом
    // (см. assets/Quarantine.h).
    sage::assets::quarantine::SetDirectory(".");
    if (const std::string broken = sage::assets::quarantine::TakeUnfinished(); !broken.empty()) {
        SetStatusMessage(T("Last session died on a file — it is quarantined: ") + broken);
    }

    // Отчёт о прошлом падении — ищем сразу после установки обработчика: если
    // прошлый запуск умер, человек должен узнать об этом первым делом, а не
    // после того, как заново соберёт сцену.
    // Отчёт о падении и файл восстановления — один поиск: и то, и другое след
    // прошлого запуска, и спрашивать о них некого, тот процесс уже мёртв.
    m_recovery.ScanOnStartup();

    // --- Превью-рендер: тени/Viewport/Game/PostFX/гизмо — в EditorSceneRenderer ---
    m_renderer.Init();

    // Системы кадра. В режиме ПРАВКИ живут только анимация и частицы — это
    // превью: художник должен видеть, как двигается модель и как выглядит
    // эффект, не запуская игру. Скрипты и физика добавляются на входе в Play и
    // снимаются на выходе (см. StartPlay/StopPlay) — иначе правка сцены
    // означала бы её симуляцию, и объект уезжал бы из-под курсора.
    //
    // ЗВУК — ТОЖЕ ПРЕВЬЮ, и по той же причине. Устройство заводилось только на
    // входе в Play, а стадия звука регистрировалась вместе с ним, поэтому в
    // режиме правки команду «послушать» из инспектора НЕКОМУ было выполнить:
    // компонент выставлял её и оставался с ней навсегда. Со стороны это ровно
    // то, на что пожаловались, — «нажимаю превью, ничего не происходит», без
    // единой строки в логе, потому что и ошибки никакой не случилось.
    //
    // Устройство одно на редактор: заводить второе на время Play значило бы
    // глушить превью на входе в игру и получать два микшера на одну звуковую
    // карту.
    {
        sage::CoreSystems preview;
        preview.Particles = &m_renderer.Particles();
        preview.Audio = &m_play.Audio(); // заводит устройство, если его ещё нет
        sage::RegisterCoreSystems(m_systems, preview);
    }

    m_tools.GizmoOp = (int)ImGuizmo::TRANSLATE; // дефолтный режим гизмо (default 0 невалиден)

    NewScene(ProjectTemplateKind::Demo);

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Дефолтные пути диалогов теперь инициализирует DialogsPanel (в конструкторе).
    // До открытия проекта папки ассетов ещё нет — панель всё равно ничего не
    // покажет (работы без проекта в редакторе нет), а как только проект
    // откроется, сюда встанет его assets/.
    m_assetsCwd = fs::current_path();

    m_projects.Load();

    // Настройки движка ДО первого кадра: файл рядом с редактором, поверх него —
    // переменные окружения (как это делает рантайм в своём main). Без этого
    // редактор рисовал по умолчаниям, что бы ни лежало в sage.cfg и что бы ни
    // задали через SAGE_*.
    m_settings.LoadFile("sage.cfg");
    m_settings.ApplyEnvOverrides();
    ApplyEngineSettings();

    // Файлы, брошенные в окно из проводника, — самый короткий путь внести своё
    // в проект: тащат туда, где смотрят, а не ищут пункт меню. Разбор отложен
    // до кадра (см. HandleDroppedFiles): колбэк приходит из недр GLFW, а
    // импорт может открыть модалку и подвинуть выбор в панелях.
    // Тот же приёмник, что и у окон панелей (см. цикл по Viewports ниже).
    s_dropSink = [this](const std::vector<std::string>& paths) {
        m_droppedFiles.insert(m_droppedFiles.end(), paths.begin(), paths.end());
    };
    sage::Application::Get().GetWindow().SetFileDropCallback(
        [this](const std::vector<std::string>& paths) {
            m_droppedFiles.insert(m_droppedFiles.end(), paths.begin(), paths.end());
        });

    if (const std::string p = sage::EnvString("SAGE_SCREENSHOT_PATH"); !p.empty())
        m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) {
        m_autoScreenshotFrame = std::atoi(f);
        // Headless-скриншот обычно снимает сцену, и hub проектов ему только
        // закрывает кадр. Но снять НАДО и сам hub (и файловый диалог из него) —
        // иначе стартовое окно остаётся единственной частью редактора, которую
        // нечем проверить, кроме как открыть глазами на своей машине.
        if (!std::getenv("SAGE_SHOW_LAUNCHER")) m_headlessProject = true;
    }
    // Начальный режим рендера (для headless-скриншотов/CI): shaded, wireframe
    // или любой отладочный вид по его имени из DebugView.h (normals, cascades,
    // roughness…). Имена берутся оттуда же, что и у игры: два разных написания
    // одного и того же режима в редакторе и в плеере — лишний источник «а у
    // меня не так».
    if (const char* m = std::getenv("SAGE_EDITOR_RENDER_MODE")) {
        const std::string mode = m;
        sage::render::DebugView dv = sage::render::DebugView::None;
        if (mode == "wireframe") {
            m_tools.RenderMode = EditorRenderMode::Wireframe;
        } else if (sage::render::ParseDebugView(mode.c_str(), dv)) {
            m_tools.RenderMode = dv == sage::render::DebugView::None
                               ? EditorRenderMode::Shaded
                               : (EditorRenderMode)((int)dv + 1);
        } else {
            LOG_WARN("Editor") << "неизвестный SAGE_EDITOR_RENDER_MODE '" << mode << "'";
        }
    }

    LOG_INFO("Editor") << "SAGE Editor started (entities: " << m_scene->Count() << ")";
}

// Плагины (C++ и Lua), каталог шаблонов, реестр команд.
void EditorLayer::InitPlugins() {
    // --- Плагины -----------------------------------------------------------
    // C++ (PluginAPI.h): каждый включается в окне «Плагины» по отдельности и по
    // умолчанию ВЫКЛЮЧЕН — нестабильный ABI и код без песочницы. Переменная
    // SAGE_EDITOR_PLUGINS=1 по-прежнему включает все разом (CI, отладка).
    {
        fs::path pluginsDir = fs::current_path() / "plugins";
        // Плагины, положенные рядом с самим редактором (архив, .app на macOS),
        // — когда текущая папка не его.
        if (std::error_code pec; !fs::is_directory(pluginsDir, pec)) pluginsDir = sage::ExecutableDir() / "plugins";
        if (const fs::path dir = sage::EnvPath("SAGE_PLUGINS_DIR"); !dir.empty())
            pluginsDir = dir;
        m_nativePluginsDir = pluginsDir;
        const bool all = !sage::EnvString("SAGE_EDITOR_PLUGINS").empty();
        m_plugins.LoadAll(pluginsDir, m_pluginCtx, [all](const fs::path& file) {
            return all || sage::editor::pluginswindow::NativeEnabled(file);
        });
    }
    // Lua (LuaPlugins.h): встроенные, «мои» и плагины проекта.
    sage::editor::luaplugins::Rescan(*this);

    // Скачивание шаблона из каталога — БЕЗ ЧЕЛОВЕКА.
    //
    // Сеть — единственная часть системы шаблонов, которую нельзя проверить ни
    // модульным тестом, ни самопроверкой: там нет ни сервера, ни щелчка по
    // кнопке. Эти два хука дают smoke-тесту поднять свой http-сервер, положить
    // на него каталог и убедиться, что редактор действительно скачал и
    // поставил шаблон, — то есть проверить ровно тот путь, которым пойдёт
    // человек, а не соседний.
    if (const std::string url = sage::EnvString("SAGE_EDITOR_TEMPLATE_CATALOG"); !url.empty()) {
        sage::editor::templates::SetCatalogUrl(url);
        LOG_INFO("Editor") << "Каталог шаблонов: " << url;
    }
    if (const std::string want = sage::EnvString("SAGE_EDITOR_INSTALL_TEMPLATE"); !want.empty()) {
        m_headlessProject = true;
        std::vector<sage::editor::templates::Manifest> list;
        std::string tplErr;
        if (!sage::editor::templates::FetchCatalog(list, tplErr)) {
            LOG_ERROR("Editor") << "TEMPLATE: каталог не получен: " << tplErr;
        } else {
            bool done = false;
            for (const sage::editor::templates::Manifest& m : list) {
                if (m.Id != want) continue;
                if (sage::editor::templates::Download(m, tplErr)) {
                    RefreshProjectTemplates();
                    const ProjectTemplate* put = FindProjectTemplate(want);
                    LOG_INFO("Editor") << "TEMPLATE: установлен " << m.Name << ", в списке: "
                                       << (put ? "да" : "НЕТ");
                    done = put != nullptr;
                } else {
                    LOG_ERROR("Editor") << "TEMPLATE: не скачался: " << tplErr;
                }
                break;
            }
            if (!done) LOG_ERROR("Editor") << "TEMPLATE: FAIL";
            else LOG_INFO("Editor") << "TEMPLATE: OK";
        }
    }

    // Команды — после загрузки тем: часть из них перечисляет темы поимённо.
    RegisterCommands();
}
