#include "EditorLayer.h"
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
#include "sage/render/DebugView.h"
#include "sage/core/Application.h"
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
#include "sage/render/ParticlePresets.h"
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
    auto hasSelection = [this] { return SelectedId() != 0; };

    m_commands.Add({"scene.save", T("Save Scene"), scene, "Ctrl+S", "scene",
                    [this] { return !m_scenePath.empty(); },
                    [this] { SaveSceneToFile(m_scenePath); }});
    m_commands.Add({"scene.new", T("New Scene"), scene, "", "scene", hasProject,
                    [this] { NewScene(ProjectTemplateKind::Empty); }});
    m_commands.Add({"edit.undo", T("Undo"), scene, "Ctrl+Z", "refresh",
                    [this] { return !m_undoStack.empty(); }, [this] { Undo(); }});
    m_commands.Add({"edit.redo", T("Redo"), scene, "Ctrl+Shift+Z", "refresh",
                    [this] { return !m_redoStack.empty(); }, [this] { Redo(); }});

    m_commands.Add({"play.start", T("Play"), scene, "", "play",
                    [this] { return m_playState == EditorPlayState::Editing; },
                    [this] { StartPlay(); }});
    m_commands.Add({"play.stop", T("Stop"), scene, "", "stop",
                    [this] { return InPlayMode(); }, [this] { StopPlay(); }});

    m_commands.Add({"object.duplicate", T("Duplicate"), object, "Ctrl+D", "cube",
                    hasSelection, [this] { DuplicateSelected(); }});
    m_commands.Add({"object.delete", T("Delete"), object, "Del", "cube",
                    hasSelection, [this] { DeleteSelected(); }});

    // Панели — по одной команде на панель: «где включается консоль» перестаёт
    // быть поиском по меню.
    struct PanelCmd { const char* Id; const char* Title; EditorPanel Panel; const char* Icon; };
    static const PanelCmd kPanels[] = {
        {"panel.hierarchy", "Hierarchy", EditorPanel::Hierarchy, "scene"},
        {"panel.inspector", "Inspector", EditorPanel::Inspector, "cube"},
        {"panel.viewport", "Viewport", EditorPanel::Viewport, "camera"},
        {"panel.game", "Game", EditorPanel::Game, "play"},
        {"panel.assets", "Assets", EditorPanel::Assets, "folder"},
        {"panel.console", "Console", EditorPanel::Console, "code"},
        {"panel.environment", "Environment", EditorPanel::Environment, "sun"},
        {"panel.code", "Code", EditorPanel::Code, "script"},
        {"panel.profiler", "Profiler", EditorPanel::Profiler, "grid"},
    };
    for (const PanelCmd& p : kPanels) {
        const EditorPanel panel = p.Panel;
        m_commands.Add({p.Id, T(p.Title), window, "", p.Icon, {},
                        [this, panel] { PanelVisible(panel) = !PanelVisible(panel); }});
    }
    m_commands.Add({"window.templates", T("Project templates..."), window, "", "folder", {},
                    [this] { m_showTemplates = true; }});
    m_commands.Add({"window.settings", T("Game Settings..."), window, "", "project", {},
                    [this] { m_showSettings = true; }});
    m_commands.Add({"window.reset", T("Reset Layout"), window, "", "grid", {},
                    [this] { ShowAllPanels(); m_rebuildDockLayout = true; }});

    // Оформление — командами тоже: тема меняется без похода в меню, и это тот
    // случай, когда её меняют часто (свет в комнате не постоянен).
    for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
        const std::string id = theme.Id;
        m_commands.Add({"theme." + id, std::string(T("Theme:")) + " " + theme.Name, view, "",
                        "sun", {}, [id] { EditorTheme::SetTheme(id); }});
    }
    m_commands.Add({"view.grid", T("Show Grid"), view, "", "grid", {},
                    [this] { m_showGrid = !m_showGrid; }});
}

void EditorLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // Копии геометрии на стороне процессора — до первого запроса любого меша,
    // иначе примитивы успеют осесть в кэше без них. Нужны для точного выбора
    // объекта мышью: без копии попадание считается по коробке, и клик в
    // просвет между деталями модели выбирает модель. В собранной игре флаг не
    // взводится — там эти мегабайты ни на что не работают.
    ResourceManager::Instance().SetKeepMeshCpuData(true);

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

    EditorTheme::LoadFont();
    // Init, а не Apply: он собирает список тем (встроенные + themes/*.json) и
    // применяет ту, что человек выбрал в прошлый раз, вместе с масштабом.
    EditorTheme::Init();
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;

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
                   (m_playState == EditorPlayState::Editing ? "Editing" : "Play") + "\n";
            if (m_scene) ctx += std::string("Entities: ") + std::to_string(m_scene->Count()) + "\n";
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

    // Отчёт о прошлом падении — ищем сразу после установки обработчика: если
    // прошлый запуск умер, человек должен узнать об этом первым делом, а не
    // после того, как заново соберёт сцену.
    FindCrashReport();

    // --- Восстановление после прошлого падения ---
    // Файл на диске — единственный след прошлого запуска: спрашивать некого,
    // процесс тот уже мёртв. Предложение показывается ОДИН раз; отказ удаляет
    // файл, иначе редактор спрашивал бы про него до конца времён.
    {
        std::error_code ec;
        for (const char* candidate : {"sage-recovered.sage", "sage-autosave.sage"}) {
            if (fs::exists(candidate, ec)) {
                m_recoveryFile = candidate;
                m_recoveryPrompt = true;
                LOG_WARN("Editor") << "Найден файл восстановления: " << candidate;
                break;
            }
        }
    }

    // --- Превью-рендер: тени/Viewport/Game/PostFX/гизмо — в EditorSceneRenderer ---
    m_renderer.Init();

    // Системы кадра. В режиме ПРАВКИ живут только анимация и частицы — это
    // превью: художник должен видеть, как двигается модель и как выглядит
    // эффект, не запуская игру. Скрипты и физика добавляются на входе в Play и
    // снимаются на выходе (см. StartPlay/StopPlay) — иначе правка сцены
    // означала бы её симуляцию, и объект уезжал бы из-под курсора.
    {
        sage::CoreSystems preview;
        preview.Particles = &m_renderer.Particles();
        sage::RegisterCoreSystems(m_systems, preview);
    }

    m_gizmoOp = (int)ImGuizmo::TRANSLATE; // дефолтный режим гизмо (default 0 невалиден)

    NewScene(ProjectTemplateKind::Demo);

    m_camera.Position = {6.5f, 5.0f, 6.5f};
    m_camera.Yaw = -135.0f;
    m_camera.Pitch = -28.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Дефолтные пути диалогов теперь инициализирует DialogsPanel (в конструкторе).
    m_assetsCwd = fs::current_path();

    m_recent.Load();

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
            m_renderMode = EditorRenderMode::Wireframe;
        } else if (sage::render::ParseDebugView(mode.c_str(), dv)) {
            m_renderMode = dv == sage::render::DebugView::None
                               ? EditorRenderMode::Shaded
                               : (EditorRenderMode)((int)dv + 1);
        } else {
            LOG_WARN("Editor") << "неизвестный SAGE_EDITOR_RENDER_MODE '" << mode << "'";
        }
    }

    LOG_INFO("Editor") << "SAGE Editor started (entities: " << m_scene->Count() << ")";

    // --- Плагины (v1, только редактор — см. PluginAPI.h) ---
    // ПО УМОЛЧАНИЮ ОТКЛЮЧЕНЫ: система плагинов v1 экспериментальная (нестабильный
    // ABI между сборками), поэтому редактор их не грузит, пока явно не разрешено
    // переменной SAGE_EDITOR_PLUGINS=1. Без неё plugins/ игнорируется.
    if (std::getenv("SAGE_EDITOR_PLUGINS")) {
        fs::path pluginsDir = fs::current_path() / "plugins";
        if (const fs::path dir = sage::EnvPath("SAGE_PLUGINS_DIR"); !dir.empty())
            pluginsDir = dir;
        m_plugins.LoadAll(pluginsDir, m_pluginCtx);
    } else {
        LOG_INFO("Editor") << "Плагины редактора отключены (SAGE_EDITOR_PLUGINS не задан)";
    }

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

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();
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
            "SAGE_EDITOR_UI_EDITOR",    "SAGE_EDITOR_COLLIDER_MODE", "SAGE_EDITOR_SELECT_ENTITY",
            "SAGE_EDITOR_SELECT_ASSET", "SAGE_EDITOR_OPEN_CODE",     "SAGE_EDITOR_SHOW_ABOUT",
            "SAGE_EDITOR_AUTOPLAY",       "SAGE_EDITOR_VARS_DEMO",
            "SAGE_EDITOR_TEMPLATE_SHOTS", "SAGE_EDITOR_SHOW_TEMPLATES",
            "SAGE_EDITOR_PALETTE",
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

    // Открыть окно Settings при старте (для скриншот-проверки/демо настроек).
    if (std::getenv("SAGE_EDITOR_SHOW_SETTINGS")) { m_headlessProject = true; m_showSettings = true; }
    if (std::getenv("SAGE_EDITOR_SHOW_PROFILER")) { m_headlessProject = true; m_showProfiler = true; }
    if (std::getenv("SAGE_EDITOR_ICON_SHEET")) { m_headlessProject = true; m_showIconSheet = true; }
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
        m_showUIEditor = true;
        m_uiEditor.RequestFocus();
    }
    if (const char* b = std::getenv("SAGE_EDITOR_UI_BACKDROP"))
        m_uiTools.Backdrop = (float)std::atof(b);
    if (std::getenv("SAGE_EDITOR_COLLIDER_MODE")) { m_headlessProject = true; m_colliderEdit = true; }
    if (const char* name = std::getenv("SAGE_EDITOR_SELECT_ENTITY")) {
        m_headlessProject = true;
        GameObject obj = m_scene->FindByName(name);
        if (obj.Valid()) SetSelectedId(obj.Id());
        else LOG_WARN("Editor") << "SAGE_EDITOR_SELECT_ENTITY: нет сущности с именем " << name;
    }
    if (const char* a = std::getenv("SAGE_EDITOR_SELECT_ASSET")) {
        m_headlessProject = true;
        m_assets.Select(a);
    }
    if (const std::string f = sage::EnvString("SAGE_EDITOR_OPEN_CODE"); !f.empty()) {
        m_headlessProject = true;
        m_showCode = true;
        m_code.OpenFile(f);
    }
    // Закрыть все панели — состояние, в которое человек попадал крестиками и из
    // которого раньше не было выхода. Проверять его иначе нечем: кликать по
    // крестикам в CI некому, а именно на этом кадре должна быть видна подсказка
    // «Все панели закрыты» с кнопкой возврата.
    if (std::getenv("SAGE_EDITOR_CLOSE_PANELS")) {
        m_headlessProject = true;
        m_showHierarchy = m_showInspector = m_showEnvironment = m_showUIEditor = false;
        m_showViewport = m_showGame = m_showConsole = m_showAssets = false;
        m_showCode = m_showProfiler = false;
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
        reg.emplace_or_replace<AnimatedModelComponent>(e);
        reg.emplace_or_replace<IKComponent>(e);
        reg.emplace_or_replace<ReflectionProbeComponent>(e);
        reg.emplace_or_replace<ScriptComponent>(e, ScriptComponent{"assets/scripts/spin.lua"});
        sage::ui::ApplyPreset(reg, e, "Button");
        ParticleEmitterComponent em;
        em.Config = ParticlePresets::Registry()[0].Make();
        reg.emplace_or_replace<ParticleEmitterComponent>(e, em);
        SetSelectedId(all.Id());
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
              << "function OnMessage(entity, name, data)\n"
              << "    if name == \"Open\" then entity:Vars().locked = false end\n"
              << "end\n";
        }
        GameObject key = m_scene->CreateObject("Ключ");
        GameObject door = m_scene->CreateObject("Дверь");
        entt::registry& reg = m_scene->Registry();
        reg.emplace_or_replace<ScriptComponent>(door.Entity(),
                                                ScriptComponent{m_project.AssetRef(scriptPath)});
        MergeScriptVars(door);
        if (VarsComponent* vc = reg.try_get<VarsComponent>(door.Entity()))
            vc->Values.Set("needs", sage::vars::Value(sage::vars::EntityRef{key.Id()}));

        GameObject button = m_scene->CreateObject("Кнопка «Открыть»");
        sage::ui::Transform t;
        t.Anchor = UIAnchor::Center;
        t.Offset = {0.0f, 0.0f};
        t.Size = {220.0f, 56.0f};
        reg.emplace_or_replace<sage::ui::Transform>(button.Entity(), t);
        reg.emplace_or_replace<sage::ui::Fill>(button.Entity());
        sage::ui::Interactable& act =
            reg.emplace_or_replace<sage::ui::Interactable>(button.Entity());
        sage::events::Binding open;
        open.Trigger = "click";
        open.Event = "door.open";
        open.Target = sage::vars::EntityRef{door.Id()};
        open.Method = "Open";
        open.Arg = sage::vars::Value(std::string("медленно"));
        act.Events.push_back(open);
        sage::events::Binding sound;
        sound.Trigger = "hoverIn";
        sound.Event = "ui.hover";
        act.Events.push_back(sound);

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
            if (m_playScripts) m_playScripts->UpdateAll(0.016f);
            sage::ui::UIInputState down;
            down.Mouse = {640.0f, 360.0f};   // центр экрана — там стоит кнопка
            down.MouseDown = true;
            down.MousePressed = true;
            sage::ui::UpdateSceneUI(*m_scene, down, 1280, 720);
            sage::ui::UIInputState up;
            up.Mouse = down.Mouse;
            up.MouseReleased = true;
            sage::ui::UpdateSceneUI(*m_scene, up, 1280, 720);
            if (m_playScripts) m_playScripts->UpdateAll(0.016f);
        }

        // Выбор: дверь по умолчанию, но SAGE_EDITOR_SELECT_ENTITY сильнее — он
        // отрабатывает ВЫШЕ, когда этих объектов ещё нет, и без повтора здесь
        // посмотреть на кнопку было бы нельзя. Ищем ПО ИМЕНИ ещё раз: Play
        // пересоздаёт сцену из снимка, и прежние дескрипторы уже чужие.
        GameObject wanted;
        if (const char* pick = std::getenv("SAGE_EDITOR_SELECT_ENTITY"))
            wanted = m_scene->FindByName(pick);
        if (!wanted.Valid()) wanted = m_scene->FindByName("Дверь");
        if (wanted.Valid()) SetSelectedId(wanted.Id());
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
            GameObject obj = m_scene->Get(m_selectedId);
            if (obj.Valid()) {
                obj.GetTransform().Position = glm::vec3(x, 0.0f, 0.0f);
                LOG_INFO("Editor") << "загружено: " << one;
            }
            x += 3.0f;
        }
    }
    // Открыть окно About (версии подсистем) при старте — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_ABOUT")) { m_headlessProject = true; m_showAbout = true; }
    // Окно шаблонов — тем же способом, что и остальные: увидеть его иначе можно
    // только руками на своей машине, а значит и проверить, как оно выглядит,
    // будет нечем.
    if (std::getenv("SAGE_EDITOR_SHOW_TEMPLATES")) { m_headlessProject = true; m_showTemplates = true; }
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
            SetSelectedId(green.Id());
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
    m_plugins.UnloadAll(); // ДО разрушения ImGui-контекста — плагины рисуют через тот же ImGui
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
    if (m_autosaveInterval > 0.0f && m_sceneDirty && m_scene &&
        m_playState == EditorPlayState::Editing) {
        m_autosaveTimer += dt;
        if (m_autosaveTimer >= m_autosaveInterval) {
            m_autosaveTimer = 0.0f;
            try {
                SceneSerializer::Save(*m_scene, "sage-autosave.sage");
                m_lastAutosave = "sage-autosave.sage";
                LOG_DEBUG("Editor") << "Автосохранение: sage-autosave.sage";
            } catch (const std::exception& e) {
                // Не сумели — не беда для кадра, но сказать надо: молчащее
                // автосохранение хуже отсутствующего, на него рассчитывают.
                LOG_WARN("Editor") << "Автосохранение не удалось: " << e.what();
                m_autosaveInterval = 0.0f;   // не долбить диск каждую минуту
            }
        }
    }

    // Логика правки — событийная, живёт в панелях. Единственный
    // "симуляционный" тик — Play: скрипты сущностей, пока не пауза.
    if (m_playState == EditorPlayState::Playing) {
        // Ввод игре — только пока в фокусе панель Game (см. EditorPlayInput).
        // В остальное время действия гасятся: клавиши уходят редактору.
        if (m_playRawInput) {
            GLFWwindow* handle = sage::Application::Get().GetWindow().Handle();
            m_playRawInput->SetGameFocused(m_game.Focused());
            m_playRawInput->SyncCapture();
            if (m_game.Focused()) m_playInput.Update(handle);
            else m_playInput.UpdateIdle();

            // ESC отпускает захваченный курсор, не выходя из Play: иначе из
            // игры от первого лица в редакторе было бы не выбраться мышью.
            if (m_playRawInput->MouseCaptured() &&
                glfwGetKey(handle, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                m_playRawInput->SetMouseCaptured(false);
            }
        }
        UpdatePlayUiInput(dt);
    }
    // Время сцены для uTime собственных шейдеров + горячая перезагрузка
    // изменённых .vert/.frag: правка шейдера видна во вьюпорте сразу.
    m_renderer.Tick(dt);
    ResourceManager::Instance().ReloadChangedShaders();
    // И ресурсы: модель или материал могли поправить снаружи (Blender, другой
    // редактор, скрипт). Без этого движок показывал бы прежние данные до
    // перезапуска — ровно то, что выглядит как «правка не применилась».
    ResourceManager::Instance().ReloadChangedAssets();

    // Кадр — планировщиком. В правке это анимация и частицы (превью), в Play к
    // ним добавляются скрипты и физика, и порядок между ними задан один раз в
    // RegisterCoreSystems, а не переписан здесь по памяти.
    // Масштаб времени и пауза игры (sage.time.SetScale, sage.game.Pause)
    // действуют и в Play-режиме редактора: иначе замедление, настроенное для
    // игры, работало бы в собранной игре и не работало там, где его настраивают.
    const float scale = m_playScripts ? m_playScripts->FrameTimeScale() : 1.0f;
    m_systems.Run(*m_scene, dt * scale);
    m_plugins.UpdateAll(dt);

    // Чего игра попросила за кадр. Здесь — после того, как все скрипты
    // отработали и ни один не находится на стеке.
    if (m_playScripts) {
        if (m_playScripts->TakeQuitRequest()) {
            // В редакторе «выйти из игры» — это остановить Play, а не закрыть
            // редактор: у человека несохранённая сцена, и закрывать её по
            // просьбе скрипта нельзя.
            LOG_INFO("Editor") << "скрипт попросил выйти из игры — останавливаю Play";
            StopPlay();
            return;
        }
        std::string sceneName;
        const bool restart = m_playScripts->TakeRestartRequest();
        if (m_playScripts->TakeSceneRequest(sceneName) || restart) {
            // Смена сцены В РЕДАКТОРЕ пока не поддержана: Play работает с той
            // сценой, что открыта, и подменить её под человеком, не спросив,
            // значило бы потерять его несохранённую правку. Говорим прямо,
            // вместо того чтобы молча ничего не сделать.
            LOG_WARN("Editor") << "sage.scene.Load/Restart в Play-режиме не выполняется — "
                                  "проверяйте переходы между сценами в собранной игре";
        }
    }

    // Правка в окне Settings обязана быть видна В КАДРЕ, а не после
    // перезапуска: ползунок, который «сработает потом», невозможно настроить.
    ApplyEngineSettings();

    // Файлы, брошенные в окно с прошлого кадра (см. HandleDroppedFiles).
    HandleDroppedFiles();
}

