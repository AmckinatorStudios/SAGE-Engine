#include "PlayerLayer.h"
#include "sage/render/DebugView.h"
#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "sage/core/Application.h"
#include "sage/core/Config.h"
#include "sage/core/Paths.h"
#include "sage/core/SaveGame.h"
#include "sage/scene/Prefab.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/ecs/DecalSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/Screenshot.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace fs = std::filesystem;

PlayerLayer::PlayerLayer(fs::path projectDir, std::string launchArgs)
    : sage::Layer("Player"), m_projectDir(std::move(projectDir)),
      m_launchArgs(std::move(launchArgs)) {}
PlayerLayer::~PlayerLayer() = default;

fs::path PlayerLayer::FindMainScene() const {
    // Через vfs, а не через directory_iterator: в собранной игре проект лежит
    // ПАКЕТОМ, каталога scenes/ на диске нет, и обход вернул бы пустоту — игра
    // честно не нашла бы ни одной сцены и не запустилась.
    if (sage::assets::vfs::Exists("scenes/main.sage")) return fs::path("scenes/main.sage");
    const std::vector<std::string> scenes =
        sage::assets::vfs::ListFiles("scenes", ".sage");
    return scenes.empty() ? fs::path() : fs::path(scenes.front());
}


// --- Экран отказа -----------------------------------------------------------
//
// Плеер, которому нечего запускать, раньше звал app.Close(): окно мигало и
// исчезало, а причина оставалась строчкой в логе — файле, о котором человек,
// запустивший игру двойным щелчком, не знает. Со стороны это «игра не
// запускается», и дальше некуда идти.
void PlayerLayer::Fail(const std::string& title, std::vector<std::string> lines) {
    m_fatalTitle = title;
    m_fatalLines = std::move(lines);
    // В лог — тоже: он нужен тому, кто разбирается по переписке, и в CI.
    LOG_ERROR("Player") << "PLAYER: " << title;
    for (const std::string& l : m_fatalLines)
        if (!l.empty()) LOG_ERROR("Player") << "PLAYER: " << l;
}

void PlayerLayer::DrawFatalScreen() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();
    device.BindDefaultFramebuffer();
    device.SetViewport(0, 0, window.Width(), window.Height());
    device.SetClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    device.Clear(true, true);

    if (!m_ui) m_ui = std::make_unique<UIRenderer>();
    const float w = (float)window.Width();
    const float h = (float)window.Height();
    m_ui->Begin(window.Width(), window.Height());

    // Полоса-подложка во всю ширину: текст на голом фоне читается хуже, а
    // «пустое тёмное окно с надписью» слишком похоже на зависшую игру.
    const float top = h * 0.28f;
    m_ui->RoundedRect(w * 0.06f, top - 34.0f, w * 0.88f,
                      52.0f + 26.0f * (float)m_fatalLines.size() + 24.0f,
                      glm::vec3(0.10f, 0.11f, 0.14f), 0.98f, 10.0f);

    m_ui->Text(w * 0.09f, top, 2.6f, glm::vec3(1.0f, 0.72f, 0.35f), m_fatalTitle);
    float y = top + 44.0f;
    for (const std::string& line : m_fatalLines) {
        if (!line.empty()) m_ui->Text(w * 0.09f, y, 1.5f, glm::vec3(0.88f, 0.90f, 0.94f), line);
        y += 26.0f;
    }
    m_ui->Text(w * 0.09f, y + 10.0f, 1.3f, glm::vec3(0.55f, 0.58f, 0.65f),
               "Esc — выход. Подробности продублированы в sage_player.log");
    m_ui->End();

    if (glfwGetKey(window.Handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS) app.Close();
    TakeAutoScreenshot();
}

void PlayerLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // 1. Шейдеры плеера — рядом с бинарником. Именно «рядом с бинарником», а не
    // «в текущей папке»: путь считается от места установки плеера (см.
    // sage/core/Paths.h). Раньше здесь стояли голые относительные пути, и плеер
    // работал только когда его запускали из его собственной папки — запуск
    // ярлыком или из папки игры валился с «не удалось открыть файл шейдера».
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    // (Сам поиск «рядом с бинарником» живёт в Shader::ReadFile — один раз на
    // все шейдеры движка, а не по копии в каждом месте загрузки.)
    m_shader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    m_shadowShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_shadows.emplace(cfg.Shadows ? cfg.ShadowResolution : 512,
                      std::clamp(cfg.ShadowCascades, 1, ShadowMap::kMaxCascades));
    // Атлас локальных теней заводится только если они включены: это отдельная
    // текстура глубины на десятки мегабайт, и держать её ради выключенной
    // настройки — значит платить памятью за то, чего в кадре нет.
    if (cfg.LocalShadows) {
        m_localShadows.emplace(cfg.LocalShadowResolution,
                               std::max(cfg.LocalShadowResolution / 4, 64));
    }
    m_sky.emplace();
    m_particles.emplace();
    m_billboards.emplace();

    if (const char* p = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);
    // Скриншот пишется до смены CWD? Нет — путь может быть абсолютным (CI так
    // и делает). Относительный путь окажется внутри папки проекта — норма.

    // 2. Проект. Сначала — пакет: собранная игра везёт содержимое проекта
    // одним файлом game.sagepak рядом с exe. Его отсутствие не ошибка: так
    // запускают игру из папки проекта во время разработки, и всё читается с
    // диска (см. sage::assets::vfs).
    std::error_code ec;
    const fs::path packFile = m_projectDir / "game.sagepak";
    if (!sage::assets::vfs::Mount(packFile)) {
        const fs::path beside = sage::ExecutableDir() / "game.sagepak";
        sage::assets::vfs::Mount(beside);
    }

    fs::path projectFile = m_projectDir / "project.sageproj";
    if (fs::exists(projectFile, ec)) {
        try {
            std::ifstream file(projectFile);
            nlohmann::json root;
            file >> root;
            m_projectName = root.value("name", m_projectName);
        } catch (const std::exception& e) {
            LOG_WARN("Player") << "project.sageproj не парсится (" << e.what() << "), продолжаю";
        }
    }
    fs::current_path(m_projectDir, ec);
    if (ec) {
        Fail("Папка проекта недоступна",
             {m_projectDir.string(), "",
              "Проверьте, что путь существует и его можно открыть."});
        return;
    }

    // ЗДЕСЬ ВООБЩЕ ПРОЕКТ? Раньше этот вопрос не задавался, и запуск плеера не
    // из папки игры (например, прямо из build/runtime) доходил до поиска сцен и
    // жаловался на отсутствие scenes/ — то есть отвечал на второй вопрос, не
    // задав первого. «Нет сцен» и «это не папка игры» — разные беды с разными
    // решениями, и путать их значит посылать человека искать не там.
    const bool packed = sage::assets::vfs::Exists("scenes/main.sage") ||
                        !sage::assets::vfs::ListFiles("scenes", ".sage").empty();
    if (!fs::exists(projectFile, ec) && !packed) {
        Fail("Здесь нет игры",
             {"SagePlayer — рантайм: он запускает ГОТОВЫЙ проект, а в этой папке его нет.",
              "Искал: " + (m_projectDir / "project.sageproj").lexically_normal().string(),
              "",
              "Что сделать — любое из:",
              "  • перетащите на плеер папку проекта или сам project.sageproj;",
              "  • запустите с путём:  SagePlayer <папка проекта>;",
              "  • соберите игру в редакторе: File > Build Game — она",
              "    получится папкой, в которой плеер уже лежит рядом с проектом."});
        return;
    }
    // База ассетов — до загрузки сцены: сцена спрашивает у неё актуальные пути
    // по GUID'ам, и пустая база означала бы, что все ссылки сломаны.
    sage::AssetDatabase::Instance().Clear();
    sage::AssetDatabase::Instance().ScanProject(".");

    glfwSetWindowTitle(app.GetWindow().Handle(), m_projectName.c_str());

    // Имя игры определяет папку сохранений. Ставится ДО загрузки сцены: скрипт
    // с OnStart вправе сразу прочитать прогресс, и к этому моменту он обязан
    // знать, откуда читать.
    sage::save::SetGameName(m_projectName);

    // 3. Главная сцена.
    fs::path scenePath = FindMainScene();
    if (scenePath.empty()) {
        Fail("В проекте нет ни одной сцены",
             {"Проект найден, но играть нечего: в scenes/ нет ни одного файла .sage.",
              "Искал в: " + (m_projectDir / "scenes").lexically_normal().string(),
              "",
              "Сохраните сцену в редакторе (File > Save Scene) — плеер берёт",
              "scenes/main.sage, а если её нет, то первую по алфавиту."});
        return;
    }
    try {
        m_scene = SceneSerializer::Load(scenePath.string());
    } catch (const std::exception& e) {
        Fail("Сцена не читается",
             {scenePath.filename().string(), "", e.what(), "",
              "Обычно это сцена от более новой версии движка либо повреждённый файл."});
        return;
    }

    m_scenePath = scenePath;
    BuildSceneRuntime();

    // Запасная камера, если в сцене НЕТ Primary-камеры. НАРОЧНО отличается от
    // редакторской орбитальной камеры (та — {6.5,5,6.5}, yaw -135, pitch -28):
    // низкий фронтальный ракурс, чтобы «нет камеры» сразу читалось как аварийный
    // вид, а не как настоящая игровая камера (иначе игрок думал бы, что игра
    // показывает вьюпорт редактора).
    m_fallbackCamera.Position = {0.0f, 1.4f, 9.0f};
    m_fallbackCamera.Yaw = -90.0f;   // строго вдоль -Z, на сцену
    m_fallbackCamera.Pitch = -6.0f;
    m_fallbackCamera.ProcessMouse(0.0f, 0.0f);

    LOG_INFO("Player") << "PLAYER: started '" << m_projectName << "', scene "
                       << scenePath.filename().string() << " (" << m_scene->Count()
                       << " entities, " << m_physics->BodyCount() << " physics bodies on "
                       << m_physics->BackendName() << ")";
}


// Меню паузы. Рисуется ПОВЕРХ интерфейса игры и своими средствами, а не
// сущностями сцены: это меню рантайма, оно обязано работать в любой игре, в том
// числе в такой, где UI вообще не собран.
//
// Мышь опрашивается здесь же, а не в общем обработчике: пока стоит пауза,
// остальной ввод игры не работает по определению, и заводить ради двух кнопок
// отдельный слой состояния незачем.
void PlayerLayer::DrawPauseMenu(int vpW, int vpH) {
    if (!m_paused || !PauseMenuWanted()) return;
    if (!m_ui) m_ui = std::make_unique<UIRenderer>();

    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    m_ui->Begin(vpW, vpH);
    // Затемнение: без него меню читается поверх пёстрой сцены плохо, а главное —
    // непонятно, что игра остановлена.
    m_ui->Rect(0.0f, 0.0f, (float)vpW, (float)vpH, glm::vec3(0.0f), 0.55f);

    const float bw = 300.0f, bh = 56.0f, gap = 14.0f;
    const float cx = vpW * 0.5f - bw * 0.5f;
    const float cy = vpH * 0.5f - (bh * 2 + gap) * 0.5f;

    const glm::vec2 cursor = CursorInViewport();
    const float px = cursor.x;
    const float py = cursor.y;
    const bool click = glfwGetMouseButton(window.Handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool clickEdge = click && !m_pauseClickLatched;
    m_pauseClickLatched = click;

    auto button = [&](float y, const char* label) {
        const bool hot = px >= cx && px <= cx + bw && py >= y && py <= y + bh;
        const glm::vec3 bg = hot ? glm::vec3(0.24f, 0.32f, 0.46f) : glm::vec3(0.13f, 0.16f, 0.22f);
        m_ui->RoundedRect(cx, y, bw, bh, bg, 0.96f, 10.0f);
        m_ui->RoundedRectOutline(cx, y, bw, bh, 10.0f, 1.0f, glm::vec3(0.55f, 0.62f, 0.75f), 0.8f);
        m_ui->TextCentered(cx + bw * 0.5f, y + bh * 0.5f - 10.0f, 2.2f, glm::vec3(1.0f), label);
        return hot && clickEdge;
    };

    m_ui->TextCentered(cx + bw * 0.5f, cy - 70.0f, 3.0f, glm::vec3(1.0f, 0.86f, 0.6f), "Пауза");
    if (button(cy, "Продолжить")) {
        m_paused = false;
        window.SetCursorCaptured(true);
    }
    if (button(cy + bh + gap, "Выйти из игры")) {
        // Скрипты узнают о выходе ДО закрытия окна — иначе игра с сохранением
        // теряет всё, что случилось после последнего автосохранения, и теряет
        // молча (см. ScriptEngine::DispatchQuit).
        QuitGame();
    }
    m_ui->TextCentered(cx + bw * 0.5f, cy + (bh + gap) * 2 + 8.0f, 1.6f,
                       glm::vec3(0.75f, 0.78f, 0.85f), "ESC — вернуться в игру", 0.9f);
    m_ui->End();
}

// Курсор из координат ОКНА в координаты ИГРОВОГО КАДРА.
//
// Три системы координат, и путать их нельзя. glfwGetCursorPos отдаёт точку в
// «экранных» координатах окна; кадр рисуется в пикселях буфера, которых на
// HiDPI-экране вдвое больше; а сам кадр — это letterbox-прямоугольник ВНУТРИ
// буфера, если в настройках задано фиксированное соотношение сторон. Пока
// перевода не было, интерфейс сцены сравнивал курсор окна прямо с
// прямоугольниками кадра: кнопки ловились мимо — на HiDPI вдвое ближе к левому
// верхнему углу, а с чёрными полосами — со сдвигом на их ширину.
glm::vec2 PlayerLayer::CursorInViewport() const {
    Window& window = sage::Application::Get().GetWindow();
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window.Handle(), &mx, &my);

    // Window::Width() — размер БУФЕРА (окно слушает FramebufferSizeCallback), а
    // курсор приходит в размерах ОКНА: их отношение и есть множитель HiDPI.
    int winW = 0, winH = 0;
    glfwGetWindowSize(window.Handle(), &winW, &winH);
    const float toPixels = winW > 0 ? (float)window.Width() / (float)winW : 1.0f;
    const float toPixelsY = winH > 0 ? (float)window.Height() / (float)winH : 1.0f;

    return {(float)mx * toPixels - (float)m_uiOffsetX,
            (float)my * toPixelsY - (float)m_uiOffsetY};
}

