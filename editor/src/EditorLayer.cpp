#include "EditorLayer.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
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

#include "EditorTheme.h"
#include "EditorIcons.h"
#include "sage/core/Application.h"
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
#include "sage/ui/UIPresets.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
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
constexpr float kToolbarHeight = 34.0f;

} // namespace

// ============================================================================
//  Жизненный цикл
// ============================================================================

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
    EditorTheme::Apply();
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
                   (m_project.Loaded() ? m_project.Dir().string() : "<none>") + "\n";
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

    NewScene(/*withDemoContent=*/true);

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
    sage::Application::Get().GetWindow().SetFileDropCallback(
        [this](const std::vector<std::string>& paths) {
            m_droppedFiles.insert(m_droppedFiles.end(), paths.begin(), paths.end());
        });

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) {
        m_autoScreenshotFrame = std::atoi(f);
        // Headless-скриншот обычно снимает сцену, и hub проектов ему только
        // закрывает кадр. Но снять НАДО и сам hub (и файловый диалог из него) —
        // иначе стартовое окно остаётся единственной частью редактора, которую
        // нечем проверить, кроме как открыть глазами на своей машине.
        if (!std::getenv("SAGE_SHOW_LAUNCHER")) m_launcher.Dismiss();
    }
    // Начальный режим рендера (для headless-скриншотов/CI): shaded|wireframe|unlit|normals.
    if (const char* m = std::getenv("SAGE_EDITOR_RENDER_MODE")) {
        std::string mode = m;
        if (mode == "wireframe") m_renderMode = EditorRenderMode::Wireframe;
        else if (mode == "unlit") m_renderMode = EditorRenderMode::Unlit;
        else if (mode == "normals") m_renderMode = EditorRenderMode::Normals;
    }

    LOG_INFO("Editor") << "SAGE Editor started (entities: " << m_scene->Count() << ")";

    // --- Плагины (v1, только редактор — см. PluginAPI.h) ---
    // ПО УМОЛЧАНИЮ ОТКЛЮЧЕНЫ: система плагинов v1 экспериментальная (нестабильный
    // ABI между сборками), поэтому редактор их не грузит, пока явно не разрешено
    // переменной SAGE_EDITOR_PLUGINS=1. Без неё plugins/ игнорируется.
    if (std::getenv("SAGE_EDITOR_PLUGINS")) {
        fs::path pluginsDir = fs::current_path() / "plugins";
        if (const char* dir = std::getenv("SAGE_PLUGINS_DIR")) pluginsDir = dir;
        m_plugins.LoadAll(pluginsDir, m_pluginCtx);
    } else {
        LOG_INFO("Editor") << "Плагины редактора отключены (SAGE_EDITOR_PLUGINS не задан)";
    }

    if (std::getenv("SAGE_EDITOR_SELFTEST")) RunSelfTest();
    if (std::getenv("SAGE_EDITOR_E2E")) RunE2EGameTest();
    if (std::getenv("SAGE_EDITOR_OPEN_PROJECT")) RunHeadlessProjectSession();

    // Открыть окно Settings при старте (для скриншот-проверки/демо настроек).
    if (std::getenv("SAGE_EDITOR_SHOW_SETTINGS")) { m_launcher.Dismiss(); m_showSettings = true; }
    if (std::getenv("SAGE_EDITOR_SHOW_PROFILER")) { m_launcher.Dismiss(); m_showProfiler = true; }
    if (std::getenv("SAGE_EDITOR_ICON_SHEET")) { m_launcher.Dismiss(); m_showIconSheet = true; }
    // Выбрать сущность по имени — для скриншот-проверок того, что рисуется
    // ТОЛЬКО при выделении: гизмо, аутлайн, габариты. Без этого проверить их
    // headless нечем: кликать во вьюпорте в CI некому.
    if (std::getenv("SAGE_EDITOR_UI_MODE")) { m_launcher.Dismiss(); m_uiEditMode = true; }
    if (const char* name = std::getenv("SAGE_EDITOR_SELECT_ENTITY")) {
        m_launcher.Dismiss();
        GameObject obj = m_scene->FindByName(name);
        if (obj.Valid()) SetSelectedId(obj.Id());
        else LOG_WARN("Editor") << "SAGE_EDITOR_SELECT_ENTITY: нет сущности с именем " << name;
    }
    if (const char* a = std::getenv("SAGE_EDITOR_SELECT_ASSET")) {
        m_launcher.Dismiss();
        m_assets.Select(a);
    }
    if (const char* f = std::getenv("SAGE_EDITOR_OPEN_CODE")) {
        m_launcher.Dismiss();
        m_showCode = true;
        m_code.OpenFile(f);
    }
    // Закрыть все панели — состояние, в которое человек попадал крестиками и из
    // которого раньше не было выхода. Проверять его иначе нечем: кликать по
    // крестикам в CI некому, а именно на этом кадре должна быть видна подсказка
    // «Все панели закрыты» с кнопкой возврата.
    if (std::getenv("SAGE_EDITOR_CLOSE_PANELS")) {
        m_launcher.Dismiss();
        m_showHierarchy = m_showInspector = m_showLighting = false;
        m_showViewport = m_showGame = m_showConsole = m_showAssets = false;
        m_showCode = m_showProfiler = false;
    }
    // Открыть окно About (версии подсистем) при старте — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_ABOUT")) { m_launcher.Dismiss(); m_showAbout = true; }
    // Вывести вперёд панель Game (вид от игровой камеры) — для скриншот-проверки.
    if (std::getenv("SAGE_EDITOR_SHOW_GAME")) m_game.RequestFocus();

    // Авто-вход в Play при старте (визуальная проверка/CI): вешает spin.lua на
    // Green Cube демо-сцены и нажимает Play — на скриншоте куб будет повёрнут,
    // а в тулбаре гореть PLAYING. Launcher в этом режиме не показываем.
    if (std::getenv("SAGE_EDITOR_AUTOPLAY")) {
        m_launcher.Dismiss(); // headless-прогон — hub не должен закрывать кадр
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            SetSelectedId(green.Id());
        }
        StartPlay();
    }

    UpdateWindowTitle();
}