// ============================================================================
//  Плагины редактора — реализация facade'а EditorPluginContext
// ============================================================================

void EditorLayer::PluginContextImpl::Log(const char* message) {
    LOG_INFO("Plugin") << message;
}

const char* EditorLayer::PluginContextImpl::SelectedEntityName() const {
    GameObject obj = m_owner.m_scene->Get(m_owner.m_selectedId);
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
    m_renderer.SetShowBounds(m_showBounds);
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
    m_renderer.RenderViewport(*m_scene, m_camera, env, m_selectedId, m_selection, m_renderMode, m_showGrid,
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
        m_renderer.RenderViewport(*m_scene, m_camera, env, m_selectedId, m_selection, m_renderMode,
                                  m_showGrid, cfg, v, p, i, ov);
    } // отдаёт view/proj для гизмо/пикинга
    m_renderer.RenderGame(*m_scene, env, cfg);      // Primary-камера сцены (если есть)

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

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

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
        m_launcher.Draw(*this, m_recent);
        m_dialogs.Draw(*this);   // диалог обзора папок открывается отсюда же
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
    if (m_showHierarchy) m_hierarchy.Draw(*this, &m_showHierarchy);
    if (m_showInspector) m_inspector.Draw(*this, &m_showInspector);
    if (m_showEnvironment) m_environment.Draw(*this, &m_showEnvironment);
    if (m_showUIEditor) m_uiEditor.Draw(*this, &m_showUIEditor);
    if (m_showViewport) m_viewport.Draw(*this, &m_showViewport);
    if (m_showGame) m_game.Draw(*this, &m_showGame);
    // Код подаётся ПОСЛЕ Viewport и Game, потому что порядок вкладок в узле
    // доккинга — это порядок подачи окон в кадре, а не порядок DockBuilder'а.
    // Пока он подавался раньше (внутри окна-хоста), вкладка «Код» вставала
    // первой, и раскладка читалась как «Код | Viewport | Game».
    if (m_showCode) m_code.Draw(&m_showCode);
    if (m_showConsole) m_console.Draw(&m_showConsole);
    if (m_showAssets) m_assets.Draw(*this, &m_showAssets);
    m_plugins.ImGuiAll();

    // Стартовое окно по просьбе (Window > Стартовое окно): проект уже открыт,
    // но человек хочет открыть другой.
    if (m_launcherRequested) {
        const std::string was = m_project.Dir().string();
        m_launcher.Draw(*this, m_recent);
        if (m_project.Dir().string() != was) m_launcherRequested = false;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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

    TakeAutoScreenshot(app);
}

void EditorLayer::TakeAutoScreenshot(sage::Application& app) {
    ++m_frameCounter;
    if (m_autoScreenshotFrame < 0 || m_frameCounter != m_autoScreenshotFrame) return;
    Window& win = app.GetWindow();
    SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
    app.Close();
}