// Выход из игры одним путём для всех кнопок и клавиш.
//
// Путей закрытия у плеера три — кнопка меню, sage.game.Quit из скрипта и
// крестик окна, — и каждый из них обязан дать игре сохраниться. Пока это было
// написано в двух местах из трёх, третий (крестик) молча терял прогресс: со
// стороны игрока «игра не сохранила последние двадцать минут», со стороны кода
// — просто отсутствующая строка.
void PlayerLayer::QuitGame() {
    if (m_scripts) m_scripts->DispatchQuit();
    sage::Application::Get().Close();
}

// Своё меню паузы у игры — значит встроенного нет: два меню на один ESC хуже,
// чем ни одного.
bool PlayerLayer::PauseMenuWanted() const {
    return !m_scripts || m_scripts->PauseMenuEnabled();
}

void PlayerLayer::OnDetach() {
    // Закрытие окна крестиком приходит сюда, минуя меню и скрипты: последний
    // шанс игре сохраниться. Повторный вызов безвреден — DispatchQuit
    // срабатывает один раз.
    if (m_scripts) m_scripts->DispatchQuit();
    m_physics.reset();
    m_scripts.reset();
    // Кэш префабов держит разобранные сцены, а в них — меши на GPU. Он
    // статический и умирает на exit(), уже после гибели контекста: деструктор
    // геометрии позвал бы драйвер, которого больше нет. Игра, ставящая префабы
    // (а это любая игра про постройку), падала бы ровно при выходе.
    sage::scene::ClearPrefabCache();
    ResourceManager::Instance().Clear();
}


// ---------------------------------------------------------------------------
//  Подъём рантайма под загруженную сцену
//
//  Один и тот же код поднимает и первую сцену игры, и каждую следующую. Это не
//  вкусовщина: два пути подъёма неизбежно разъезжаются, и разница вылезает не
//  на первом уровне, а на втором — у игрока.
// ---------------------------------------------------------------------------
void PlayerLayer::BuildSceneRuntime() {
    sage::Application& app = sage::Application::Get();

    // 4. Скрипты: игра стартует сразу (Play всегда включён).
    m_scripts = std::make_unique<ScriptEngine>();
    m_scripts->BindScene(*m_scene);
    if (m_particles) m_scripts->BindParticles(*m_particles); // паритет с Play редактора
    if (m_billboards) m_scripts->BindBillboards(*m_billboards);

    // Ввод — ДО привязки скриптов: раскладку объявляет сам скрипт в OnStart
    // (BindAction), а OnStart вызывается прямо из AttachScript. Привяжи мы ввод
    // после, первые же BindAction упали бы с «ввод не привязан».
    m_input.Attach(app.GetWindow());
    m_rawInput = std::make_unique<WindowRawInput>(m_input, app.GetWindow());
    m_scripts->BindInput(m_input.Actions());
    m_scripts->BindRawInput(*m_rawInput);

    // Звук игры. Отсутствие звукового устройства (CI, headless) — не повод
    // ронять игру: AudioEngine сам работает вхолостую, а PlaySound из Lua
    // остаётся вызываемым.
    m_audio = std::make_unique<AudioEngine>();
    m_scripts->BindAudio(*m_audio);

    // Модули Lua: require "voxel" найдёт assets/scripts/voxel.lua проекта.
    // CWD уже внутри проекта, поэтому путь относительный — как и у скриптов
    // сущностей в .sage.
    m_scripts->AddScriptSearchPath("assets/scripts");

    // Параметры запуска (--autopilot=1, SAGE_GAME_ARGS) — до AttachScript:
    // скрипт читает их уже в OnStart, выбирая режим (автопрогон, зерно мира).
    if (!m_launchArgs.empty()) {
        m_scripts->SetLaunchArgsFromString(m_launchArgs);
        LOG_INFO("Player") << "Параметры запуска игры: " << m_launchArgs;
    }

    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        try {
            m_scripts->AttachScript(GameObject(&m_scene->Registry(), e), path);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Player") << "Скрипт не привязался: " << ex.what();
        }
    }

    // Физика: строим мир по сущностям с RigidBodyComponent (бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный движок). Игра всегда «в Play».
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты рулят физикой времени выполнения (SetVelocity/SetGravity) — доступно
    // после построения мира (RuntimeBody сущностей уже созданы).
    m_scripts->BindPhysics(*m_physics);
    m_scripts->BindNetwork(m_network); // Net.* в Lua: хост/подключение из скриптов

    // Состав кадра. Регистрируется здесь, когда все подсистемы уже созданы:
    // порядок при этом не задаётся — он определён стадиями внутри
    // RegisterCoreSystems и не зависит от того, кто когда зарегистрировался.
    sage::CoreSystems core;
    core.Scripts = m_scripts.get();
    core.Physics = m_physics.get();
    core.Particles = m_particles ? &*m_particles : nullptr;
    core.Audio = m_audio.get();
    sage::RegisterCoreSystems(m_systems, core);

}