void EditorLayer::OnDetach() {
    // Список строк, которым не нашлось перевода. Без него новая панель молча
    // выходит по-английски, и узнаётся об этом от пользователя, а не от
    // проверки: SAGE_EDITOR_L10N_MISSING=<файл> выгружает их при выходе.
    if (const char* path = std::getenv("SAGE_EDITOR_L10N_MISSING")) {
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

namespace {
// Расширение файла в нижнем регистре, с точкой. Файлы приходят от человека и
// от системы, и «.SAGE» с «.sage» — один и тот же формат.
std::string ToLowerExt(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}
} // namespace

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
void EditorLayer::HandleDroppedFiles() {
    if (m_droppedFiles.empty()) return;
    std::vector<std::string> dropped;
    dropped.swap(m_droppedFiles);

    int imported = 0;
    std::string lastError;
    for (const std::string& raw : dropped) {
        const fs::path path(raw);
        std::error_code ec;

        // Папку не вносим: рекурсивная копия чужого дерева в проект — не то, чего
        // ждут, бросая её на окно, и отменить это нечем.
        if (fs::is_directory(path, ec)) {
            SetStatusMessage(T("A folder cannot be dragged in — drop files"));
            continue;
        }

        const std::string ext = ToLowerExt(path);
        if (ext == ".sageproj") {
            std::string err;
            if (!OpenProject(path.string(), err)) SetStatusMessage(T("Failed to open the project: ") + err);
            continue;
        }
        if (ext == ".sage") {
            // Сцена ОТКРЫВАЕТСЯ. Если она из другого проекта, ссылки внутри неё
            // указывают в тот проект, и об этом честнее сказать сразу.
            if (m_project.Loaded() && m_project.AssetRef(path) == path.generic_string())
                SetStatusMessage(T("This scene is not from this project — assets may not be found"));
            LoadSceneFromFile(path);
            continue;
        }

        if (!m_project.Loaded()) {
            SetStatusMessage(T("Open a project first — there is nowhere to bring the file"));
            continue;
        }

        const AssetsPanel::ImportReport rep = AssetsPanel::ImportAsset(path, m_assetsCwd);
        if (!rep.Ok) {
            lastError = rep.Error;
            continue;
        }
        ++imported;
        m_assets.Select(rep.Created);
        for (const std::string& missing : rep.Missing)
            LOG_WARN("Editor") << "Перетаскивание: спутник не найден — " << missing;
    }

    if (imported == 1) SetStatusMessage(T("Brought into the project: ") + m_assets.Selected().filename().string());
    else if (imported > 1) SetStatusMessage(T("Files brought into the project: ") + std::to_string(imported));
    else if (!lastError.empty()) SetStatusMessage(T("Import failed: ") + lastError);
}

// ============================================================================
//  Play-режим
// ============================================================================

void EditorLayer::StartPlay() {
    if (InPlayMode()) return;

    // Снапшот сцены — Stop вернёт всё ровно как было до Play.
    m_playSnapshot = SceneSerializer::SaveToString(*m_scene);

    m_playScripts = std::make_unique<ScriptEngine>();
    m_playScripts->BindScene(*m_scene);
    // Паритет с рантаймом: частицы доступны скриптам уже в OnStart
    // (EmitParticles/CreateParticleStream рисуются в предпросмотре сцены).
    m_playScripts->BindParticles(m_renderer.Particles());

    // Ввод — как в собранной игре: действия объявляют сами скрипты (BindAction),
    // поэтому карту действий начинаем с ЧИСТОГО ЛИСТА на каждый Play (иначе
    // раскладка прошлого запуска пережила бы правку скрипта), а привязываем ДО
    // AttachScript — OnStart скриптов зовёт BindAction прямо оттуда.
    m_playInput = InputSystem();
    m_playInput.Attach(sage::Application::Get().GetWindow());
    m_playRawInput = std::make_unique<EditorPlayInput>(m_playInput, sage::Application::Get().GetWindow());
    m_playScripts->BindInput(m_playInput.Actions());
    m_playScripts->BindRawInput(*m_playRawInput);

    // Звук — как в собранной игре. Устройство может отсутствовать (headless CI):
    // AudioEngine в этом случае работает вхолостую, но вызовы из Lua валидны.
    if (!m_playAudio) m_playAudio = std::make_unique<AudioEngine>();
    m_playScripts->BindAudio(*m_playAudio);

    // Модули Lua (require "voxel") ищутся в скриптовой папке ОТКРЫТОГО ПРОЕКТА —
    // тот же контракт, что в собранной игре, где CWD и есть корень проекта.
    if (m_project.Loaded())
        m_playScripts->AddScriptSearchPath((m_project.Dir() / "assets" / "scripts").string());
    m_playScripts->AddScriptSearchPath("assets/scripts"); // скрипты рядом с редактором

    // Параметры запуска игры (LaunchArg в Lua) — до AttachScript, потому что
    // OnStart скриптов читает их сразу. В редакторе источник один: окружение
    // (headless-прогон CI ставит SAGE_GAME_ARGS="autopilot=1").
    if (const char* args = std::getenv("SAGE_GAME_ARGS")) m_playScripts->SetLaunchArgsFromString(args);

    // Привязываем скрипты всех сущностей со ScriptComponent. Ошибка в одном
    // скрипте (нет файла, синтаксис) не срывает Play — логируется, остальные
    // продолжают работать.
    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent, IdComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        // Пути скриптов в сцене — ОТНОСИТЕЛЬНО ПРОЕКТА ("assets/scripts/x.lua"):
        // так их резолвит собранная игра (SagePlayer делает chdir в проект). CWD
        // редактора — не папка проекта, поэтому здесь резолвим сами: как есть
        // (скрипты редактора, абсолютные пути), иначе — от корня проекта. Без
        // этого скрипты проекта работали бы в собранной игре, но НЕ в Play.
        std::string resolved = path;
        std::error_code scriptEc;
        if (!fs::exists(resolved, scriptEc) && m_project.Loaded()) {
            fs::path inProject = m_project.Dir() / path;
            if (fs::exists(inProject, scriptEc)) resolved = inProject.string();
        }
        try {
            m_playScripts->AttachScript(GameObject(&m_scene->Registry(), e), resolved);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Editor") << "Play: script attach failed: " << ex.what();
        }
    }

    // Физика: строим мир по сущностям с RigidBodyComponent. Бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный Simple (см. PhysicsWorld::DefaultBackend).
    m_playPhysics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты получают доступ к физике времени выполнения (SetVelocity/GetVelocity/
    // SetGravity) — привязываем ПОСЛЕ построения мира, чтобы RuntimeBody сущностей
    // уже существовали к первому OnUpdate.
    m_playScripts->BindPhysics(*m_playPhysics);

    // Состав кадра на время Play — ТОТ ЖЕ, что у собранной игры (см.
    // PlayerLayer): скрипты, физика, анимация, частицы, звук в порядке,
    // заданном один раз в RegisterCoreSystems.
    //
    // Без этой регистрации Play выглядел запущенным и не был им: AttachScript
    // выше зовёт OnStart (и в консоли честно появляется «spin.lua attached
    // to: …»), но UpdateAll не звал НИКТО — планировщик о скриптах не знал.
    // То есть скрипт «привязывался и ничего не делал», а физика не считала ни
    // одного шага. StopPlay при этом снимал системы "scripts"/"physics",
    // которых никогда не добавляли, — по коду выхода из Play было видно
    // намерение, но входа в него не было.
    {
        sage::CoreSystems core;
        core.Scripts = m_playScripts.get();
        core.Physics = m_playPhysics.get();
        core.Particles = &m_renderer.Particles();
        core.Audio = m_playAudio.get();
        // Анимация уже зарегистрирована набором режима правки (превью) и
        // повторной регистрацией только заменилась бы сама на себя.
        core.Animation = false;
        sage::RegisterCoreSystems(m_systems, core);
    }

    m_playState = EditorPlayState::Playing;
    m_game.RequestFocus(); // «игровое окно» выходит на передний план при запуске
    LOG_INFO("Editor") << "Play started (" << attached << " script(s), "
                       << m_playPhysics->BodyCount() << " physics body(ies) on "
                       << m_playPhysics->BackendName() << ")";
}

void EditorLayer::StopPlay() {
    if (!InPlayMode()) return;

    // Порядок важен: ScriptEngine держит указатель на текущую сцену — гасим
    // его ДО того, как заменить сцену восстановленным снапшотом.
    // Снимаем ДО разрушения объектов: система держит на них указатель, и
    // оставленная в кадре она обратилась бы к освобождённой памяти.
    // Ровно то, что добавил StartPlay. "particles" и "animation" остаются: это
    // превью режима правки, а не игровые системы.
    m_systems.Remove("scripts");
    m_systems.Remove("physics");
    m_systems.Remove("audio");
    m_playScripts.reset();
    m_playPhysics.reset();
    // Курсор возвращается человеку РАНЬШЕ всего остального: игра могла его
    // захватить, и без этого Stop оставил бы редактор без мыши.
    if (m_playRawInput) m_playRawInput->ReleaseCapture();
    m_playRawInput.reset();
    RestoreSceneFromString(m_playSnapshot);
    m_playSnapshot.clear();
    m_playState = EditorPlayState::Editing;
    m_viewport.RequestFocus(); // вернулись к редактированию — Viewport вперёд
    LOG_INFO("Editor") << "Play stopped, scene restored";
}

// ============================================================================
//  Undo/Redo (снапшот-модель) + dirty-маркер
// ============================================================================

bool EditorLayer::RestoreSceneFromString(const std::string& snapshot) {
    try {
        std::unique_ptr<Scene> restored = SceneSerializer::LoadFromString(snapshot);
        // Запечённый GI переезжает указателем: строковый снапшот не тащит
        // страницы лайтмап, а бейк валиден для той же статичной геометрии
        // (Transplant сверяет отпечаток и при несовпадении не переносит).
        if (m_scene && restored) sage::gi::Transplant(*m_scene, *restored);
        m_scene = std::move(restored);
        // Выбор хранится как id, а сериализатор сохраняет id — выбор переживает
        // откат, если сущность существует в снапшоте (иначе Get() даст invalid).
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene restore failed: " << e.what();
        return false;
    }
}

void EditorLayer::PushUndoSnapshot() {
    if (InPlayMode()) return; // правки в Play эфемерны — Stop их и так откатит
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    m_redoStack.clear(); // новая мутация обрывает redo-ветку
    m_sceneDirty = true;
    UpdateWindowTitle();
}

void EditorLayer::CapturePendingSnapshot() {
    if (InPlayMode()) return;
    m_pendingEditSnapshot = SceneSerializer::SaveToString(*m_scene);
}

void EditorLayer::CommitPendingSnapshot() {
    if (InPlayMode() || m_pendingEditSnapshot.empty()) return;
    constexpr size_t kMaxUndoEntries = 100;
    if (m_undoStack.size() >= kMaxUndoEntries) m_undoStack.erase(m_undoStack.begin());
    m_undoStack.push_back(m_pendingEditSnapshot);
    m_pendingEditSnapshot.clear();
    m_redoStack.clear();
    m_sceneDirty = true;
    UpdateWindowTitle();
}

// Одна запись undo на всё перетаскивание DragFloat/набор текста: состояние
// «до» запоминается на активации виджета, в стек уходит на завершении правки.
void EditorLayer::TrackLastImGuiItem() {
    if (InPlayMode()) return;
    if (ImGui::IsItemActivated()) CapturePendingSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitPendingSnapshot();
}

void EditorLayer::Undo() {
    if (InPlayMode() || m_undoStack.empty()) return;
    m_redoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_undoStack.back())) {
        m_undoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_redoStack.pop_back(); // откат не удался — не ломаем историю
    }
}

void EditorLayer::Redo() {
    if (InPlayMode() || m_redoStack.empty()) return;
    m_undoStack.push_back(SceneSerializer::SaveToString(*m_scene));
    if (RestoreSceneFromString(m_redoStack.back())) {
        m_redoStack.pop_back();
        m_sceneDirty = true;
        UpdateWindowTitle();
    } else {
        m_undoStack.pop_back();
    }
}

// ============================================================================
//  Сущности
// ============================================================================


// Готовый элемент интерфейса по имени пресета. Значения подобраны так, чтобы
// созданный элемент был СРАЗУ ВИДЕН и сразу делал то, что обещает названием:
// кнопка ловит мышь, полоса заполнена наполовину, поле ввода имеет подсказку.
// Ноль в размере или прозрачный цвет по умолчанию означали бы, что человек
// создал элемент и не увидел ничего.
GameObject EditorLayer::CreateUIEntity(const std::string& preset) {
    GameObject obj = m_scene->CreateObject(preset);
    UIElementComponent& u = m_scene->Registry().emplace<UIElementComponent>(obj.Entity());
    u.Anchor = UIAnchor::Center;
    u.Offset = glm::vec2(0.0f, 0.0f);

    // Новый элемент становится ДОЧЕРНИМ к выделенному элементу интерфейса.
    //
    // Интерфейс собирается из вложенных прямоугольников: панель, а в ней
    // надпись, кнопка и полоса. Раньше каждый созданный элемент вставал в
    // корень, то есть отсчитывался от края ЭКРАНА, и собрать панель означало
    // создать элементы, а потом перетащить каждый в иерархии на панель, помня,
    // что до этого они лежали не там. Самый частый шаг верстки требовал
    // отдельного ручного действия — и именно это ощущается как «неудобно
    // прикреплять».
    if (m_selectedId >= 0) {
        GameObject sel = m_scene->Get(m_selectedId);
        if (sel.Valid() && m_scene->Registry().all_of<UIElementComponent>(sel.Entity())) {
            m_scene->SetParent(obj.Entity(), sel.Entity());
        }
    }

    // Что именно значит «кнопка» или «полоса», знает ДВИЖОК (sage/ui/UIPresets.h).
    // Раньше это знание жило только здесь, и получить кнопку можно было лишь
    // мышью в редакторе: скрипт, собирающий интерфейс на лету, повторял те же
    // семь присваиваний у себя.
    sage::ui::ApplyPreset(u, preset);

    return obj;
}

