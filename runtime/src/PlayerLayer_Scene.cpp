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

// Сцены собранной игры: поиск главной, подъём рантайма сцены, переходы (см. PlayerLayer.h).


// С КАКОЙ СЦЕНЫ НАЧИНАЕТСЯ ИГРА.
//
// Порядок ответов — от «автор сказал» к «движок угадал», и угадывание идёт
// последним не случайно. Пока стартовой сцены в проекте не было, игра брала
// main.sage, а если её нет — ПЕРВУЮ ПО АЛФАВИТУ: уровень, названный «arena»,
// молча становился началом игры, а переименование файла меняло то, что увидит
// игрок. Заметить это можно было только собрав игру и запустив её.
//
// Через vfs, а не через directory_iterator: в собранной игре проект лежит
// ПАКЕТОМ, каталога scenes/ на диске нет, и обход вернул бы пустоту — игра
// честно не нашла бы ни одной сцены и не запустилась.
fs::path PlayerLayer::FindMainScene() const {
    // 1. Выбор автора: project.sageproj -> startScene.
    std::string manifest;
    if (sage::assets::vfs::ReadText("project.sageproj", manifest)) {
        std::string startScene;
        try {
            startScene = nlohmann::json::parse(manifest).value("startScene", std::string());
        } catch (const std::exception&) {
            // Битый манифест — не повод не запускаться: имя игры и стартовая
            // сцена восстановимы догадкой, а игра нужна сейчас.
        }
        if (!startScene.empty()) {
            const std::string found = SceneFile(startScene);
            if (!found.empty()) return fs::path(found);
            const fs::path path = fs::path("assets/Scenes") / (startScene + ".sage");
            // Сказано прямо: сцену, названную в проекте, не нашли. Молча уйти на
            // запасной путь значит запустить НЕ ТУ игру и не объяснить почему.
            LOG_WARN("Player") << "Стартовая сцена проекта не найдена: " << path.generic_string()
                               << " — беру запасную";
        }
    }
    // 2. Соглашение: main.sage.
    if (const std::string main = SceneFile("main"); !main.empty()) return fs::path(main);
    // 3. Догадка: первая по алфавиту. Это уже не выбор, а «хоть что-нибудь».
    const std::vector<std::string> scenes = SceneFiles();
    if (!scenes.empty())
        LOG_WARN("Player") << "Стартовая сцена не задана в проекте — играю первую по алфавиту ("
                           << scenes.front() << ")";
    return scenes.empty() ? fs::path() : fs::path(scenes.front());
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
    // Мост к окну подключается ОДИН раз за жизнь плеера, а не на каждую сцену:
    // BuildSceneRuntime зовётся ещё и при смене уровня (sage.scene.Load), а
    // подписка на события окна снимается только вместе с окном. Второй мост
    // означал бы два одинаковых события на одно нажатие — то есть удвоенную
    // дельту мыши и обзор, летящий вдвое быстрее, начиная со второго уровня.
    if (!m_inputAttached) {
        m_inputBridge.Attach(app.GetWindow(), m_input);
        m_inputAttached = true;
    }
    // Раскладку объявляют скрипты нового уровня — старая уходит вместе с ним.
    m_input.ClearActions();
    m_scripts->BindInput(m_input);
    // События действий уходят и на шину сцены: «input.Jump» слышат подписчики
    // на Lua и связи, настроенные в инспекторе, — а не только тот, кто
    // спрашивает опросом.
    m_input.SetEventBus(&m_scene->Events);

    // Звук игры. Отсутствие звукового устройства (CI, headless) — не повод
    // ронять игру: AudioEngine сам работает вхолостую, а PlaySound из Lua
    // остаётся вызываемым.
    m_audio = std::make_unique<AudioEngine>();
    m_scripts->BindAudio(*m_audio);

    // Модули Lua: require "voxel" найдёт assets/scripts/voxel.lua проекта.
    // CWD уже внутри проекта, поэтому путь относительный — как и у скриптов
    // сущностей в .sage.
    m_scripts->AddScriptSearchPath("assets/scripts");
    // Плагины проекта (plugins/<id>/) уехали в пакет вместе с проектом; их
    // папки — в путях require, чтобы плагин мог разложить код по модулям.
    m_plugins = sage::plugins::Scan("plugins");
    for (const sage::plugins::Manifest& m : m_plugins)
        if (m.Enabled && m.Error.empty()) m_scripts->AddScriptSearchPath(sage::PathToUtf8(m.Dir));

    // Параметры запуска (--autopilot=1, SAGE_GAME_ARGS) — до AttachScript:
    // скрипт читает их уже в OnStart, выбирая режим (автопрогон, зерно мира).
    if (!m_launchArgs.empty()) {
        m_scripts->SetLaunchArgsFromString(m_launchArgs);
        LOG_INFO("Player") << "Параметры запуска игры: " << m_launchArgs;
    }

    // Библиотека SAGE AI (assets/ai, из пакета): до скриптов объектов — их
    // Start вправе запустить граф по имени.
    m_scripts->LoadAiLibrary("assets/ai");

    // Раскладка управления проекта — ПОСЛЕ скриптов, и порядок здесь значащий.
    // Скрипты объявляют СВОИ умолчания (BindAction в OnStart), а input.sageinput
    // — это «как решил автор игры» в редакторе, и он обязан их замещать, а не
    // дописываться к ним: иначе переназначенное действие продолжало бы работать
    // и на старой клавише. Ровно тот же порядок в Play-режиме редактора, чтобы
    // превью игралось тем же управлением, что игра.
    //
    // Через VFS, а не std::ifstream: в собранной игре проект лежит внутри
    // game.sagepak, и обычное чтение файла нашло бы там пустоту — раскладка
    // молча пропала бы ровно при сборке, то есть там, где это заметят последним.
    if (std::string mapping; sage::assets::vfs::ReadText("input.sageinput", mapping)) {
        if (m_input.LoadMappingFromString(mapping))
            LOG_INFO("Player") << "Раскладка управления проекта применена";
    }

    // Физика: строим мир по сущностям с RigidBodyComponent (бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный движок). Игра всегда «в Play».
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты рулят физикой времени выполнения (SetVelocity/SetGravity) — доступно
    // после построения мира (RuntimeBody сущностей уже созданы).
    m_scripts->BindPhysics(*m_physics);
    m_scripts->BindNetwork(m_network); // Net.* в Lua: хост/подключение из скриптов

    // --- СИСТЕМА СКРИПТИНГА -------------------------------------------------
    //
    // Поведение объектов ведёт она (Start/Update/FixedUpdate/LateUpdate,
    // столкновения, публичные переменные), а язык подключается бэкендом. Тот
    // же состав, что в Play-режиме редактора: превью обязано вести себя как
    // игра, иначе редактор перестаёт заменять сборку.
    //
    // ПОСЛЕ физики: Start скрипта вправе спросить у себя CharacterController и
    // получить работающий, а не «физика не привязана» ровно на первом кадре.
    m_scripting = std::make_unique<sage::scripting::ScriptingSystem>();
    m_scripting->AddBackend(sage::scripting::MakeLuaBackend({m_scripts.get()}));
    sage::scripting::ScriptServices services;
    services.ScenePtr = &*m_scene;
    services.Input = &m_input;
    services.Physics = m_physics.get();
    services.Audio = m_audio.get();
    m_scripting->Bind(services);
    m_scripting->SetPlugins(m_plugins);
    const int attached = m_scripting->AttachScene(*m_scene);
    if (attached > 0) LOG_INFO("Player") << "Скриптов привязано: " << attached;
    if (m_scripting->AttachedPlugins() > 0)
        LOG_INFO("Player") << "Плагинов запущено: " << m_scripting->AttachedPlugins();

    // Состав кадра. Регистрируется здесь, когда все подсистемы уже созданы:
    // порядок при этом не задаётся — он определён стадиями внутри
    // RegisterCoreSystems и не зависит от того, кто когда зарегистрировался.
    sage::CoreSystems core;
    core.Scripts = m_scripts.get();
    core.Scripting = m_scripting.get();
    core.Physics = m_physics.get();
    core.Particles = m_particles ? &*m_particles : nullptr;
    core.Audio = m_audio.get();
    core.DayNight = true;   // игра всегда «в Play»: время суток идёт
    sage::RegisterCoreSystems(m_systems, core);

    // Звуковые источники сцены — на старт. Игра всегда «в Play», поэтому
    // отдельного события «начали» у неё нет: началом служит построение
    // рантайма сцены, и оно же случается при переходе на следующий уровень.
    if (m_audio) {
        const int started = sage::audio::StartScene(*m_scene, *m_audio);
        if (started > 0) LOG_INFO("Player") << "Звуковых источников запущено: " << started;
    }
}