bool PlayerLayer::SwitchScene(const std::string& sceneName) {
    namespace fs = std::filesystem;
    fs::path path = m_scenePath;
    if (!sceneName.empty()) {
        // Имя, а не путь: скрипт не знает и не должен знать, где лежат сцены.
        path = fs::path("scenes") / (sceneName + ".sage");
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            LOG_ERROR("Player") << "сцена не найдена: " << path.string()
                                << " — остаюсь в текущей";
            return false;
        }
    }

    std::unique_ptr<Scene> loaded;
    try {
        loaded = SceneSerializer::Load(path.string());
    } catch (const std::exception& e) {
        // Битая сцена НЕ обрывает игру: остаёмся в текущей и говорим почему.
        // Вылет на середине уровня хуже, чем не открывшаяся дверь.
        LOG_ERROR("Player") << "сцена не загрузилась (" << path.string() << "): " << e.what();
        return false;
    }

    // Порядок разрушения важен. Сначала снимаем состав кадра: он держит
    // указатели на скрипты и физику, и оставить его на снесённые подсистемы
    // означало бы обращение по мёртвому адресу в первом же кадре новой сцены.
    m_systems.Clear();
    m_physics.reset();
    m_scripts.reset();   // вместе с ним уходит всё состояние скриптов уровня
    m_scene = std::move(loaded);
    m_scenePath = path;
    m_sceneTime = 0.0f;

    BuildSceneRuntime();
    LOG_INFO("Player") << "сцена: " << path.filename().string() << " (" << m_scene->Count()
                       << " сущностей)";
    return true;
}

void PlayerLayer::ApplyGameFlowRequests() {
    if (!m_scripts) return;

    if (m_scripts->TakeQuitRequest()) {
        QuitGame();
        return;   // дальше делать нечего: игра закрывается
    }
    // Перезапуск разбирается ДО смены сцены: если скрипт попросил и то и
    // другое, побеждает более конкретное — переход на названную сцену.
    const bool restart = m_scripts->TakeRestartRequest();
    std::string sceneName;
    if (m_scripts->TakeSceneRequest(sceneName)) {
        SwitchScene(sceneName);
    } else if (restart) {
        SwitchScene({});   // пустое имя — та же сцена заново
    }
}