GameObject EditorLayer::CreateCubeEntity(const std::string& name) {
    return CreatePrimitiveEntity(name, MeshRef::Type::Cube);
}

GameObject EditorLayer::CreatePrimitiveEntity(const std::string& name, MeshRef::Type type) {
    GameObject obj = m_scene->CreateObject(name);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{type, ""};
    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(type);
    return obj;
}

namespace {
// Копирует компонент T с сущности src на copy, если он есть. Дубликат должен
// нести ВСЕ движковые компоненты — раньше копировались только Script/Camera, и
// дубликат света/физического тела/эмиттера молча терял суть оригинала.
template <typename T>
void CopyComponentIfPresent(GameObject& src, GameObject& copy) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        copy.Registry()->emplace_or_replace<T>(copy.Entity(), *c);
    }
}
} // namespace

namespace {
// Копирование сущностей и поддеревьев переехало в движок (sage/scene/Prefab.h):
// ровно то же самое нужно игре, а жило оно здесь, в безымянном пространстве
// имён редактора, и потому было недоступно никому, кроме него. Здесь остались
// короткие псевдонимы, чтобы не править два десятка мест вызова.
using sage::scene::CopyAllComponents;
using sage::scene::CopySubtree;
} // namespace

// Копирует одну сущность (без детей) со всеми компонентами; сдвиг, чтобы копия
// не сливалась с оригиналом. Возвращает копию.
GameObject EditorLayer::DuplicateEntity(GameObject src) {
    GameObject copy = m_scene->CreateObject(src.Name() + " Copy");
    CopyAllComponents(src, copy);
    copy.GetTransform().Position.x += 0.5f;
    return copy;
}

void EditorLayer::DuplicateSelected() {
    if (m_selection.empty()) return;
    PushUndoSnapshot();
    std::vector<int> copies;
    for (int id : m_selection) {
        GameObject src = m_scene->Get(id);
        if (!src.Valid()) continue;
        entt::entity parent = m_scene->ParentOf(src.Entity()); // копия остаётся у того же родителя
        GameObject copy = DuplicateEntity(src);
        if (parent != entt::null) m_scene->SetParent(copy.Entity(), parent);
        copies.push_back(copy.Id());
    }
    m_selection = copies;
    m_selectedId = copies.empty() ? -1 : copies.back();
}

namespace {
// Сколько сущностей под этой в иерархии. Нужно ровно для одного: сказать
// человеку в вопросе, СКОЛЬКО он на самом деле удаляет.
int CountDescendants(Scene& scene, entt::entity e) {
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(e);
    if (!h) return 0;
    int n = 0;
    for (entt::entity kid : h->Children) {
        if (!scene.Registry().valid(kid)) continue;
        n += 1 + CountDescendants(scene, kid);
    }
    return n;
}
} // namespace

void EditorLayer::DeleteSelected() {
    int count = 0;
    std::string firstName;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        if (count == 0) firstName = o.Name();
        ++count;
    }
    if (count == 0) return;

    // Спрашиваем — и считаем ПОДДЕРЕВО, а не только выделенное: удаление
    // родителя уносит детей, и человек, выделивший одну строку в иерархии,
    // сплошь и рядом не помнит, сколько под ней.
    int withChildren = 0;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        withChildren += 1 + CountDescendants(*m_scene, o.Entity());
    }

    std::string message;
    if (count == 1) {
        message = T("Delete \u00ab") + firstName + "»?";
        if (withChildren > 1)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    } else {
        message = T("Delete the selected objects (") + std::to_string(count) + ")?";
        if (withChildren > count)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    }
    message += T("\nCtrl+Z undoes this.");

    m_confirm.Ask("delete-entity", T("Deleting an object"), message, [this]() {
        PushUndoSnapshot();
        for (int id : m_selection)
            if (m_scene->Get(id).Valid()) m_scene->RemoveObject(id); // удаляет и поддерево
        SetSelectedId(-1);
        m_selection.clear();
    });
}

// «Показать в Assets»: перейти в папку файла, выделить его и открыть панель.
//
// Путь в компоненте относительный (см. Project::AssetRef), а текущая папка
// процесса — не обязательно корень проекта, поэтому сначала превращаем ссылку в
// настоящий путь. Панель открываем принудительно: команда «покажи, где лежит»,
// после которой ничего не появилось (панель была закрыта), выглядит как
// поломка.
void EditorLayer::ShowAssetInPanel(const fs::path& path) {
    if (path.empty()) return;
    fs::path full = path;
    std::error_code ec;
    if (!fs::exists(full, ec) && m_project.Loaded()) full = m_project.Dir() / path;
    if (!fs::exists(full, ec)) {
        SetStatusMessage(T("File not found: ") + path.string());
        return;
    }
    m_showAssets = true;
    m_assetsCwd = full.parent_path();
    m_assets.Select(full);
}

void EditorLayer::SetSelectedId(int id) {
    m_selectedId = id;
    m_selection.clear();
    if (id != -1) m_selection.push_back(id);
}

bool EditorLayer::IsSelected(int id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void EditorLayer::ToggleSelection(int id) {
    if (id == -1) return;
    auto it = std::find(m_selection.begin(), m_selection.end(), id);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        m_selectedId = m_selection.empty() ? -1 : m_selection.back();
    } else {
        m_selection.push_back(id);
        m_selectedId = id; // добавленная становится первичной
    }
}

// ============================================================================
//  Префабы — переиспользуемые сущности-поддеревья (.sageprefab). Формат —
//  та же JSON-сериализация, что у сцен: префаб = мини-сцена с одним корнем.
// ============================================================================
bool EditorLayer::SaveSelectedAsPrefab(const fs::path& path, std::string& err) {
    GameObject root = m_scene->Get(m_selectedId);
    if (!root.Valid()) { err = T("nothing selected"); return false; }
    if (!sage::scene::SavePrefab(*m_scene, root.Entity(), path.string(), err)) return false;
    SetStatusMessage(T("Prefab saved: ") + path.filename().string());
    return true;
}

int EditorLayer::InstantiatePrefab(const fs::path& path) {
    PushUndoSnapshot();
    const int rootId = sage::scene::InstantiatePrefab(*m_scene, path.string());
    if (rootId != -1) SetSelectedId(rootId);
    return rootId;
}

// ============================================================================
//  Сцена / проект
// ============================================================================

void EditorLayer::NewScene(bool withDemoContent) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_undoStack.clear();
    m_redoStack.clear();
    m_scene = std::make_unique<Scene>("Untitled");
    SetSelectedId(-1);
    m_scenePath.clear();
    m_sceneDirty = false;

    if (withDemoContent) {
        // Скайбокс включён по умолчанию — сцена сразу с атмосферным фоном.
        m_scene->Lighting.Skybox.Enabled = true;

        struct Def { const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale; };
        Def defs[] = {
            {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}},
            {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}},
            {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}},
            {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}},
            {"Tower",      {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}},
        };
        for (const Def& d : defs) {
            GameObject obj = CreateCubeEntity(d.name);
            obj.GetTransform().Position = d.pos;
            obj.GetTransform().Scale = d.scale;
            obj.Renderer().Color = d.color;
        }

        // Криволинейные примитивы — витрина форм И проверка аутлайна выделения:
        // кайма строится из СИЛУЭТА реального меша, поэтому одинаково точна для
        // сферы/цилиндра/конуса, а не только для боксов.
        struct Prim { const char* name; MeshRef::Type type; glm::vec3 pos; glm::vec3 color; };
        Prim prims[] = {
            {"Sphere",   MeshRef::Type::Sphere,   {-3.0f, 0.5f, 1.8f}, {0.85f, 0.55f, 0.25f}},
            {"Cylinder", MeshRef::Type::Cylinder, {-1.5f, 0.5f, 2.2f}, {0.55f, 0.35f, 0.80f}},
            {"Cone",     MeshRef::Type::Cone,     {1.5f,  0.5f, 2.2f}, {0.30f, 0.70f, 0.70f}},
        };
        for (const Prim& p : prims) {
            GameObject obj = CreatePrimitiveEntity(p.name, p.type);
            obj.GetTransform().Position = p.pos;
            obj.Renderer().Color = p.color;
        }

        // Игровая камера сцены — панель Game сразу показывает картинку. НАРОЧНО
        // поставлена НЕ как редакторская орбитальная камера ({6.5,5,6.5}, взгляд
        // сверху): низкий, почти фронтальный «кинематографичный» ракурс с уровня
        // сцены — так сразу видно, что панель Game показывает СВОЮ, игровую
        // камеру, а не вид вьюпорта. Сущность без меша (не рисуется в мире).
        GameObject camObj = m_scene->CreateObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.5f, 6.5f};
        camObj.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f}; // чуть вниз, вдоль -Z
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());

        // Солнце — такая же сущность, как всё остальное. Раньше его роль играли
        // три поля в настройках сцены; теперь его видно в иерархии, можно
        // повернуть гизмо и увидеть, как поехали тени.
        GameObject sun = m_scene->CreateObject("Sun");
        sun.GetTransform().Position = {0.0f, 10.0f, 0.0f};
        sun.GetTransform().Rotation =
            sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        LightComponent sunLc;
        sunLc.Kind = LightComponent::Type::Directional;
        sunLc.Color = {1.0f, 0.95f, 0.85f};
        sunLc.Intensity = 1.0f;
        m_scene->Registry().emplace<LightComponent>(sun.Entity(), sunLc);

        // Тёплая лампа — демонстрация точечного света-сущности (LightComponent).
        GameObject lamp = m_scene->CreateObject("Lamp");
        lamp.GetTransform().Position = {2.4f, 1.6f, 1.8f};
        m_scene->Registry().emplace<LightComponent>(lamp.Entity());

        // Прожектор сверху — демонстрация конусного света (Spot): смотрит вниз
        // (поворот -90° по X направляет «вперёд» -Z в -Y), кладёт круг света
        // на кубы и пол.
        GameObject spot = m_scene->CreateObject("Spotlight");
        spot.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        spot.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f};
        LightComponent spotLc;
        spotLc.Kind = LightComponent::Type::Spot;
        spotLc.Color = {0.55f, 0.7f, 1.0f};
        spotLc.Intensity = 4.0f;
        spotLc.Range = 12.0f;
        spotLc.InnerConeDeg = 18.0f;
        spotLc.OuterConeDeg = 30.0f;
        m_scene->Registry().emplace<LightComponent>(spot.Entity(), spotLc);

        // Демо-худ (UIElementComponent): панель со скруглением и рамкой + полоса
        // здоровья ребёнком — показывает UI-систему сразу в панели Game и служит
        // стартовой точкой для своего интерфейса (правится в Inspector).
        GameObject hud = m_scene->CreateObject("HUD Panel");
        UIElementComponent hudUi;
        hudUi.Type = UIElementComponent::Kind::Panel;
        hudUi.Anchor = UIAnchor::TopLeft;
        hudUi.Offset = {16.0f, 16.0f};
        hudUi.Size = {230.0f, 64.0f};
        hudUi.Rounding = 12.0f;
        hudUi.BorderThickness = 2.0f;
        hudUi.Text = "SAGE UI";
        hudUi.TextCentered = false;
        m_scene->Registry().emplace<UIElementComponent>(hud.Entity(), hudUi);

        GameObject hp = m_scene->CreateObject("HP Bar");
        UIElementComponent hpUi;
        hpUi.Type = UIElementComponent::Kind::Bar;
        hpUi.Anchor = UIAnchor::BottomLeft;   // внутри панели-родителя
        hpUi.Offset = {12.0f, 8.0f};
        hpUi.Size = {206.0f, 18.0f};
        hpUi.Rounding = 8.0f;
        hpUi.Color = {0.0f, 0.0f, 0.0f, 0.55f};
        hpUi.Value = 0.72f;
        hpUi.BarFillColor = {0.85f, 0.30f, 0.30f, 1.0f};
        m_scene->Registry().emplace<UIElementComponent>(hp.Entity(), hpUi);
        m_scene->SetParent(hp.Entity(), hud.Entity());

        // Что-то выбрано сразу — гизмо видно, Inspector не пустой. Выбираем
        // криволинейный примитив: сразу демонстрирует аутлайн на изогнутом
        // силуэте (кайма строится из силуэта меша — точна для любой формы).
        GameObject sel = m_scene->FindByName("Cone");
        if (!sel.Valid()) sel = m_scene->FindByName("Green Cube");
        if (sel.Valid()) SetSelectedId(sel.Id());
    }
    UpdateWindowTitle();
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_undoStack.clear();
        m_redoStack.clear();
        SetSelectedId(-1);
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene loaded: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene load failed: " << e.what();
        return false;
    }
}