bool PlayerLayer::SwitchScene(const std::string& sceneName) {
    namespace fs = std::filesystem;
    fs::path path = m_scenePath;
    if (!sceneName.empty()) {
        // Имя, а не путь: скрипт не знает и не должен знать, где лежат сцены.
        // Через общий поиск (vfs): проверка fs::exists не находила сцену в
        // пакете собранной игры, и переход там не работал вовсе.
        const std::string found = sage::scene::ResolveScene(sceneName);
        if (found.empty()) {
            LOG_ERROR("Player") << "сцена не найдена: scenes/" << sceneName << ".sage"
                                << " — остаюсь в текущей";
            sage::scene::DropNextParams();
            return false;
        }
        path = found;
    }

    std::unique_ptr<Scene> loaded;
    try {
        loaded = SceneSerializer::Load(path.string());
    } catch (const std::exception& e) {
        // Битая сцена НЕ обрывает игру: остаёмся в текущей и говорим почему.
        // Вылет на середине уровня хуже, чем не открывшаяся дверь.
        LOG_ERROR("Player") << "сцена не загрузилась (" << path.string() << "): " << e.what();
        sage::scene::DropNextParams();
        return false;
    }
    // Объекты с меткой «переходит в следующую сцену» — пока старая сцена ещё
    // жива: после её разрушения копировать будет не из чего.
    if (m_scene) {
        std::unique_ptr<Scene> kept = sage::scene::TakeKept(*m_scene);
        sage::scene::PutKept(*kept, *loaded);
    }

    // Порядок разрушения важен. Сначала снимаем состав кадра: он держит
    // указатели на скрипты и физику, и оставить его на снесённые подсистемы
    // означало бы обращение по мёртвому адресу в первом же кадре новой сцены.
    m_systems.Clear();
    if (m_scripting) m_scripting->Shutdown();
    m_scripting.reset();
    if (m_particles) m_particles->SetCollisionQuery({});   // запрос держит физику
    m_physics.reset();
    m_scripts.reset();   // вместе с ним уходит всё состояние скриптов уровня
    // Шина событий принадлежит сцене — снимаем ссылку ДО того, как старая
    // сцена уйдёт из-под ног. BuildSceneRuntime ниже поставит новую.
    m_input.SetEventBus(nullptr);
    // Звук старой сцены глушим, пока она ещё жива: её компоненты держат
    // дескрипторы, и после std::move остановить их будет некому — водопад
    // предыдущего уровня продолжал бы шуметь на следующем.
    if (m_audio && m_scene) sage::audio::StopScene(*m_scene, *m_audio);
    m_scene = std::move(loaded);
    m_scenePath = path;
    m_sceneTime = 0.0f;
    sage::scene::EnterScene(path.stem().string());

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
    // Два источника запроса — прежний движок (sage.scene.Load уровневых
    // скриптов) и система скриптинга (scene:Load скриптов объектов). Спрашиваем
    // оба и в одном месте: иначе «переход работает из одного скрипта и не
    // работает из другого» — и объяснить это игре будет нечем.
    bool wantScene = m_scripts->TakeSceneRequest(sceneName);
    if (!wantScene && m_scripting) wantScene = m_scripting->TakeSceneRequest(sceneName);
    if (wantScene) {
        SwitchScene(sceneName);
    } else if (restart) {
        SwitchScene({});   // пустое имя — та же сцена заново
    }
}