void PlayerLayer::OnUpdate(float dt) {
    if (Failed() || !m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    // Ввод опрашиваем ПЕРВЫМ делом в кадре: скрипты ниже читают именно этот
    // снимок (действия + смещение мыши), и он должен быть одним на весь кадр.
    m_input.Update(window.Handle());

    // Интерфейс получает ввод РАНЬШЕ скриптов: щелчок по кнопке меню не должен
    // одновременно стрелять, а буква, набранная в поле имени, — двигать
    // персонажа. Результат (что съел интерфейс) уходит скриптам.
    UpdateUiInput(dt);

    // Логика кадра — планировщиком, а не пятью строками подряд: порядок
    // «скрипты -> физика -> анимация -> частицы -> звук» записан ОДИН раз в
    // RegisterCoreSystems и одинаков в рантайме, редакторе и играх. Раньше эти
    // пять строк были в каждом потребителе своими, и в одной из игр скрипты
    // стояли ПОСЛЕ физики — управление там отставало на кадр.
    // В паузе мир НЕ ТИКАЕТ: ни скрипты, ни физика, ни таймеры. Пауза, в
    // которой продолжает капать здоровье и добегать враги, — не пауза.
    // Масштаб времени игры (sage.time.SetScale) и её собственная пауза
    // (sage.game.Pause) — поверх паузы плеера по ESC. Оба сводятся в одном
    // множителе: разводить их по разным местам значило бы однажды учесть один
    // и забыть другой.
    const float scale = m_scripts ? m_scripts->FrameTimeScale() : 1.0f;
    const float scaledDt = dt * scale;
    if (!m_paused && scaledDt > 0.0f) {
        // Сеть — ДО систем кадра и по НЕмасштабированному времени: транспорт
        // живёт в реальных секундах (таймауты, темп снапшотов), и замедление
        // игры не должно замедлять её связь с сервером. В паузе не тикает
        // вместе со всем миром — иначе пауза одного игрока рвала бы соединение.
        m_network.Update(*m_scene, dt);
        m_systems.Run(*m_scene, scaledDt);
        m_sceneTime += scaledDt; // uTime собственных шейдеров материалов
    }

    // То, что игра запросила за кадр, выполняется ЗДЕСЬ — после того, как все
    // скрипты отработали и ни один из них не находится на стеке.
    ApplyGameFlowRequests();
    if (!m_scene) return;

    // ESC открывает ПАУЗУ, а не закрывает игру.
    //
    // Раньше первый ESC отпускал курсор, а следующий закрывал игру — молча,
    // мгновенно и без подтверждения. Со стороны это выглядело как «игра
    // вылетела»: человек нажимает привычную клавишу «отменить/назад», и окна
    // просто больше нет. Никакой возможности передумать не было.
    //
    // Теперь ESC — переключатель паузы: мир замирает, курсор возвращается,
    // поверх кадра появляется меню с «Продолжить» и «Выйти». Выход остался, но
    // стал НАМЕРЕННЫМ действием, а не побочным эффектом клавиши.
    //
    // Игра со СВОИМ меню (sage.game.SetPauseMenu(false)) забирает ESC себе
    // целиком: перехватывать клавишу и показывать поверх её меню ещё одно —
    // ровно то, из-за чего своё меню было невозможно сделать.
    if (!PauseMenuWanted()) {
        m_escLatched = glfwGetKey(window.Handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
        m_paused = false;
        return;
    }
    const bool escDown = glfwGetKey(window.Handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if (escDown && !m_escLatched) {
        m_paused = !m_paused;
        // В паузе курсор нужен для меню; при возврате в игру — обратно в захват,
        // иначе после «Продолжить» игрок остаётся с курсором посреди экрана.
        window.SetCursorCaptured(!m_paused);
    }
    m_escLatched = escDown;
}

// Собирает состояние ввода для интерфейса и прогоняет его через UI сцены.
//
// Символы и клавиши редактирования приходят СОБЫТИЯМИ (колбэки окна), а не
// опросом: символ зависит от раскладки и композиции, а у Backspace должен
// работать автоповтор. Мышь, наоборот, опрашивается — её состояние
// непрерывно.
void PlayerLayer::UpdateUiInput(float dt) {
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    if (!m_uiCallbacksBound) {
        m_uiCallbacksBound = true;
        window.SetCharCallback(
            [this](unsigned int cp) { sage::ui::AppendUtf8(m_uiInput.TypedText, cp); });
        window.AddKeyCallback([this](int key, int action, int) {
            if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
            switch (key) {
                case GLFW_KEY_BACKSPACE: m_uiInput.Backspace = true; break;
                case GLFW_KEY_DELETE:    m_uiInput.Delete = true; break;
                case GLFW_KEY_LEFT:      m_uiInput.Left = true; break;
                case GLFW_KEY_RIGHT:     m_uiInput.Right = true; break;
                case GLFW_KEY_HOME:      m_uiInput.Home = true; break;
                case GLFW_KEY_END:       m_uiInput.End = true; break;
                case GLFW_KEY_ENTER:
                case GLFW_KEY_KP_ENTER:  m_uiInput.Enter = true; break;
                case GLFW_KEY_ESCAPE:    m_uiInput.Escape = true; break;
                case GLFW_KEY_TAB:       m_uiInput.Tab = true; break;
                default: break;
            }
        });
    }

    auto uiView = m_scene->Registry().view<sage::ui::Transform>();
    if (uiView.begin() == uiView.end()) { ResetUiEdits(); return; }

    const bool down = glfwGetMouseButton(window.Handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    // Захваченный курсор — это режим обзора: экранной точки у мыши нет, и
    // подсвечивать ею элементы нельзя (подсветилось бы то, что под центром).
    const bool captured = window.CursorCaptured();
    m_uiInput.Mouse = captured ? glm::vec2(-1.0f) : CursorInViewport();
    m_uiInput.MousePressed = down && !m_uiMouseWasDown && !captured;
    m_uiInput.MouseReleased = !down && m_uiMouseWasDown && !captured;
    m_uiInput.MouseDown = down && !captured;
    m_uiMouseWasDown = down;
    m_uiInput.DeltaTime = dt;

    m_uiResult = sage::ui::UpdateSceneUI(*m_scene, m_uiInput, m_uiWidth, m_uiHeight);
    ResetUiEdits();
}

// Однокадровые события съедены — гасим, иначе следующий кадр повторит ввод.
void PlayerLayer::ResetUiEdits() {
    m_uiInput.TypedText.clear();
    m_uiInput.Backspace = m_uiInput.Delete = false;
    m_uiInput.Left = m_uiInput.Right = false;
    m_uiInput.Home = m_uiInput.End = false;
    m_uiInput.Enter = m_uiInput.Escape = m_uiInput.Tab = false;
}

ShadowBinding PlayerLayer::FrameShadows(bool sunEnabled) const {
    ShadowBinding b(*m_shadows, sunEnabled);
    if (m_localShadows) b.Local = m_localShadows->Binding();
    return b;
}

void PlayerLayer::OnRender() {
    // Отказ рисуется ВМЕСТО кадра: сцены нет, рисовать нечего, а окно обязано
    // объяснить, почему оно пустое.
    if (Failed()) { DrawFatalScreen(); return; }
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();

    // Наклейки: пересобираются только помеченные (см. DecalComponent::Dirty),
    // поэтому вызов каждый кадр стоит обхода их списка и ничего больше. Здесь,
    // до сбора кадра, чтобы поставленная скриптом наклейка попала в ЭТОТ кадр,
    // а не появилась через один.
    sage::ecs::BuildDecals(*m_scene, sage::ecs::MakeDecalMesh);

    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    // Флаги качества из конфига переопределяют настройки сцены.
    if (!cfg.Skybox) env.Skybox.Enabled = false;
    if (!cfg.Fog) env.Fog.Enabled = false;

    // --- Соотношение сторон: letterbox-viewport по центру окна (или весь экран) ---
    int vpX, vpY, vpW, vpH;
    cfg.LetterboxViewport(window.Width(), window.Height(), vpX, vpY, vpW, vpH);

    // --- Камера: Primary-CameraComponent сцены либо запасной вид ---
    // Кадр берём ТЕМ ЖЕ хелпером, что и панель Game редактора — вид в собранной
    // игре побайтово совпадает с превью (никакой «фальшивости» превью↔игра).
    glm::mat4 view, proj;
    glm::vec3 viewPos;
    float aspect = (float)vpW / (float)std::max(vpH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(*m_scene, aspect);
    if (frame.HasPrimary) {
        view = frame.View;
        proj = frame.Proj;
        viewPos = frame.Position;
    } else {
        // Явно предупреждаем (один раз): без Primary-камеры игра показывает
        // ЗАПАСНОЙ вид — и он НАРОЧНО не совпадает с редакторской камерой, чтобы
        // «нет камеры» нельзя было спутать с нормальным игровым видом.
        if (!m_warnedNoCamera) {
            LOG_WARN("Player") << "В сцене нет Primary-камеры (CameraComponent) — показываю "
                                  "запасной вид. Добавьте камеру: Entity > Create Camera в редакторе.";
            m_warnedNoCamera = true;
        }
        view = m_fallbackCamera.GetViewMatrix();
        proj = m_fallbackCamera.GetProjectionMatrix(aspect);
        viewPos = m_fallbackCamera.Position;
    }

    // Слушатель звука — на игровой камере: 3D-звуки (PlaySound3D из Lua) должны
    // приходить с той стороны, куда игрок реально смотрит. Базис берём из
    // матрицы вида по той же причине, что и ниже для теней — камера может быть
    // как компонентом сцены, так и запасной.
    if (m_audio) {
        const glm::mat3 basis = glm::mat3(view);
        glm::vec3 forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
        glm::vec3 up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
        m_audio->SetListener(viewPos, forward, up);
    }

    // --- Кадр как ГРАФ ПРОХОДОВ ---------------------------------------------
    //
    // Проходы объявляются данными (что читают, во что пишут), а не порядком
    // вызовов. Даёт это три вещи, каждая из которых работает уже сегодня:
    //
    //   • Проход, результат которого никому не нужен, НЕ ВЫПОЛНЯЕТСЯ. Раньше
    //     это были ручные `if` перед каждым захватом; теперь условие живёт в
    //     описании кадра, и забыть его негде.
    //   • Описание проверяется ДО отрисовки: прочитать то, во что никто не
    //     писал, — ошибка кадра, и находится она сообщением, а не чёрным
    //     экраном.
    //   • На вопрос «что и почему было в кадре» отвечает Describe().
    //
    // Порядок при этом задаёт человек — граф его не переставляет (см.
    // FrameGraph.h): он проверяет, что порядок осмыслен, и выбрасывает лишнее.
    m_frame.Reset();
    const auto rShadow = m_frame.DeclareResource("ShadowMap");
    const auto rEnv = m_frame.DeclareResource("EnvCube");
    const auto rPlanar = m_frame.DeclareResource("PlanarMirror");
    const auto rScreen = m_frame.DeclareResource("Screen");

    const bool wantReflections = m_scene->Reflections.Enabled && cfg.Reflections;
    const bool wantPlanar =
        wantReflections && m_scene->Reflections.PlanarEnabled && cfg.PlanarReflections;

    // Отладочный вид кадра (SAGE_DEBUG_VIEW / настройка debugView): показать
    // одну величину вместо освещения. В игре он нужен не меньше, чем в
    // редакторе, — «в редакторе нормально, в игре тёмно» разбирают именно так.
    sage::render::DebugView debugView = sage::render::DebugView::None;
    if (!sage::render::ParseDebugView(cfg.DebugView.c_str(), debugView) &&
        !cfg.DebugView.empty() && cfg.DebugView != "none") {
        static std::string said;
        if (said != cfg.DebugView) {
            said = cfg.DebugView;
            LOG_WARN("Render") << "неизвестный debugView '" << cfg.DebugView << "'";
        }
    }
    const bool debugging = debugView != sage::render::DebugView::None;

    // --- Отражения: карта окружения ---
    // Строго ДО прохода теней и сцены: захват меняет привязанный буфер и
    // viewport, и посреди кадра это стоило бы лишних переключений. Пересъёмка
    // происходит только при смене цвета неба.
    {
        sage::render::RenderPassDesc pass{"Отражения: окружение"};
        pass.Writes = {rEnv};
        pass.Enabled = wantReflections;
        pass.Execute = [&, this] {
        m_reflections.SetEnabled(wantReflections);
        m_reflections.SetIntensity(m_scene->Reflections.Intensity);
        if (m_sky) {
            // Настоящее небо сцены (набор граней), если оно задано, — иначе
            // отражение показывало бы градиент вместо того, что видно в кадре.
            std::shared_ptr<Skybox> skyAsset;
            const Skybox* cubemap = nullptr;
            if (env.Skybox.HasCubemap()) {
                skyAsset = ResourceManager::Instance().GetSkybox(env.Skybox.CubemapDir);
                cubemap = skyAsset.get();
            }
            m_reflections.UpdateSky(*m_sky, env, cubemap);
        }

        // Зонды сцены: не больше одного за кадр (шесть проходов геометрии).
        if (wantReflections) {
            sage::render::UpdateReflectionProbes(
                *m_scene,
                [&](const glm::mat4& v, const glm::mat4& p) {
                    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    device.Clear(true, true);
                    if (m_sky && env.Skybox.Enabled)
                        m_sky->Draw(v, p, env.Skybox.TopColor, env.Skybox.HorizonColor,
                                    CelestialsFromEnvironment(env));
                    sage::render::SceneColorInput c;
                    c.View = v;
                    c.Proj = p;
                    c.ViewPos = glm::vec3(glm::inverse(v)[3]);
                    c.Env = &env;
                    c.Shadows = FrameShadows(cfg.Shadows);
                    c.Time = m_sceneTime;
                    sage::render::RenderSceneColor(*m_scene, m_batch, c);
                },
                1);
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Тени: глубина от солнца (можно отключить в настройках) ---
    // Строго ПОСЛЕ выбора камеры: каскады делят дальность именно её взгляда, и
    // без неё считать их не из чего. Порядок «тени, потом камера» держался
    // только на том, что одной карте камера была не нужна.
    {
        sage::render::RenderPassDesc pass{"Тени: глубина"};
        pass.Writes = {rShadow};
        // Тени ламп НЕ привязаны к солнечным: сцена без солнца (подвал, ночь,
        // интерьер) — самое место для теней от прожекторов, и выключать их
        // заодно с солнцем значило бы отнимать тени ровно там, где кроме них
        // ничего нет.
        pass.Enabled = cfg.Shadows || m_localShadows.has_value();
        pass.Execute = [&, this] {
        if (cfg.Shadows) {
            ShadowMap::CameraView v;
            v.Position = viewPos;
            // Направление и «верх» достаём из матрицы вида: камера здесь может быть
            // как компонентом сцены, так и запасной, и общего объекта Camera у них
            // нет.
            const glm::mat3 basis = glm::mat3(view);
            v.Forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
            v.Up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
            // Угол обзора и ближнюю плоскость восстанавливаем из матрицы проекции —
            // по той же причине.
            v.FovY = 2.0f * std::atan(1.0f / proj[1][1]);
            v.Aspect = aspect;
            v.Near = proj[3][2] / (proj[2][2] - 1.0f);
            // Дальность теней: сцена главнее настроек. Сто двадцать метров на
            // мир в тридцать метров — это карта теней, четыре пятых которой
            // ушли в пустоту, и оторванная от предмета тень (см.
            // sage::ShadowSettings).
            v.ShadowDistance =
                env.Shadows.Distance > 0.0f ? env.Shadows.Distance : cfg.ShadowDistance;
            // Одна карта — это тоже карта ВОКРУГ КАМЕРЫ, а не вокруг начала мира:
            // раньше здесь стоял ортобокс радиусом 24 м в точке (0,0,0), и всё, что
            // игра успевала отплыть или отойти от неё, оставалось без теней —
            // молча, потому что это выглядит как «тени просто выключены», а не как
            // ошибка.
            if (m_shadows->CascadeCount() > 1) m_shadows->SetCascades(env.Sun.Direction, v);
            else m_shadows->FitSingle(env.Sun.Direction, v);
            sage::render::RenderShadowDepth(*m_shadows, *m_scene, m_batch, window.Width(),
                                            window.Height());
        }
        if (m_localShadows) {
            m_localShadows->Prepare(env);
            sage::render::RenderLocalShadowDepth(*m_localShadows, *m_scene, m_batch,
                                                 window.Width(), window.Height());
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Плоское отражение (вода, зеркало) ---
    // Сцена рисуется второй раз зеркально относительно плоскости. Внутри этого
    // прохода плоского отражения НЕТ: иначе зеркало смотрело бы в себя, и
    // каждый кадр стоил бы вдвое дороже предыдущего.
    m_planar.Reset();
    {
        // Зеркальный проход ЧИТАЕТ тени и карту окружения: отражённая сцена
        // освещается тем же светом, что и прямая.
        sage::render::RenderPassDesc pass{"Плоское отражение"};
        pass.Reads = {rShadow, rEnv};
        pass.Writes = {rPlanar};
        pass.Enabled = wantPlanar;
        pass.Execute = [&, this] {
            if (wantPlanar) {
            const glm::vec4 plane = m_scene->Reflections.Plane;
            m_planar.Capture(plane, view, proj, vpW, vpH,
                             [&](const glm::mat4& mv, const glm::mat4& mp) {
                                 const glm::vec3 mirrorEye =
                                     glm::vec3(glm::inverse(mv)[3]);
                                 if (m_sky && env.Skybox.Enabled)
                                     m_sky->Draw(mv, mp, env.Skybox.TopColor, env.Skybox.HorizonColor,
                                                 CelestialsFromEnvironment(env));
                                 sage::render::SceneColorInput rc;
                                 rc.View = mv;
                                 rc.Proj = mp;
                                 rc.ViewPos = mirrorEye;
                                 rc.Env = &env;
                                 rc.Shadows = FrameShadows(cfg.Shadows);
                                 rc.Time = m_sceneTime;
                                 rc.Reflection = m_reflections.Binding(vpW, vpH);
                                 rc.Reflection.CapturingPlanar = true;
                                 sage::render::RenderSceneColor(*m_scene, m_batch, rc);
                             });
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Основной проход: полное освещение + тени + туман/скайбокс ---
    // Полосы letterbox чёрные: сперва чистим весь экран, затем рендерим в
    // центральный viewport нужного соотношения.
    {
        // Главный проход читает ВСЁ, что насчитали предыдущие. Именно эти
        // связи и делают отбрасывание лишнего осмысленным: выключи отражения —
        // и проходы, которые их готовили, перестанут выполняться сами, потому
        // что их результат больше никто не читает.
        sage::render::RenderPassDesc pass{"Сцена: освещение, тени, отражения"};
        pass.Reads = {rShadow};
        if (wantReflections) pass.Reads.push_back(rEnv);
        if (wantPlanar) pass.Reads.push_back(rPlanar);
        pass.Writes = {rScreen};
        pass.ColorLoad = sage::render::LoadOp::Clear;
        pass.ClearColor = glm::vec4(env.SkyColor * 0.9f, 1.0f);
        pass.Execute = [&, this] {
        // Пост-обработка: сцена уходит в HDR-буфер, а на экран — результат
        // цепочки. Тот же PostFX и те же настройки из конфига, что у окна Game
        // в редакторе, — иначе превью и игра показывают разное.
        //
        // В отладочном виде пост-обработка ВЫКЛЮЧЕНА, и это не экономия.
        // Тон-маппинг, гамма, виньетка и насыщенность придуманы для картинки, а
        // здесь на экране не картинка, а величина: 0.2 шероховатости обязаны
        // остаться 0.2, иначе вид отвечает не на тот вопрос, ради которого его
        // включили. Кадр глубины после ACES и гаммы становился почти белым.
        // Пост-обработка ПРОВЕРЯЕТСЯ на этой видеокарте (см. PostFX::CheckPipeline).
        // Её отказ выглядит как чёрный экран при живом кадре — с игрой это ещё
        // хуже, чем с редактором: там хотя бы есть отладочные виды, а здесь
        // человек видит просто чёрное окно. Лучше кадр без свечения, чем ничего.
        bool postOk = true;
        if (cfg.PostProcessing && m_postfx) {
            const sage::render::PostFX::SelfCheck& check = m_postfx->CheckPipeline();
            postOk = !check.Ran || check.Ok;
            if (!postOk && !m_postWarned) {
                m_postWarned = true;
                LOG_ERROR("Player") << "Пост-обработка на этой видеокарте не работает ("
                                    << check.Reason << ") — кадр без эффектов";
            }
        }
        const bool usePost = cfg.PostProcessing && !debugging && postOk;
        if (usePost) {
            if (!m_postfx) m_postfx.emplace();
            // Число сэмплов — по конфигу и той же функцией, что у редактора:
            // MSAA работает на буфере СЦЕНЫ (в нём растеризуется геометрия), а
            // не на экранном, куда уходит уже готовая картинка.
            EnsureFramebuffer(m_sceneFbo, vpW, vpH, sage::render::SceneSamples(cfg));
            m_sceneFbo->Bind();
            device.SetViewport(0, 0, vpW, vpH);
        } else if (vpX != 0 || vpY != 0) {
            device.SetViewport(0, 0, window.Width(), window.Height());
            device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            device.Clear();
        }
        if (!usePost) device.SetViewport(vpX, vpY, vpW, vpH);
        device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
        device.Clear();

        // Аппаратная гамма-коррекция нужна ТОЛЬКО когда сцена идёт прямо в
        // экран: иначе линейный цвет шейдеров уходит на монитор сырым и игра
        // выглядит неоправданно тёмной. С пост-обработкой гамму накладывает
        // composite-проход, и включать её здесь значило бы применить дважды.
        device.SetSRGBWrite(!usePost);

        // Съёмка сцены в именованные картинки (иконки, экраны, зеркала) — до
        // основного прохода: показать их собираются в этом же кадре.
        if (sage::render::RenderTextureViews(*m_scene, m_batch, env, m_sceneTime) > 0 && usePost) {
            m_sceneFbo->Bind();
            device.SetViewport(0, 0, vpW, vpH);
        }

        if (env.Skybox.Enabled) {
            m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor,
                        CelestialsFromEnvironment(env));
        }

        // Статика — через RenderBatch: отсечение по фрустуму + инстансный батчинг.
        sage::render::SceneColorInput color;
        color.View = view;
        color.Proj = proj;
        color.ViewPos = viewPos;
        color.Env = &env;
        color.Shadows = FrameShadows(cfg.Shadows);
        color.OcclusionCulling = cfg.OcclusionCulling;
        color.Time = m_sceneTime;
        color.Reflection = m_reflections.Binding(
            vpW, vpH, wantPlanar ? m_planar.Texture() : sage::rhi::TextureHandle{});
        if (wantReflections) {
            float probeIntensity = 1.0f;
            if (const sage::render::EnvironmentMap* probe =
                    sage::render::PickReflectionProbe(*m_scene, viewPos, &probeIntensity)) {
                color.Reflection.Env = probe;
                color.Reflection.Intensity = probeIntensity;
            }
        }
        color.ShadingMode = (int)debugView;
        sage::render::RenderSceneColor(*m_scene, m_batch, color);

        // Частицы (billboard) — camRight/Up берём из матрицы вида.
        if (m_particles) m_particles->DrawFromView(view, proj);
        // Спрайты-маркеры — тем же способом и сразу после частиц: и те и
        // другие полупрозрачные и не пишут глубину, поэтому идут после всей
        // непрозрачной геометрии и до пост-обработки.
        if (m_billboards) m_billboards->DrawFromView(view, proj);

        // Объём — после геометрии и ДО пост-обработки: лучи обязаны попасть в
        // bloom, иначе солнце светится, а его лучи нет. Работает только с
        // буфером сцены: без пост-обработки читать глубину неоткуда.
        if (usePost && cfg.Volumetrics) {
            m_sceneFbo->Resolve();
            if (!m_volumetrics) m_volumetrics.emplace();
            m_volumetrics->Render(*m_sceneFbo, m_sceneFbo->DepthTexture(), m_sceneFbo->Width(),
                                  m_sceneFbo->Height(), proj, view, viewPos, env,
                                  color.Shadows, sage::render::VolumetricsFromConfig(cfg),
                                  m_sceneTime);
        }

        // Блик в объективе — ПОСЛЕ объёма: облако, закрывшее солнце, обязано
        // погасить и блик, а до объёма его в кадре ещё нет. И до пост-обработки:
        // блик должен пройти через bloom и тон-маппинг вместе со всем кадром.
        if (usePost && cfg.LensFlare) {
            m_sceneFbo->Resolve();
            if (!m_lensFlare) m_lensFlare.emplace();
            m_lensFlare->Render(*m_sceneFbo, m_sceneFbo->ColorTexture(),
                                m_sceneFbo->DepthTexture(), m_sceneFbo->Width(),
                                m_sceneFbo->Height(), proj, view, env,
                                sage::render::LensFlareFromConfig(cfg));
        }

        if (usePost) {
            m_sceneFbo->Resolve(); // MSAA -> обычные текстуры (без MSAA — пустышка)
            device.BindDefaultFramebuffer();
            // Полосы letterbox чистим здесь: цепочка пишет только в свой
            // прямоугольник, и без этого по краям осталось бы содержимое
            // прошлого кадра.
            device.SetViewport(0, 0, window.Width(), window.Height());
            device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            device.Clear();
            m_postfx->Render(m_sceneFbo->ColorTexture(), m_sceneFbo->DepthTexture(),
                             m_sceneFbo->Width(), m_sceneFbo->Height(), proj, view,
                             sage::render::FxFromConfig(cfg),
                             /*output=*/nullptr, vpX, vpY, vpW, vpH);
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // Сборка и выполнение. Ошибка описания кадра — это ошибка программиста, и
    // сообщить о ней надо ГРОМКО: без графа она проявлялась бы чёрным экраном
    // или отражением прошлого кадра, то есть чем-то, что легко принять за
    // «шейдер барахлит».
    std::string frameError;
    if (!m_frame.Compile(rScreen, frameError)) {
        LOG_ERROR("Frame") << "Описание кадра неверно: " << frameError;
        return;
    }
    m_frame.Execute();

    // Описание кадра — при каждой смене состава. Ради этого граф во многом и
    // делается: «почему этот проход не выполнился» — вопрос, на который иначе
    // отвечать нечем, а гадать по чёрному экрану дороже, чем прочитать строку.
    const sage::render::FrameGraphStats& fgs = m_frame.Stats();
    if (fgs.PassesExecuted != m_lastFramePasses) {
        m_lastFramePasses = fgs.PassesExecuted;
        LOG_INFO("Frame") << "Состав кадра: " << fgs.PassesExecuted << " из "
                          << fgs.PassesDeclared << " проходов (отброшено "
                          << fgs.PassesCulled << ", выключено " << fgs.PassesDisabled << ")\n"
                          << m_frame.Describe();
    }

    device.SetSRGBWrite(false); // всё после сцены (UI/оверлеи) — уже в sRGB

    // UI сцены (компоненты интерфейса из .sage): худ/меню, собранные в редакторе.
    // Рисуется в letterbox-viewport с его размерами — якоря совпадают с панелью
    // Game редактора (WYSIWYG).
    auto uiView = m_scene->Registry().view<sage::ui::Transform>();
    if (uiView.begin() != uiView.end()) {
        if (!m_ui) m_ui = std::make_unique<UIRenderer>();
        m_uiWidth = vpW;  // тот же прямоугольник, с которым сравнивается мышь
        m_uiHeight = vpH;
        m_uiOffsetX = vpX;
        m_uiOffsetY = vpY;
        m_ui->Begin(vpW, vpH);
        sage::ui::DrawSceneUI(*m_scene, *m_ui, vpW, vpH);
        m_ui->End();
    }

    DrawPauseMenu(vpW, vpH);

    TakeAutoScreenshot();
}

// Снимок кадра по SAGE_SCREENSHOT_AT_FRAME. Отдельной функцией, потому что
// кадр заканчивается в ДВУХ местах: обычный и экран отказа. Пока снимок жил
// только в обычном, экран отказа нельзя было проверить иначе как открыв его
// глазами — а это ровно тот кадр, который видит человек, у которого что-то не
// работает.
void PlayerLayer::TakeAutoScreenshot() {
    ++m_frameCounter;
    if (m_autoScreenshotFrame < 0 || m_frameCounter != m_autoScreenshotFrame) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
    app.Close();
}