// Предложение восстановить сцену после падения или из автосохранения.
//
// Модалка, а не тихая загрузка: восстановленная сцена может быть НЕ той, над
// которой человек работал последней (например, он с тех пор открыл другой
// проект), и решать это должен он, а не редактор.
void EditorLayer::DrawRecoveryPrompt() {
    if (!m_recoveryPrompt) return;
    ImGui::OpenPopup(T("Restore scene?"));
    if (ImGui::BeginPopupModal(T("Restore scene?" "###Restore scene?"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("The previous session seems to have crashed"));
        ImGui::Spacing();
        ImGui::Text(T("Found file: %s"), m_recoveryFile.c_str());
        ImGui::TextDisabled("%s", T("It did not overwrite your scene — this is a separate copy."));
        ImGui::Spacing();
        if (ImGui::Button(T("Open the copy"), ImVec2(150, 0))) {
            if (LoadSceneFromFile(m_recoveryFile)) {
                // Путь сцены НЕ ставим: иначе первое же Ctrl+S записало бы
                // восстановленное поверх файла восстановления, а не сцены.
                m_scenePath.clear();
                m_sceneDirty = true;
                UpdateWindowTitle();
            }
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Keep the file"), ImVec2(150, 0))) {
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Delete"), ImVec2(110, 0))) {
            std::error_code ec;
            fs::remove(m_recoveryFile, ec);
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool EditorLayer::SaveSceneToFile(const fs::path& path) {
    try {
        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        SceneSerializer::Save(*m_scene, path.string());
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene saved: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene save failed: " << e.what();
        return false;
    }
}

bool EditorLayer::CreateProject(const std::string& dir, const std::string& name, std::string& err) {
    if (!m_project.CreateNew(dir, name, err)) return false;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());
    NewScene(/*withDemoContent=*/true);
    UpdateWindowTitle();
    return true;
}

bool EditorLayer::OpenProject(const std::string& path, std::string& err) {
    if (!m_project.Open(path, err)) return false;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());

    // Настройки проекта (sage.cfg) — в окно Settings; отсутствие файла не ошибка
    // (остаются значения по умолчанию).
    m_settings = sage::EngineConfig{};
    m_settings.LoadFile((m_project.Dir() / "sage.cfg").string());
    m_settings.ApplyEnvOverrides();   // SAGE_* поверх файла — как у рантайма
    ApplyEngineSettings();

    // Автозагрузка первой сцены проекта (по алфавиту) — открытый проект сразу
    // показывает свой контент, а не осиротевшую демо-сцену.
    std::error_code ec;
    std::vector<fs::path> scenes;
    for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
        if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
    }
    std::sort(scenes.begin(), scenes.end());
    if (!scenes.empty()) LoadSceneFromFile(scenes.front());

    UpdateWindowTitle();
    return true;
}

// ============================================================================
//  Сборка игры: SagePlayer + рантайм-ассеты + project/ => запускаемая папка
// ============================================================================

bool EditorLayer::BuildGame(const fs::path& outputDir, std::string& err) {
    if (!m_project.Loaded()) {
        err = "No project open";
        return false;
    }

    // 1. Собранный SagePlayer: явный SAGE_PLAYER_PATH, иначе стандартные
    // места относительно редактора (../runtime в дереве сборки, рядом с exe).
#ifdef _WIN32
    const char* playerName = "SagePlayer.exe";
    std::string exeSuffix = ".exe";
#else
    const char* playerName = "SagePlayer";
    std::string exeSuffix;
#endif
    std::vector<fs::path> candidates;
    if (const char* p = std::getenv("SAGE_PLAYER_PATH")) candidates.push_back(p);
    candidates.push_back(fs::path("..") / "runtime" / playerName);
    candidates.push_back(fs::path(".") / playerName);

    std::error_code ec;
    fs::path player;
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate, ec)) { player = candidate; break; }
    }
    if (player.empty()) {
        err = "SagePlayer not found (build the SagePlayer target or set SAGE_PLAYER_PATH)";
        return false;
    }

    // 2. Слепить папку игры: <out>/<Name>/{<Name>, assets/(рантайм), project/}.
    fs::path gameDir = outputDir / m_project.Name();
    fs::remove_all(gameDir, ec); // пересборка затирает прошлую (это артефакт, не данные)
    fs::create_directories(gameDir, ec);
    if (ec) {
        err = "Cannot create " + gameDir.string() + ": " + ec.message();
        return false;
    }

    fs::copy_file(player, gameDir / (m_project.Name() + exeSuffix),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "Player copy failed: " + ec.message();
        return false;
    }
    fs::copy(player.parent_path() / "assets", gameDir / "assets",
             fs::copy_options::recursive, ec);
    if (ec) {
        err = "Runtime assets copy failed: " + ec.message();
        return false;
    }
    // Проект едет в игру ПАКЕТОМ (game.sagepak), а не россыпью файлов.
    //
    // Копирование папки как есть означало три вещи сразу: медленный старт
    // (тысяча мелких файлов открывается дольше одного большого), игру, которую
    // открывают блокнотом (исходные .lua и .sage лежат рядом с exe), и лишний
    // размер (текстовые сцены и скрипты жмутся в разы).
    //
    // Файлы .meta и .sageimport в пакет не кладутся: это служебные данные
    // редактора (GUID'ы ассетов, параметры импорта), в игре по ним никто не
    // ходит, а место они занимают.
    {
        sage::assets::PackWriter pack;
        // project.sageproj в пакет НЕ кладётся, а копируется рядом с exe. Это
        // манифест игры: по нему плеер узнаёт её имя, и на него указывают,
        // когда запускают игру из другой папки или перетаскивают файл на
        // плеер. Спрятав его в пакет, мы отняли бы этот способ запуска — что и
        // случилось, и поймал это smoke-тест «игра запускается из ЛЮБОЙ папки».
        //
        // Копия одна, а не две: файла нет в пакете, поэтому правила «пакет
        // против россыпи» он не нарушает.
        const size_t packed =
            pack.AddDirectory(m_project.Dir(), {".meta", ".sageimport", "project.sageproj"});
        if (!pack.Save(gameDir / "game.sagepak")) {
            err = T("Could not write the game package");
            return false;
        }
        fs::copy_file(m_project.Dir() / "project.sageproj", gameDir / "project.sageproj",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = T("Could not copy the project file: ") + ec.message();
            return false;
        }
        LOG_INFO("Editor") << "Пакет игры: " << packed << " файлов";
    }

    // Настройки проекта — рядом с exe игры (sage.cfg), чтобы игрок мог править их
    // без залезания в project/. SagePlayer грузит и этот, и project/sage.cfg.
    std::error_code cfgEc;
    fs::path projCfg = m_project.Dir() / "sage.cfg";
    if (fs::exists(projCfg, cfgEc)) {
        fs::copy_file(projCfg, gameDir / "sage.cfg", fs::copy_options::overwrite_existing, cfgEc);
    }

    LOG_INFO("Editor") << "Game built: " << gameDir.string();
    return true;
}

