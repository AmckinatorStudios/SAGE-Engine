#include "sage/app/CoreSystems.h"
#include "PlayerLayer.h"

#include "sage/audio/AudioSystem.h"
#include "sage/render/SkyDraw.h"
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

#include "sage/app/Application.h"
#include "sage/core/Config.h"
#include "sage/core/Paths.h"
#include "sage/assets/SaveGame.h"
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
#include "sage/scene/SceneFlow.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

#include "PlayerScenes.h"

namespace fs = std::filesystem;
using namespace player;

PlayerLayer::PlayerLayer(fs::path projectDir, std::string launchArgs)
    : sage::Layer("Player"), m_projectDir(std::move(projectDir)),
      m_launchArgs(std::move(launchArgs)) {}
PlayerLayer::~PlayerLayer() = default;


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

    // Единственное место плеера, где клавиша опрашивается у окна напрямую: до
    // этого экрана дело доходит, когда проект не загрузился, а значит система
    // ввода не построена вовсе (см. BuildSceneRuntime) — спрашивать не у кого.
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
    // ПАКЕТ — ПЕРВЫМ ДЕЛОМ, до шейдеров: собранная игра везёт в game.sagepak и
    // проект, и собственные ресурсы движка (шейдеры, шрифт), россыпи рядом с
    // exe больше нет. Его отсутствие не ошибка: так запускают игру из папки
    // проекта во время разработки, и всё читается с диска (см. vfs).
    {
        const fs::path packFile = m_projectDir / "game.sagepak";
        if (!sage::assets::vfs::Mount(packFile)) {
            const fs::path beside = sage::ExecutableDir() / "game.sagepak";
            sage::assets::vfs::Mount(beside);
        }
    }
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

    if (const std::string p = sage::EnvString("SAGE_SCREENSHOT_PATH"); !p.empty())
        m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);
    // Скриншот пишется до смены CWD? Нет — путь может быть абсолютным (CI так
    // и делает). Относительный путь окажется внутри папки проекта — норма.

    // 2. Проект. Сначала — пакет: собранная игра везёт содержимое проекта
    // одним файлом game.sagepak рядом с exe. Его отсутствие не ошибка: так
    // запускают игру из папки проекта во время разработки, и всё читается с
    // диска (см. sage::assets::vfs).
    std::error_code ec;

    fs::path projectFile = m_projectDir / "project.sageproj";
    fs::current_path(m_projectDir, ec);
    if (ec) {
        Fail("Папка проекта недоступна",
             {m_projectDir.string(), "",
              "Проверьте, что путь существует и его можно открыть."});
        return;
    }

    // ИМЯ ИГРЫ — ИЗ МАНИФЕСТА, ГДЕ БЫ ОН НИ ЛЕЖАЛ.
    //
    // Собранная игра везёт project.sageproj ВНУТРИ game.sagepak: отдельная его
    // копия рядом с exe — лишний файл, и по умолчанию редактор её больше не
    // кладёт (см. EngineConfig::BuildProjectFile). Поэтому читаем через vfs:
    // он сначала смотрит в смонтированный пакет, потом на диск — то есть один
    // и тот же код работает и для собранной игры, и для запуска из папки
    // проекта во время разработки. Пока это был честный std::ifstream, игра
    // без отдельного файла показывалась в заголовке окна как «SAGE Player».
    std::string manifest;
    if (sage::assets::vfs::ReadText("project.sageproj", manifest)) {
        try {
            m_projectName = nlohmann::json::parse(manifest).value("name", m_projectName);
        } catch (const std::exception& e) {
            LOG_WARN("Player") << "project.sageproj не парсится (" << e.what() << "), продолжаю";
        }
    }

    // ЗДЕСЬ ВООБЩЕ ПРОЕКТ? Раньше этот вопрос не задавался, и запуск плеера не
    // из папки игры (например, прямо из build/runtime) доходил до поиска сцен и
    // жаловался на отсутствие scenes/ — то есть отвечал на второй вопрос, не
    // задав первого. «Нет сцен» и «это не папка игры» — разные беды с разными
    // решениями, и путать их значит посылать человека искать не там.
    const bool packed = !SceneFile("main").empty() || !SceneFiles().empty();
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
    // Собранная игра: оглавление базы лежит в пакете (сайдкаров .meta там
    // нет). Сканировать диск рядом с exe — значит найти пару служебных файлов
    // и объявить битой каждую ссылку сцены.
    std::string assetIndex;
    if (sage::assets::vfs::Mounted() &&
        sage::assets::vfs::ReadText(sage::AssetDatabase::kIndexFile, assetIndex)) {
        sage::AssetDatabase::Instance().LoadIndex(assetIndex);
    } else {
        sage::AssetDatabase::Instance().ScanProject(".");
    }

    glfwSetWindowTitle(app.GetWindow().Handle(), m_projectName.c_str());

    // Имя игры определяет папку сохранений. Ставится ДО загрузки сцены: скрипт
    // с OnStart вправе сразу прочитать прогресс, и к этому моменту он обязан
    // знать, откуда читать.
    sage::save::SetGameName(m_projectName);

    // 3. Главная сцена.
    fs::path scenePath = FindMainScene();
    if (scenePath.empty()) {
        Fail("В проекте нет ни одной сцены",
             {"Проект найден, но играть нечего: в assets/Scenes нет ни одного файла .sage.",
              "Искал в: " + (m_projectDir / "assets" / "Scenes").lexically_normal().string() + " и " +
                  (m_projectDir / "scenes").lexically_normal().string(),
              "",
              "Сохраните сцену в редакторе (File > Save Scene) и назначьте её",
              "стартовой: ПКМ по сцене в панели Assets > «Сделать стартовой сценой».",
              "Без этого плеер берёт main.sage, а если её нет — первую по алфавиту."});
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
    // Где лежат сцены игры — для scene:Load, scene:LoadAdditive, scene:List.
    // Через vfs: в собранной игре сцены лежат в пакете, а не на диске.
    {
        sage::scene::SceneLocator where;
        where.Resolve = [](const std::string& name) { return SceneFile(name); };
        where.List = [] {
            std::vector<std::string> names;
            for (const std::string& f : SceneFiles()) names.push_back(fs::path(f).stem().string());
            return names;
        };
        sage::scene::SetSceneLocator(std::move(where));
        sage::scene::ResetTransit(scenePath.stem().string());
    }
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
    // Курсор берётся из системы ввода — того же снимка кадра, что видят
    // действия и скрипты. Отдельный опрос окна давал бы интерфейсу другое
    // положение мыши, чем игре, ровно в те кадры, когда это важнее всего.
    const glm::vec2 cursor = m_input.MousePosition();

    // Window::Width() — размер БУФЕРА (окно слушает FramebufferSizeCallback), а
    // курсор приходит в размерах ОКНА: их отношение и есть множитель HiDPI.
    int winW = 0, winH = 0;
    glfwGetWindowSize(window.Handle(), &winW, &winH);
    const float toPixels = winW > 0 ? (float)window.Width() / (float)winW : 1.0f;
    const float toPixelsY = winH > 0 ? (float)window.Height() / (float)winH : 1.0f;

    return {cursor.x * toPixels - (float)m_uiOffsetX,
            cursor.y * toPixelsY - (float)m_uiOffsetY};
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

void PlayerLayer::OnDetach() {
    // Закрытие окна крестиком приходит сюда, минуя меню и скрипты: последний
    // шанс игре сохраниться. Повторный вызов безвреден — DispatchQuit
    // срабатывает один раз.
    if (m_scripts) m_scripts->DispatchQuit();
    // Скрипты объектов узнают о закрытии ДО того, как исчезнут (OnDestroy), и
    // уходят раньше прежнего движка: Lua-бэкенд живёт на его состоянии.
    if (m_scripting) m_scripting->Shutdown();
    m_scripting.reset();
    if (m_particles) m_particles->SetCollisionQuery({});   // запрос держит физику
    m_physics.reset();
    m_scripts.reset();
    // Кэш префабов держит разобранные сцены, а в них — меши на GPU. Он
    // статический и умирает на exit(), уже после гибели контекста: деструктор
    // геометрии позвал бы драйвер, которого больше нет. Игра, ставящая префабы
    // (а это любая игра про постройку), падала бы ровно при выходе.
    sage::scene::ClearPrefabCache();
    ResourceManager::Instance().Clear();
}

void PlayerLayer::OnUpdate(float dt) {
    if (Failed() || !m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    // Ввод опрашиваем ПЕРВЫМ делом в кадре: скрипты ниже читают именно этот
    // снимок (действия + смещение мыши), и он должен быть одним на весь кадр.
    // Геймпады опрашиваются отдельно: событий для них оконная система не шлёт
    // вовсе — джойстик приходится спрашивать самому, раз в кадр.
    m_inputBridge.PollGamepads();

    // Кадр ввода в два шага (см. InputSystem::BeginFrame): сначала устройства,
    // потом интерфейс, и только потом действия. Иначе выстрел успевает
    // получиться из того самого щелчка, которым нажали кнопку меню.
    m_input.BeginFrame();

    // Интерфейс получает ввод РАНЬШЕ скриптов: щелчок по кнопке меню не должен
    // одновременно стрелять, а буква, набранная в поле имени, — двигать
    // персонажа.
    UpdateUiInput(dt);

    // Что интерфейс съел, того игра не увидит (§29 ТЗ). Раньше UIInputResult
    // считался и не использовался вовсе: игра стреляла сквозь собственное
    // меню, и это было видно каждому, кто нажимал кнопку.
    uint8_t eaten = sage::input::DeviceNone;
    if (m_uiResult.WantsMouse) eaten |= sage::input::DeviceMouse;
    if (m_uiResult.WantsKeyboard) eaten |= sage::input::DeviceKeyboard;
    if (eaten != sage::input::DeviceNone) m_input.BlockDevices(eaten);

    m_input.UpdateActions(dt);

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
    // Положение мира до шага — «прошлое» для смаза движения: кадр ниже сравнит
    // с ним то, куда всё сдвинулось. Сдвигается и на паузе: иначе застывший
    // кадр вечно помнил бы последнее движение и оставался смазанным.
    m_batch.AdvanceVelocityHistory();
    if (scaledDt > 0.0f) {
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

    // ESC В СОБРАННОЙ ИГРЕ — ИГРЕ. Встроенной паузы с меню «Продолжить /
    // Выйти» больше нет: она перехватывала клавишу, которую игра хотела для
    // себя (своё меню, отмена прицела, закрытие инвентаря), и появлялась поверх
    // готового интерфейса игры. Пауза и меню — часть игры: sage.game.Pause,
    // sage.game.Quit и интерфейс сцены дают сделать их какими угодно.
}

// Собирает состояние ввода для интерфейса и прогоняет его через UI сцены.
//
// Всё берётся из системы ввода движка (§28 ТЗ): и мышь, и набранный текст, и
// клавиши редактирования. Своих колбэков окна интерфейс больше не заводит —
// раньше их было два комплекта (у ввода игры и у интерфейса), и какой из них
// увидит нажатие первым, зависело от порядка инициализации.
void PlayerLayer::UpdateUiInput(float dt) {
    if (!m_scene) return;

    auto uiView = m_scene->Registry().view<sage::ui::Element>();
    if (uiView.begin() == uiView.end()) { ResetUiEdits(); return; }

    // Символы — готовыми кодами Unicode: раскладка, Shift и мёртвые клавиши
    // превращают нажатия в символы по правилам системы, повторять их в движке
    // нельзя.
    for (unsigned int codepoint : m_input.TypedText())
        sage::ui::AppendUtf8(m_uiInput.TypedText, codepoint);

    // Клавиши редактирования — по СОБЫТИЯМ кадра, а не по состоянию: у
    // Backspace должен работать автоповтор, а «зажато» и «повторилось» — разные
    // вещи.
    for (const sage::input::InputEvent& e : m_input.FrameEvents()) {
        if (e.Type != sage::input::InputEventType::KeyPressed &&
            e.Type != sage::input::InputEventType::KeyRepeat)
            continue;
        switch (e.Keyboard) {
            case sage::input::Key::Backspace: m_uiInput.Backspace = true; break;
            case sage::input::Key::Delete:    m_uiInput.Delete = true; break;
            case sage::input::Key::Left:      m_uiInput.Left = true; break;
            case sage::input::Key::Right:     m_uiInput.Right = true; break;
            case sage::input::Key::Home:      m_uiInput.Home = true; break;
            case sage::input::Key::End:       m_uiInput.End = true; break;
            case sage::input::Key::Enter:
            case sage::input::Key::KpEnter:   m_uiInput.Enter = true; break;
            case sage::input::Key::Escape:    m_uiInput.Escape = true; break;
            case sage::input::Key::Tab:       m_uiInput.Tab = true; break;
            default: break;
        }
    }

    const sage::input::Mouse& mouse = m_input.State().MouseState();
    const bool down = mouse.Down(sage::input::MouseButton::Left);
    // Захваченный курсор — это режим обзора: экранной точки у мыши нет, и
    // подсвечивать ею элементы нельзя (подсветилось бы то, что под центром).
    const bool captured = m_input.CursorCaptured();
    m_uiInput.Mouse = captured ? glm::vec2(-1.0f) : CursorInViewport();
    m_uiInput.MousePressed = mouse.Pressed(sage::input::MouseButton::Left) && !captured;
    m_uiInput.MouseReleased = mouse.Released(sage::input::MouseButton::Left) && !captured;
    m_uiInput.MouseDown = down && !captured;
    m_uiMouseWasDown = down;
    m_uiInput.DeltaTime = dt;
    // Колесо — прокрутке списков. Захваченный курсор означает обзор, и колесо
    // там принадлежит камере, а не интерфейсу.
    m_uiInput.Wheel = captured ? 0.0f : mouse.Wheel();

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