// Заголовок OS-окна: "SAGE Editor — сцена[*] — проект". Обновляется только
// по факту изменения (не дёргаем GLFW каждый кадр).
void EditorLayer::UpdateWindowTitle() {
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    std::string title = "SAGE Editor — " + scene + (m_sceneDirty ? "*" : "");
    if (m_project.Loaded()) title += " — " + m_project.Name();
    if (title == m_windowTitle) return;
    m_windowTitle = title;
    glfwSetWindowTitle(sage::Application::Get().GetWindow().Handle(), title.c_str());
}

// ============================================================================
//  Пикинг из вьюпорта
// ============================================================================

void EditorLayer::PickAtViewport(float u, float v, bool additive) {
    PickAtViewportWith(m_view, m_proj, u, v, additive);
}

bool EditorLayer::ApplyAssetToEntity(int entityId, const fs::path& asset) {
    GameObject obj = m_scene->Get(entityId);
    if (!obj.Valid()) return false;

    const std::string ext = ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    if (ext == ".sagemat") {
        PushUndoSnapshot();
        MeshRendererComponent& mr = obj.Renderer();
        mr.MaterialPath = ref;
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(ref);
        SetStatusMessage(T("Material assigned: ") + asset.filename().string());
        return true;
    }
    if (ext == ".lua") {
        PushUndoSnapshot();
        m_scene->Registry().emplace_or_replace<ScriptComponent>(obj.Entity(), ScriptComponent{ref});
        SetStatusMessage(T("Script assigned: ") + asset.filename().string());
        return true;
    }
    if (ModelLoader::IsSupportedModel(ext)) {
        std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(ref);
        if (!mesh) {
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        PushUndoSnapshot();
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref.type = MeshRef::Type::Model;
        mr.Ref.path = ref;
        mr.MeshPtr = std::move(mesh);
        SetStatusMessage(T("Mesh replaced: ") + asset.filename().string());
        return true;
    }
    // Префаб на сущность НЕ применяется: он сам себе поддерево, и «применить» его
    // к чужой сущности значило бы её заменить. Ставится он в сцену — броском во
    // вьюпорт или в список.
    SetStatusMessage(T("This file cannot be assigned to an object"));
    return false;
}

bool EditorLayer::AddAssetToScene(const fs::path& asset) {
    const std::string ext = ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    int newId = -1;
    if (ext == ".sageprefab") {
        PushUndoSnapshot();
        newId = sage::scene::InstantiatePrefab(*m_scene, ref);
        if (newId < 0) {
            SetStatusMessage(T("The prefab could not be placed: ") + asset.filename().string());
            return true;
        }
    } else if (ModelLoader::IsSupportedModel(ext)) {
        std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(ref);
        if (!mesh) {
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateObject(asset.stem().string());
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref.type = MeshRef::Type::Model;
        mr.Ref.path = ref;
        mr.MeshPtr = std::move(mesh);
        newId = obj.Id();
    } else {
        return false;
    }

    SetSelectedId(newId);
    m_selection = {newId};
    m_sceneDirty = true;
    UpdateWindowTitle();
    SetStatusMessage(T("Added to the scene: ") + asset.filename().string());
    return true;
}

bool EditorLayer::DropAssetAtViewport(const glm::mat4& view, const glm::mat4& proj, float u,
                                      float v, const fs::path& asset) {
    const std::string ext = ToLowerExt(asset);
    const std::string ref = m_project.AssetRef(asset);

    const bool isModel = ModelLoader::IsSupportedModel(ext);
    const bool isPrefab = ext == ".sageprefab";
    const bool isMaterial = ext == ".sagemat";
    if (!isModel && !isPrefab && !isMaterial) return false;

    // Луч через точку, где отпустили кнопку. Та же математика, что у выбора
    // мышью (см. PickAtViewportWith), и это важно: место, куда встанет объект,
    // обязано совпадать с тем, по чему бы кликнули.
    const glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    const glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    const glm::vec3 ro = glm::vec3(p0) / p0.w;
    const glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    // Ближайшая поверхность под курсором: и точка постановки, и объект, которому
    // достанется материал.
    float bestT = 1e30f;
    entt::entity bestEntity = entt::null;
    auto meshes = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : meshes) {
        Mesh* mesh = meshes.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        const glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
        const glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        const glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f));
        const sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
        if (hit.Hit && hit.Distance < bestT) {
            bestT = hit.Distance;
            bestEntity = e;
        }
    }

    if (isMaterial) {
        // Материал ложится на то, НА ЧТО его уронили. В пустоту ронять его
        // бессмысленно — там нечего красить, и создавать ради этого объект было
        // бы сюрпризом.
        if (bestEntity == entt::null) {
            SetStatusMessage(T("Nowhere to drop the material — no object under the cursor"));
            return true;
        }
        PushUndoSnapshot();
        MeshRendererComponent& mr = m_scene->Registry().get<MeshRendererComponent>(bestEntity);
        mr.MaterialPath = ref;
        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(ref);
        const int id = m_scene->Registry().get<IdComponent>(bestEntity).Id;
        SetSelectedId(id);
        m_selection = {id};
        SetStatusMessage(T("Material assigned: ") + asset.filename().string());
        return true;
    }

    // Точка постановки: поверхность под курсором, а если её нет — точка на луче
    // в паре метров от камеры. Ронять в бесконечность нельзя, а «в начало
    // координат» означало бы, что объект исчез из виду.
    const bool onSurface = bestEntity != entt::null;
    const glm::vec3 point = onSurface ? (ro + rd * bestT) : (ro + rd * 8.0f);

    PushUndoSnapshot();
    int newId = -1;
    if (isPrefab) {
        newId = sage::scene::InstantiatePrefabAt(*m_scene, ref, point);
        if (newId < 0) {
            SetStatusMessage(T("The prefab could not be placed: ") + asset.filename().string());
            return true;
        }
    } else {
        GameObject obj = m_scene->CreateObject(asset.stem().string());
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref.type = MeshRef::Type::Model;
        mr.Ref.path = ref;
        mr.MeshPtr = ResourceManager::Instance().GetModel(ref);
        if (!mr.MeshPtr) {
            m_scene->RemoveObject(obj.Id());
            SetStatusMessage(T("The model failed to load: ") + asset.filename().string() +
                             T(" — details in Console"));
            return true;
        }
        obj.GetTransform().Position = point;
        newId = obj.Id();

        // Ставим НА поверхность, а не центром в точку попадания: иначе половина
        // модели уходит под пол, и первое, что приходится делать после
        // перетаскивания, — поднимать её вручную.
        if (onSurface) {
            const glm::vec3 bmin = mr.MeshPtr->BoundsMin();
            obj.GetTransform().Position.y -= bmin.y * obj.GetTransform().Scale.y;
        }
    }

    SetSelectedId(newId);
    m_selection = {newId};
    m_sceneDirty = true;
    UpdateWindowTitle();
    SetStatusMessage(T("Placed in the scene: ") + asset.filename().string());
    return true;
}

void EditorLayer::PickAtViewportWith(const glm::mat4& view, const glm::mat4& proj, float u, float v,
                                     bool additive) {
    // Луч из камеры через пиксель вьюпорта: unprojection ближней/дальней точек NDC.
    // Для ортогональной проекции это работает ровно так же: обе точки уходят в
    // одну сторону, просто луч получается параллельным, а не расходящимся.
    glm::vec2 ndc(u * 2.0f - 1.0f, 1.0f - v * 2.0f);
    glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 p0 = invVP * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 p1 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    glm::vec3 ro = glm::vec3(p0) / p0.w;
    glm::vec3 rd = glm::normalize(glm::vec3(p1) / p1.w - ro);

    int bestId = -1;
    float bestDist = 1e30f;
    bool bestExact = false;
    auto meshes = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
    for (auto e : meshes) {
        Mesh* mesh = meshes.get<MeshRendererComponent>(e).MeshPtr.get();
        if (!mesh) continue;
        // МИРОВАЯ матрица (учёт иерархии родителей): раньше бралась локальная —
        // дочерние сущности выделялись по неверной позиции.
        glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
        glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f)); // без нормализации: t остаётся в масштабе мира

        sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
        if (!hit.Hit) continue;

        // Точное попадание (по треугольнику) бьёт приблизительное (по коробке)
        // ДАЖЕ ЕСЛИ ОНО ДАЛЬШЕ. Иначе объект без копии геометрии перехватывал бы
        // выбор у соседа просто потому, что его коробка начинается раньше, —
        // а именно так пол и перекрывал всё, что на нём стоит.
        const bool better = (hit.Exact && !bestExact) ||
                            (hit.Exact == bestExact && hit.Distance < bestDist);
        if (!better) continue;
        bestDist = hit.Distance;
        bestExact = hit.Exact;
        bestId = meshes.get<IdComponent>(e).Id;
    }

    // Невидимые сущности (камера/свет) кликабельны по маленькому боксу вокруг
    // их позиции — иначе их гизмо не выбрать (меша нет).
    auto pickMarker = [&](entt::entity e, int id, const glm::vec3& pos) {
        glm::mat4 boxInv = glm::inverse(glm::translate(glm::mat4(1.0f), pos) *
                                        glm::scale(glm::mat4(1.0f), glm::vec3(0.6f)));
        glm::vec3 lro = glm::vec3(boxInv * glm::vec4(ro, 1.0f));
        glm::vec3 lrd = glm::vec3(boxInv * glm::vec4(rd, 0.0f));
        float t = RayUnitCube(lro, lrd);
        // Маркер считается ТОЧНЫМ попаданием: у света и камеры нет геометрии,
        // этот кубик и есть их единственное видимое тело, и промахнуться по
        // нему нельзя — он ровно там, где нарисован.
        if (t < 0.0f) return;
        const bool better = !bestExact || t < bestDist;
        if (!better) return;
        bestDist = t;
        bestExact = true;
        bestId = id;
    };
    auto camMarkers = m_scene->Registry().view<CameraComponent, Transform, IdComponent>();
    for (auto e : camMarkers)
        pickMarker(e, camMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));
    auto lightMarkers = m_scene->Registry().view<LightComponent, Transform, IdComponent>();
    for (auto e : lightMarkers)
        pickMarker(e, lightMarkers.get<IdComponent>(e).Id, glm::vec3(m_scene->WorldMatrix(e)[3]));

    // Ctrl-клик (additive): добавить/убрать попадание из набора (клик по пустоте
    // ничего не меняет). Обычный клик: одиночный выбор (мимо всех — снять).
    if (additive) {
        if (bestId != -1) ToggleSelection(bestId);
    } else {
        SetSelectedId(bestId);
    }
}

// ============================================================================
//  Инструменты над выделением
// ============================================================================

float EditorLayer::SnapStepForCurrentOp() {
    switch ((ImGuizmo::OPERATION)m_gizmoOp) {
        case ImGuizmo::ROTATE: return m_snapRotate;
        case ImGuizmo::SCALE:  return m_snapScale;
        default:               return m_snapMove;
    }
}

namespace {

// Мировой AABB одной сущности: восемь углов локальной коробки через мировую
// матрицу. Не «центр ± радиус»: при повороте коробка перестаёт быть выровненной
// по осям, и охватывающая её мировая коробка строится только по углам.
bool EntityWorldBounds(Scene& scene, entt::entity e, glm::vec3& lo, glm::vec3& hi) {
    const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(e);
    if (!mr || !mr->MeshPtr) return false;
    const glm::vec3 bmin = mr->MeshPtr->BoundsMin();
    const glm::vec3 bmax = mr->MeshPtr->BoundsMax();
    const glm::mat4 world = scene.WorldMatrix(e);
    bool any = false;
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                               (c & 4) ? bmax.z : bmin.z);
        const glm::vec3 w = glm::vec3(world * glm::vec4(corner, 1.0f));
        lo = any ? glm::min(lo, w) : w;
        hi = any ? glm::max(hi, w) : w;
        any = true;
    }
    return any;
}

} // namespace

bool EditorLayer::SelectionBounds(glm::vec3& outMin, glm::vec3& outMax) {
    bool any = false;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;
        outMin = any ? glm::min(outMin, lo) : lo;
        outMax = any ? glm::max(outMax, hi) : hi;
        any = true;
    }
    return any;
}

void EditorLayer::FocusSelected() {
    glm::vec3 lo, hi;
    glm::vec3 target;
    float radius = 1.0f;
    if (SelectionBounds(lo, hi)) {
        target = (lo + hi) * 0.5f;
        radius = std::max(glm::length(hi - lo) * 0.5f, 0.1f);
    } else {
        // Выделено что-то без геометрии (свет, камера, пустышка) — подводим
        // камеру к его позиции: маркер всё равно нарисован, и добраться до него
        // человек хочет ровно так же.
        GameObject o = SelectedObject();
        if (!o.Valid()) return;
        target = glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    }

    // Расстояние — из вертикального угла обзора, чтобы объект занял кадр
    // примерно на 70%: впритык он упирался бы в края, а «с запасом» съедало бы
    // смысл операции.
    const float fov = glm::radians(std::max(m_camera.Fov, 10.0f));
    const float dist = std::max(radius / std::tan(fov * 0.5f) / 0.7f, radius + 0.5f);
    m_camera.Position = target - m_camera.Front * dist;
}

void EditorLayer::DropSelectedToSurface() {
    if (m_selection.empty()) return;
    PushUndoSnapshot();

    int moved = 0;
    for (int id : m_selection) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;

        // Луч вниз из центра НИЖНЕЙ грани: из центра объекта он сначала прошёл
        // бы сквозь него самого, а из угла — промахнулся бы мимо опоры.
        const glm::vec3 bottom((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
        const glm::vec3 ro = bottom + glm::vec3(0.0f, 0.001f, 0.0f);
        const glm::vec3 rd(0.0f, -1.0f, 0.0f);

        float bestT = 1e30f;
        bool found = false;
        auto view = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
        for (auto e : view) {
            // Себя и других выделенных пропускаем: они едут вместе с этим, и
            // опираться на них значило бы ставить объект сам на себя.
            if (IsSelected(view.get<IdComponent>(e).Id)) continue;
            Mesh* mesh = view.get<MeshRendererComponent>(e).MeshPtr.get();
            if (!mesh) continue;
            const glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
            const glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
            const glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f));
            sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
            if (hit.Hit && hit.Distance < bestT) {
                bestT = hit.Distance;
                found = true;
            }
        }
        if (!found) continue;

        // Двигаем на дельту в МИРЕ, а потом переводим в локальные координаты:
        // у сущности с родителем прибавление к Transform.Position означало бы
        // смещение в системе родителя, то есть не туда.
        const float dropBy = bestT;
        Transform& tr = o.GetTransform();
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        if (parent == entt::null) {
            tr.Position.y -= dropBy;
        } else {
            glm::mat4 world = m_scene->WorldMatrix(o.Entity());
            world[3].y -= dropBy;
            const glm::mat4 local = glm::inverse(m_scene->WorldMatrix(parent)) * world;
            tr.Position = glm::vec3(local[3]);
        }
        ++moved;
    }
    SetStatusMessage(moved ? (T("Dropped onto the surface: ") + std::to_string(moved))
                           : T("There is no surface under the selection"));
}

void EditorLayer::AlignSelection(int axis) {
    if (m_selection.size() < 2 || axis < 0 || axis > 2) return;
    GameObject primary = SelectedObject();
    if (!primary.Valid()) return;
    PushUndoSnapshot();

    // Эталон — первичная сущность (та, вокруг которой стоит гизмо): выравнивать
    // «по среднему» бессмысленно, человек всегда равняет ПО ЧЕМУ-ТО.
    const float target = m_scene->WorldMatrix(primary.Entity())[3][axis];
    for (int id : m_selection) {
        if (id == primary.Id()) continue;
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::mat4 world = m_scene->WorldMatrix(o.Entity());
        world[3][axis] = target;
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        const glm::mat4 local =
            (parent == entt::null) ? world : glm::inverse(m_scene->WorldMatrix(parent)) * world;
        o.GetTransform().Position = glm::vec3(local[3]);
    }
    SetStatusMessage(std::string(T("Aligned along axis ")) + "XYZ"[axis]);
}

bool EditorLayer::HasPrimaryCamera() {
    auto view = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) return true;
    }
    return false;
}

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
    m_renderer.SetDrawUIOverlay(m_uiEditMode);

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

    app.Device().SetViewport(0, 0, app.GetWindow().Width(), app.GetWindow().Height());
    app.Device().SetClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    app.Device().Clear();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    DrawDockspaceAndMenu(); // включая модалки (m_dialogs) и окно настроек (m_settingsPanel)
    // Панель подаётся в кадр только когда она открыта: закрытая вкладка иначе
    // возвращалась бы сама собой на следующем кадре, и крестик не работал бы.
    if (m_showHierarchy) m_hierarchy.Draw(*this, &m_showHierarchy);
    if (m_showInspector) m_inspector.Draw(*this, &m_showInspector);
    if (m_showLighting) m_lighting.Draw(*this, &m_showLighting);
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

    // Стартовый launcher проектов: пока проект не открыт (и не отклонён).
    if ((!m_project.Loaded() && !m_launcher.Dismissed()) || m_launcherRequested) {
        m_launcher.Draw(*this, m_recent);
        if (m_project.Loaded()) m_launcherRequested = false;
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Multi-viewport: панели, вытащенные за пределы главного окна, живут в
    // собственных OS-окнах — их нужно обновить и отрисовать отдельно.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        Window& win = app.GetWindow();
        SaveScreenshot(m_screenshotPath, win.Width(), win.Height());
        app.Close();
    }
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

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Lighting", right);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    // Viewport докается ПЕРВЫМ в центральный узел — так он и есть таб по
    // умолчанию (первый добавленный к узлу становится выбранным). Раньше первым
    // шёл Game, из-за чего редактор открывался на «игровом окне» без пикинга/
    // гизмо/аутлайна — выглядело как «выделение не работает». Game выходит
    // вперёд при входе в Play (GamePanel::RequestFocus).
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Game", center);
    // Код — третьей вкладкой рядом с Viewport и Game, а не отдельным плавающим
    // окном. Правка скрипта и проверка результата — это одно занятие, и
    // «редактор кода поверх сцены» заставлял таскать окно с места на место
    // ровно так же, как раньше заставлял alt-tab во внешний редактор.
    ImGui::DockBuilderDockWindow("Code", center);
    ImGui::DockBuilderFinish(dockspaceId);

    ImGui::SetWindowFocus("Viewport");
}

// Help > About SAGE — версия движка + таблица версий ВСЕХ подсистем (пока все
// v1). Единый источник — sage::EngineSystems() (тот же список, что в лог старта).
void EditorLayer::DrawAboutWindow() {
    if (!m_showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("About SAGE" "###About SAGE"), &m_showAbout)) { ImGui::End(); return; }

    ImGui::Text(T("SAGE Engine %s"), kSageEngineVersion);
    ImGui::TextDisabled("%s", T("A modular 3D engine: ECS, RHI, PBR, physics, scripting, UI."));
    ImGui::Spacing();
    const auto& systems = sage::EngineSystems();
    ImGui::Text(T("Subsystems: %zu (all v1)"), systems.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##systems", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const sage::SystemVersion& s : systems) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.Name);
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", s.Tag().c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.Summary);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void EditorLayer::DrawStatusBar(float height) {
    // Строка состояния внизу хост-окна: проект | сцена(+dirty) | сущности |
    // Play-статус | сообщение плагинов | FPS.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
    ImGui::BeginChild("##statusbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();

    ImGui::TextDisabled("%s", m_project.Loaded() ? m_project.Name().c_str() : T("No project"));
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    ImGui::Text("%s%s", scene.c_str(), m_sceneDirty ? "*" : "");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::TextDisabled(T("Entities: %zu"), m_scene->Count());

    // Ошибки и предупреждения — В СТАТУСНОЙ СТРОКЕ, а не только в консоли.
    // Консоль легко держать свёрнутой, и тогда единственное предупреждение о
    // непрочитанном шейдере остаётся незамеченным, а сцена «просто выглядит
    // не так». Клик открывает консоль уже с нужным фильтром.
    if (m_console.ErrorCount() > 0 || m_console.WarnCount() > 0) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool hasErrors = m_console.ErrorCount() > 0;
        const ImVec4 col = hasErrors ? ImVec4(0.95f, 0.40f, 0.40f, 1.0f)
                                     : ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
        EditorIcons::Inline(hasErrors ? "error" : "warn", glm::vec3(col.x, col.y, col.z));
        ImGui::SameLine();
        // Склонение по-русски: 1 ошибка, 2 ошибки, 5 ошибок. Мелочь, но
        // «1 ошибок» в статусной строке читается как недоделка интерфейса.
        auto plural = [](int n, const char* one, const char* few, const char* many) {
            const int n100 = n % 100, n10 = n % 10;
            if (n100 >= 11 && n100 <= 14) return many;
            if (n10 == 1) return one;
            if (n10 >= 2 && n10 <= 4) return few;
            return many;
        };
        ImGui::TextColored(col, "%d %s, %d %s", m_console.ErrorCount(),
                           plural(m_console.ErrorCount(), T("error"), T("errors"), T("errors (many)")),
                           m_console.WarnCount(),
                           plural(m_console.WarnCount(), T("warning"), T("warnings"),
                                  T("warnings (many)")));
        if (ImGui::IsItemClicked()) ImGui::SetWindowFocus("Console");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Open console"));
    }

    if (InPlayMode()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        bool playing = m_playState == EditorPlayState::Playing;
        ImGui::TextColored(playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           playing ? "PLAYING" : "PAUSED");
    }
    if (!m_pluginStatusMessage.empty()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::TextDisabled("%s", m_pluginStatusMessage.c_str());
    }

    // Статистика рендера (отсечение/батчинг) + FPS — справа.
    char stat[128];
    const sage::ecs::RenderStats& rs = m_renderer.LastStats();
    std::snprintf(stat, sizeof(stat), "meshes %d/%d  culled %d  batches %d  |  %.0f FPS",
                  rs.Drawn, rs.Total, rs.Culled, rs.Batches, sage::Application::Get().Fps());
    float w = ImGui::CalcTextSize(stat).x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextDisabled("%s", stat);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool EditorLayer::AnyPanelVisible() const {
    return m_showHierarchy || m_showInspector || m_showLighting || m_showViewport || m_showGame ||
           m_showConsole || m_showAssets || m_showCode || m_showProfiler;
}

void EditorLayer::ShowAllPanels() {
    m_showHierarchy = m_showInspector = m_showLighting = true;
    m_showViewport = m_showGame = m_showConsole = m_showAssets = true;
    m_showCode = true;
    // Профайлер сюда НЕ входит: он служебный и по умолчанию закрыт, а «вернуть
    // панели» не должно означать «открыть то, чего человек не открывал».
}

void EditorLayer::DrawEmptyDockHint(float minX, float minY, float maxX, float maxY) {
    const char* title = T("All panels are closed");
    const char* body = T("Windows menu returns any panel, or restore the default layout.");
    const char* action = T("Restore panels");

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const ImVec2 bodySize = ImGui::CalcTextSize(body);
    const float buttonHeight = ImGui::GetFrameHeight();
    const float buttonWidth = ImGui::CalcTextSize(action).x + style.FramePadding.x * 6.0f;
    const float blockWidth = std::max(std::max(titleSize.x, bodySize.x), buttonWidth);
    const float blockHeight =
        titleSize.y + bodySize.y + buttonHeight + style.ItemSpacing.y * 4.0f;

    // Окно ровно по размеру подсказки и по центру пустого дока: полноэкранное
    // прозрачное окно перехватывало бы клики по всему редактору.
    ImGui::SetNextWindowPos(ImVec2((minX + maxX) * 0.5f, (minY + maxY) * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(blockWidth + style.WindowPadding.x * 2.0f,
                                    blockHeight + style.WindowPadding.y * 2.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##SageEmptyDockHint", nullptr, flags)) {
        ImGui::End();
        return;
    }

    auto centered = [blockWidth](float width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (blockWidth - width) * 0.5f);
    };
    centered(titleSize.x);
    ImGui::TextDisabled("%s", title);
    centered(bodySize.x);
    ImGui::TextDisabled("%s", body);
    ImGui::Spacing();
    centered(buttonWidth);
    if (ImGui::Button(action, ImVec2(buttonWidth, buttonHeight))) {
        ShowAllPanels();
        m_rebuildDockLayout = true; // панель могла быть закрыта вместе со своим узлом
    }
    ImGui::End();
}

void EditorLayer::DrawDockspaceAndMenu() {
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
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##SageEditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Тулбар — горизонтальный бар сразу под меню-баром (инструменты гизмо,
    // Play, режим рендера). Рисуется ДО dockspace, чтобы занять свою полосу.
    m_toolbar.Draw(*this, kToolbarHeight);

    ImGuiID dockspaceId = ImGui::GetID("SageDockSpace");
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        BuildDefaultDockLayout(dockspaceId);
    }
    // Док-пространство занимает всё между тулбаром и статус-баром.
    const ImVec2 dockMin = ImGui::GetCursorScreenPos();
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -kStatusBarHeight), ImGuiDockNodeFlags_None);
    const ImVec2 dockMax = ImGui::GetItemRectMax();
    DrawStatusBar(kStatusBarHeight);

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(T("File"))) {
            if (ImGui::MenuItem(T("New Project..."))) openDialog = "New Project";
            if (ImGui::MenuItem(T("Open Project..."))) openDialog = "Open Project";
            if (ImGui::MenuItem(T("Project Launcher..."))) m_launcherRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem(T("New Scene"))) NewScene(false);
            if (ImGui::MenuItem(T("Open Scene..."))) openDialog = "Open Scene";

            // Сцены открытого проекта — прямой доступ без файлового диалога.
            if (m_project.Loaded() && ImGui::BeginMenu(T("Project Scenes"))) {
                std::error_code ec;
                std::vector<fs::path> scenes;
                for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
                    if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
                }
                std::sort(scenes.begin(), scenes.end());
                if (scenes.empty()) ImGui::TextDisabled("%s", T("(no scenes yet)"));
                for (const fs::path& scenePath : scenes) {
                    bool current = scenePath == m_scenePath;
                    if (ImGui::MenuItem(scenePath.filename().string().c_str(), nullptr, current)) {
                        LoadSceneFromFile(scenePath);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(T("Save Scene"), "Ctrl+S")) {
                if (!m_scenePath.empty()) SaveSceneToFile(m_scenePath);
                else openDialog = "Save Scene As";
            }
            if (ImGui::MenuItem(T("Save Scene As..."))) openDialog = "Save Scene As";
            ImGui::Separator();
            if (ImGui::MenuItem(T("Build Game..."), nullptr, false, m_project.Loaded())) {
                openDialog = "Build Game";
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Exit"))) sage::Application::Get().Close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Edit"))) {
            if (ImGui::MenuItem(T("Undo"), "Ctrl+Z", false, !m_undoStack.empty() && !InPlayMode())) Undo();
            if (ImGui::MenuItem(T("Redo"), "Ctrl+Y", false, !m_redoStack.empty() && !InPlayMode())) Redo();
            ImGui::Separator();
            bool hasSel = m_scene->Get(m_selectedId).Valid();
            if (ImGui::MenuItem(T("Duplicate"), "Ctrl+D", false, hasSel)) DuplicateSelected();
            if (ImGui::MenuItem(T("Delete"), "Del", false, hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Play"))) {
            if (ImGui::MenuItem(T("Play"), nullptr, false, m_playState == EditorPlayState::Editing)) StartPlay();
            if (ImGui::MenuItem(T("Pause"), nullptr, false, m_playState == EditorPlayState::Playing)) PausePlay();
            if (ImGui::MenuItem(T("Resume"), nullptr, false, m_playState == EditorPlayState::Paused)) ResumePlay();
            if (ImGui::MenuItem(T("Stop"), nullptr, false, InPlayMode())) StopPlay();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Entity"))) {
            if (ImGui::MenuItem(T("Create Empty"))) {
                PushUndoSnapshot();
                SetSelectedId(m_scene->CreateObject("Empty").Id());
            }
            if (ImGui::BeginMenu(T("Create Primitive"))) {
                struct { const char* name; MeshRef::Type type; } prims[] = {
                    {"Cube", MeshRef::Type::Cube}, {"Sphere", MeshRef::Type::Sphere},
                    {"Plane", MeshRef::Type::Plane}, {"Cylinder", MeshRef::Type::Cylinder},
                    {"Cone", MeshRef::Type::Cone},
                };
                for (const auto& p : prims) {
                    if (ImGui::MenuItem(p.name)) {
                        PushUndoSnapshot();
                        SetSelectedId(CreatePrimitiveEntity(p.name, p.type).Id());
                    }
                }
                ImGui::EndMenu();
            }
            // Наклейка ставится в СЕРЕДИНУ вида и смотрит туда же, куда
            // камера: поставленная в начало координат, она чаще всего оказалась
            // бы внутри пола или далеко за спиной, и первое, что пришлось бы
            // делать, — искать её.
            if (ImGui::MenuItem(T("Create Decal"))) {
                PushUndoSnapshot();
                GameObject d = m_scene->CreateObject("Decal");
                d.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
                d.GetTransform().Position = m_camera.Position + m_camera.Front * 4.0f;
                // Ось Z наклейки — навстречу камере: проекция идёт вдоль -Z, то
                // есть от зрителя вглубь сцены, как и смотрит человек.
                const glm::vec3 f = -m_camera.Front;
                d.GetTransform().Rotation =
                    glm::vec3(glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f))),
                              glm::degrees(std::atan2(f.x, f.z)), 0.0f);
                d.GetTransform().Scale = glm::vec3(1.0f);
                m_scene->Registry().emplace<DecalComponent>(d.Entity());
                SetSelectedId(d.Id());
            }

            // Готовые элементы интерфейса, а не «добавь компонент и настрой».
            //
            // Пустой UIElementComponent — это панель без текста, без размера под
            // содержимое и без интерактива: чтобы получить из него кнопку, надо
            // знать, какие пять полей поменять. Меню отдаёт то, что человек и
            // хотел получить, сразу настроенным; дальше правится всё.
            if (ImGui::BeginMenu(T("Create UI"))) {
                struct Preset { const char* Name; const char* Label; };
                static const Preset kPresets[] = {
                    {"Panel", T("Panel")},   {"Button", T("Button")}, {"Label", T("Label")},
                    {"Image", T("Image")}, {"Bar", T("Bar")},    {"Checkbox", T("Checkbox")},
                    {"Slider", T("Slider")}, {"Input", T("Input")},
                };
                for (const Preset& p : kPresets) {
                    if (ImGui::MenuItem(p.Label)) {
                        PushUndoSnapshot();
                        SetSelectedId(CreateUIEntity(p.Name).Id());
                        // Вёрстка включается сама: элемент, которого не видно
                        // сразу после создания, выглядит как «кнопка не сработала».
                        m_uiEditMode = true;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(T("Create Camera"))) {
                PushUndoSnapshot();
                GameObject camObj = m_scene->CreateObject("Camera");
                m_scene->Registry().emplace<CameraComponent>(camObj.Entity());
                SetSelectedId(camObj.Id());
            }
            // Свет — подменю по типам. Одного пункта «Create Light» было мало:
            // он всегда создавал точечный, а прожектор и солнце приходилось
            // делать через инспектор, догадавшись, что тип там переключается.
            if (ImGui::BeginMenu(T("Create Light"))) {
                struct LightPreset {
                    const char* item;
                    const char* name;
                    LightComponent::Type kind;
                    glm::vec3 position;
                    glm::vec3 rotation;
                };
                const LightPreset presets[] = {
                    {T("Point"), "Light", LightComponent::Type::Point,
                     {0.0f, 2.5f, 0.0f}, {0.0f, 0.0f, 0.0f}},
                    // Прожектор смотрит вниз: поворот -90° по X направляет
                    // «вперёд» (-Z) в -Y. Созданный «в никуда», он выглядел бы
                    // как свет, который не работает.
                    {T("Spot"), "Spotlight", LightComponent::Type::Spot,
                     {0.0f, 5.0f, 0.0f}, {-90.0f, 0.0f, 0.0f}},
                    {T("Directional (sun)"), "Sun", LightComponent::Type::Directional,
                     {0.0f, 10.0f, 0.0f}, {-55.0f, -25.0f, 0.0f}},
                };
                for (const LightPreset& p : presets) {
                    if (!ImGui::MenuItem(p.item)) continue;
                    PushUndoSnapshot();
                    GameObject lightObj = m_scene->CreateObject(p.name);
                    lightObj.GetTransform().Position = p.position;
                    lightObj.GetTransform().Rotation = p.rotation;
                    LightComponent lc;
                    lc.Kind = p.kind;
                    if (p.kind == LightComponent::Type::Directional) {
                        lc.Color = {1.0f, 0.95f, 0.85f};
                        lc.Intensity = 1.0f;
                    } else if (p.kind == LightComponent::Type::Spot) {
                        lc.Intensity = 4.0f;
                    }
                    m_scene->Registry().emplace<LightComponent>(lightObj.Entity(), lc);
                    SetSelectedId(lightObj.Id());
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Физический куб: меш + динамическое тело + бокс-коллайдер — падает
            // под гравитацией сразу в Play (быстрый способ проверить физику).
            if (ImGui::MenuItem(T("Create Physics Cube"))) {
                PushUndoSnapshot();
                GameObject box = CreatePrimitiveEntity("Physics Cube", MeshRef::Type::Cube);
                box.GetTransform().Position = {0.0f, 4.0f, 0.0f};
                m_scene->Registry().emplace<RigidBodyComponent>(box.Entity());
                m_scene->Registry().emplace<ColliderComponent>(box.Entity());
                SetSelectedId(box.Id());
            }
            // Скелетно-анимированная модель: без пути — процедурный демо-щупалец
            // с клипом «Wave» (сразу проигрывается в вьюпорте).
            if (ImGui::MenuItem(T("Create Animated Model"))) {
                PushUndoSnapshot();
                GameObject anim = m_scene->CreateObject("Animated Model");
                anim.GetTransform().Position = {0.0f, 0.0f, 0.0f};
                m_scene->Registry().emplace<AnimatedModelComponent>(anim.Entity());
                SetSelectedId(anim.Id());
            }
            // Эмиттер частиц: по умолчанию пресет «Fire» в точке над началом.
            if (ImGui::MenuItem(T("Create Particle Emitter"))) {
                PushUndoSnapshot();
                GameObject fx = m_scene->CreateObject("Particle Emitter");
                fx.GetTransform().Position = {0.0f, 0.5f, 0.0f};
                ParticleEmitterComponent em;
                em.Config = ParticlePresets::Registry()[0].Make(); // Fire
                em.Preset = 0;
                m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity(), em);
                SetSelectedId(fx.Id());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Window"))) {
            // Сброс раскладки возвращает и сами панели: закрытая вкладка иначе
            // не восстанавливалась «сбросом», хотя именно этого от него ждут.
            if (ImGui::MenuItem(T("Reset Layout"))) {
                ShowAllPanels();
                m_rebuildDockLayout = true;
            }
            ImGui::MenuItem(T("Show Grid"), nullptr, &m_showGrid);
            ImGui::Separator();
            // Каждая панель — переключатель. Это единственный путь назад после
            // крестика на вкладке, поэтому здесь перечислены ВСЕ панели, а не
            // только служебные.
            ImGui::MenuItem(T("Hierarchy"), nullptr, &m_showHierarchy);
            ImGui::MenuItem(T("Inspector"), nullptr, &m_showInspector);
            ImGui::MenuItem(T("Viewport"), nullptr, &m_showViewport);
            ImGui::MenuItem(T("Game"), nullptr, &m_showGame);
            ImGui::MenuItem(T("Assets"), nullptr, &m_showAssets);
            ImGui::MenuItem(T("Console"), nullptr, &m_showConsole);
            ImGui::MenuItem(T("Lighting"), nullptr, &m_showLighting);
            ImGui::MenuItem(T("Code"), nullptr, &m_showCode);
            ImGui::MenuItem(T("Profiler"), nullptr, &m_showProfiler);
            ImGui::MenuItem(T("Icon sheet"), nullptr, &m_showIconSheet);
            ImGui::Separator();
            ImGui::MenuItem(T("Settings..."), nullptr, &m_showSettings);

            // Язык интерфейса. Здесь, а не в окне Settings: то окно правит
            // настройки ИГРЫ и сохраняется в проект, а язык — настройка
            // редактора и живёт в профиле пользователя. Смешать их значило бы
            // получить проект, навязывающий язык всем, кто его откроет.
            if (ImGui::BeginMenu(T("Language"))) {
                for (const sage::editor::LanguageInfo& lang : sage::editor::AvailableLanguages()) {
                    const bool active = lang.Code == sage::editor::CurrentLanguageCode();
                    // Название языка НЕ переводится: человек, случайно
                    // переключивший интерфейс на незнакомый, должен найти
                    // дорогу назад по слову «English», а не по переводу.
                    if (ImGui::MenuItem(lang.Name.c_str(), nullptr, active) && !active) {
                        sage::editor::SetLanguage(lang.Code);
                        SetStatusMessage(T("Interface language changed"));
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Help"))) {
            ImGui::MenuItem(T("About SAGE..."), nullptr, &m_showAbout);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре.
        std::string status = m_project.Loaded()
                                 ? (std::string(T("Project:")) + " " + m_project.Name())
                                 : T("No project");
        float w = ImGui::CalcTextSize(status.c_str()).x + 16.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status.c_str());

        ImGui::EndMenuBar();
    }

    // Модалки диалогов и окно настроек — самостоятельные панели, но рисуются
    // ЗДЕСЬ, на уровне окна-хоста: модалка ImGui совпадает с OpenPopup по
    // ID-стеку окна, поэтому открытие и отрисовку нельзя разносить по окнам.
    if (openDialog) m_dialogs.Open(openDialog);
    m_dialogs.Draw(*this);
    DrawRecoveryPrompt();
    m_settingsPanel.Draw(*this, m_showSettings);
    m_profiler.Draw(&m_showProfiler);
    if (m_showIconSheet) EditorIcons::DrawSheet(&m_showIconSheet);
    m_confirm.Draw();
    DrawAboutWindow();

    ImGui::End();

    // Все панели закрыты — на месте редактора пустой прямоугольник. Молчать
    // здесь нельзя: человек видит серое поле и не знает, что перед ним
    // результат его же крестиков, а не поломка. Подсказка рисуется ПОСЛЕ
    // окна-хоста и отдельным окном: внутри хоста её накрывает фон пустого
    // док-узла, который ImGui кладёт в фоновый канал списка отрисовки.
    if (!AnyPanelVisible()) DrawEmptyDockHint(dockMin.x, dockMin.y, dockMax.x, dockMax.y);

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_scenePath.empty()) SaveSceneToFile(m_scenePath);
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) Redo();
    }
}
