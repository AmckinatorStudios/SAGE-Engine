#include "ScriptEngine.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/SkinnedModel.h"
#include "sage/physics/Ragdoll.h"
#include "sage/ui/UIShowcase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIIcons.h"
#include "sage/core/KeyNames.h"
#include <algorithm>
#include <cctype>
#include <optional>


ScriptEngine::ScriptEngine() {
    // package открыт ради require: игра крупнее одного экрана кода должна
    // раскладываться по модулям, а общий код (воксельное ядро, утилиты)
    // писаться один раз, а не копироваться в каждый скрипт сущности. io/os
    // по-прежнему закрыты — скриптам игры незачем читать диск и звать процессы.
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::coroutine, sol::lib::package);
    // Пути поиска модулей задаёт ХОСТ через AddScriptSearchPath (папка скриптов
    // проекта). Дефолтный package.path указывает в системные каталоги Lua,
    // которых у игрока нет, и только мешает диагностике — обнуляем. cpath
    // обнуляем насовсем: подгрузка нативных .so/.dll из Lua игре не нужна.
    m_lua["package"]["path"] = "";
    m_lua["package"]["cpath"] = "";
    RegisterModuleLoader();
    RegisterEngineApi();
}

// --- require ЧЕРЕЗ vfs --------------------------------------------------------
//
// СОБРАННАЯ ИГРА НЕ МОГЛА ЗАГРУЗИТЬ НИ ОДНОГО МОДУЛЯ. Скрипт сущности движок
// читает через vfs (в собранной игре проект лежит одним файлом game.sagepak, и
// с диска читать нечего), а `require` шёл штатным загрузчиком Lua — то есть по
// package.path, то есть по НАСТОЯЩЕМУ диску. В редакторе и при запуске из папки
// проекта всё работало, потому что файлы там действительно лежат; в собранной
// игре первый же `require "blocks"` падал с «module not found», и игра не
// стартовала вовсе.
//
// Заметить это по коду было почти нельзя: обе половины по отдельности
// правильны, и расходятся они только там, где файлов на диске нет. Поэтому
// поиск модулей теперь идёт тем же путём, что и всё остальное чтение игры, —
// через vfs, который сам решает, пакет это или диск.
void ScriptEngine::RegisterModuleLoader() {
    sol::table searchers = m_lua["package"]["searchers"];
    // Своим искателем ЗАМЕНЯЕМ штатный поиск по package.path (второй в списке):
    // держать оба значило бы иметь два ответа на вопрос «где лежит модуль» —
    // и в собранной игре они дают разное.
    searchers[2] = [this](const std::string& name) -> sol::object {
        // "world.gen" -> "world/gen": точки в имени модуля — это каталоги.
        std::string rel = name;
        std::replace(rel.begin(), rel.end(), '.', '/');

        std::string tried;
        for (const std::string& dir : m_scriptSearchPaths) {
            const std::string candidates[] = {dir + rel + ".lua", dir + rel + "/init.lua"};
            for (const std::string& path : candidates) {
                std::string source;
                if (!sage::assets::vfs::ReadText(path, source)) {
                    tried += "\n\tнет файла '" + path + "'";
                    continue;
                }
                // Имя куска с '@' — чтобы ошибки внутри модуля ссылались на его
                // файл и строку, а не на «[string \"...\"]».
                sol::load_result chunk = m_lua.load(source, "@" + path);
                if (!chunk.valid()) {
                    sol::error err = chunk;
                    // Синтаксическая ошибка в модуле — это ошибка, а не «модуля
                    // нет»: вернув её строкой, мы покажем человеку причину, а не
                    // список мест, где не нашли.
                    return sol::make_object(m_lua, std::string("\n\t") + err.what());
                }
                return sol::make_object(m_lua, chunk.get<sol::protected_function>());
            }
        }
        if (tried.empty()) tried = "\n\tпути поиска модулей не заданы (AddScriptSearchPath)";
        return sol::make_object(m_lua, tried);
    };
}

void ScriptEngine::AddScriptSearchPath(const std::string& dir) {
    if (dir.empty()) return;
    std::string pattern = dir;
    if (pattern.back() != '/' && pattern.back() != '\\') pattern += '/';
    m_scriptSearchPaths.push_back(pattern);
    // package.path остаётся заполненным ради сообщений об ошибках и чужого
    // кода, который на него смотрит; сам поиск идёт искателем выше.
    std::string entry = pattern + "?.lua;" + pattern + "?/init.lua";
    std::string current = m_lua["package"]["path"];
    m_lua["package"]["path"] = current.empty() ? entry : current + ";" + entry;
}

// Таблица модуля sage.<name>, создаваемая по требованию. get_or_create, а не
// присваивание: модули заполняются из разных Register*-методов, и второй из них

// Таблица модуля sage.<name>, создаваемая по требованию. get_or_create, а не
// присваивание: модули заполняются из разных Register*-методов, и второй из них
// не должен затирать то, что положил первый.
sol::table ScriptEngine::Module(const char* name) {
    sol::table root = m_lua["sage"].get_or_create<sol::table>();
    return root[name].get_or_create<sol::table>();
}

void ScriptEngine::RegisterEngineApi() {
    // Тонкий диспетчер: по одному вызову на связную группу привязок. Порядок —
    // как в прежней монолитной функции (типы прежде их потребителей), поэтому
    // поведение идентично, но каждую область теперь видно/править отдельно.
    RegisterMathTypes();
    RegisterComponentTypes();
    RegisterUIApi();
    RegisterTweenApi();
    RegisterAnimationApi();
    RegisterGameObject();
    RegisterSceneApi();
    RegisterMeshApi();
    RegisterInputApi();
    RegisterCameraApi();
    RegisterParticleApi();
    RegisterBillboardApi();
    RegisterAudioApi();
    RegisterSaveApi();
    RegisterTimerApi();
    RegisterMessagingApi();
    RegisterLaunchArgsApi();
    RegisterGameFlowApi();
    RegisterMathHelpers();
    RegisterLightingApi();
    RegisterPhysicsApi();
    RegisterEventsApi();
    RegisterRenderTextureApi();
    RegisterNetApi();
}

void ScriptEngine::AttachScript(GameObject object, const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    // Текст скрипта берётся через vfs: в собранной игре .lua лежат в пакете.
    // Имя файла передаётся вторым аргументом, чтобы ошибки Lua по-прежнему
    // ссылались на «assets/scripts/hero.lua:12», а не на «string:12» — без
    // этого любая ошибка в скрипте становится неотлаживаемой.
    std::string source;
    if (!sage::assets::vfs::ReadText(scriptPath, source)) {
        throw std::runtime_error("Скрипт не найден: " + scriptPath);
    }
    auto result = m_lua.script(source, env, sol::script_pass_on_error, "@" + scriptPath);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];
    // OnUpdate необязателен для объектных скриптов, которые всё делают в
    // OnStart или через StartCoroutine/Schedule — раньше это было required,
    // что мешало писать скрипты "разово настроил и забыл".

    // OnMessage(entity, name, data) — необязательный хук: если объявлен, скрипт
    // может принимать сообщения от других скриптов (SendMessage/Broadcast).
    sol::protected_function messageFn = env["OnMessage"];

    // Userdata сущности создаём один раз — все дальнейшие вызовы хуков
    // передают его же (см. комментарий у ScriptInstance::EntityRef).
    sol::object entityRef = sol::make_object(m_lua, object);

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn(entityRef);
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    // Повторная привязка ТОГО ЖЕ скрипта к ТОЙ ЖЕ сущности ЗАМЕНЯЕТ прежний
    // экземпляр, а не добавляет второй.
    //
    // Два экземпляра одного скрипта на одном объекте — это всегда ошибка
    // вызывающего (в сцене у сущности ровно один ScriptComponent), но
    // проявляется она мерзко: OnUpdate зовётся дважды за кадр, объект движется
    // вдвое быстрее, счётчики растут вдвое. Ищут такое где угодно — в физике, в
    // дельте времени, в самой формуле, — только не в числе привязок. Замена
    // заодно даёт естественный смысл «перепривязать» для перезагрузки скрипта
    // после правки файла: OnStart выполнится заново, старое состояние уйдёт.
    for (ScriptInstance& existing : m_instances) {
        if (!existing.HasObject || existing.Path != scriptPath) continue;
        if (existing.Object.Entity() != object.Entity()) continue;
        existing.Env = std::move(env);
        existing.UpdateFn = std::move(updateFn);
        existing.MessageFn = std::move(messageFn);
        existing.FixedUpdateFn = existing.Env["OnFixedUpdate"];
        existing.LateUpdateFn = existing.Env["OnLateUpdate"];
        existing.EntityRef = std::move(entityRef);
        existing.Object = object;
        existing.UpdateErrors = 0;
        existing.UpdateDisabled = false;
        return;
    }

    m_instances.push_back({ object, /*HasObject=*/true, std::move(env), std::move(updateFn),
                            std::move(messageFn), std::move(entityRef), scriptPath });
    m_instances.back().FixedUpdateFn = m_instances.back().Env["OnFixedUpdate"];
    m_instances.back().LateUpdateFn = m_instances.back().Env["OnLateUpdate"];
}

void ScriptEngine::RunScript(const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    // Текст скрипта берётся через vfs: в собранной игре .lua лежат в пакете.
    // Имя файла передаётся вторым аргументом, чтобы ошибки Lua по-прежнему
    // ссылались на «assets/scripts/hero.lua:12», а не на «string:12» — без
    // этого любая ошибка в скрипте становится неотлаживаемой.
    std::string source;
    if (!sage::assets::vfs::ReadText(scriptPath, source)) {
        throw std::runtime_error("Скрипт не найден: " + scriptPath);
    }
    auto result = m_lua.script(source, env, sol::script_pass_on_error, "@" + scriptPath);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn();
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    m_instances.push_back({ GameObject{}, /*HasObject=*/false, std::move(env), std::move(updateFn),
                            sol::protected_function{}, sol::object{}, scriptPath });
    m_instances.back().FixedUpdateFn = m_instances.back().Env["OnFixedUpdate"];
    m_instances.back().LateUpdateFn = m_instances.back().Env["OnLateUpdate"];
}

void ScriptEngine::DispatchMessage(int targetId, const std::string& name, sol::object data) {
    // Ограничение глубины: OnMessage может сам слать сообщения — цикл из двух
    // обработчиков, пересылающих друг другу, дал бы неограниченную рекурсию и
    // переполнение C++-стека (краш всего движка из-за ошибки в скрипте).
    constexpr int kMaxMessageDepth = 16;
    if (m_messageDepth >= kMaxMessageDepth) {
        LOG_ERROR("ScriptEngine") << "SendMessage/Broadcast: превышена глубина вложенной рассылки ("
                                  << kMaxMessageDepth << ") на сообщении '" << name
                                  << "' — вероятен цикл OnMessage-обработчиков, рассылка оборвана";
        return;
    }
    ++m_messageDepth;

    // Собираем цели ДО вызова обработчиков: обработчик может сам слать сообщения,
    // спавнить или уничтожать объекты — любое из этого способно реаллоцировать
    // m_instances и оборвать итератор. Снимок (объект + КОПИЯ обработчика, дешёвая
    // ссылка на тот же Lua-объект) к такой мутации иммунен. targetId < 0 — всем.
    struct Target { GameObject Object; sol::object Ref; sol::protected_function Fn; };
    std::vector<Target> targets;
    for (auto& inst : m_instances) {
        if (!inst.HasObject || !inst.Object.Valid()) continue;
        if (!inst.MessageFn.valid()) continue;
        if (targetId >= 0 && inst.Object.Id() != targetId) continue;
        targets.push_back({ inst.Object, inst.EntityRef, inst.MessageFn });
    }
    for (auto& t : targets) {
        if (!t.Object.Valid()) continue; // мог быть уничтожен предыдущим обработчиком в этой рассылке
        auto result = t.Fn(t.Ref, name, data);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = t.Object.Valid() ? t.Object.Name() : std::string("?");
            LOG_ERROR("ScriptEngine") << "Ошибка в OnMessage (" << who << "): " << err.what();
        }
    }

    --m_messageDepth;
}

// Игра закрывается: последний вызов, который скрипты ещё увидят.
//
// Обработчик ищется в окружении скрипта здесь, а не запоминается при привязке:
// он зовётся один раз за всю жизнь движка, и держать ради этого поле в каждом
// экземпляре — плата за каждый скрипт ради того, что случится однажды.
//
// Ошибка внутри OnQuit НЕ мешает выходу: игра уже закрывается, и падать на
// прощание — худшее, что можно сделать. Сообщаем и идём дальше.
void ScriptEngine::DispatchQuit() {
    if (!m_quitDispatched) DispatchEvent("quit");
    if (m_quitDispatched) return;
    m_quitDispatched = true;

    for (ScriptInstance& inst : m_instances) {
        sol::protected_function quitFn = inst.Env["OnQuit"];
        if (!quitFn.valid()) continue;
        // Сущность могла умереть раньше выхода — уровневым скриптам её и так
        // не передают, а объектному вместо мёртвой ссылки не передаём ничего.
        const bool alive = inst.HasObject && inst.Object.Valid();
        auto result = alive ? quitFn(inst.EntityRef) : quitFn();
        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnQuit (" << inst.Path << "): " << err.what();
        }
    }
}

// Сколько раз подряд OnUpdate может упасть, прежде чем движок перестанет его
// звать. Три, а не один: разовая ошибка бывает от переходного состояния (объект
// ещё не создан, цель ещё не найдена), и гасить скрипт из-за одного кадра —
// перебор. Но три раза подряд — это уже не случайность, а сломанный скрипт, и
// звать его ещё пятьдесят семь раз за эту же секунду незачем.
//
// ПОЧЕМУ ВООБЩЕ ГАСИМ. Ошибка в OnUpdate повторяется каждый кадр. Одна опечатка
// давала три с половиной тысячи одинаковых строк в минуту: консоль редактора
// переставала быть читаемой, и найти в этом потоке вторую ошибку было нельзя.
// Само сообщение при этом печатается ОДИН раз — повторы ничего не добавляют.
static constexpr int kMaxUpdateErrors = 3;

void ScriptEngine::UpdateAll(float deltaTime) {
    for (auto& instance : m_instances) {
        // Объект уничтожен через DestroyObject() — сущность больше не валидна.
        if (instance.HasObject && !instance.Object.Valid()) continue;
        if (!instance.UpdateFn.valid()) continue; // скрипт без OnUpdate — легитимно (см. AttachScript)
        if (instance.UpdateDisabled) continue;    // погашен после серии ошибок

        // Объектные скрипты получают entity первым аргументом, уровневые — нет.
        // EntityRef — кэшированный userdata (см. ScriptInstance): без него sol2
        // аллоцировал бы новый объект на каждый скрипт каждый кадр.
        auto result = instance.HasObject ? instance.UpdateFn(instance.EntityRef, deltaTime)
                                         : instance.UpdateFn(deltaTime);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = (instance.HasObject && instance.Object.Valid()) ? instance.Object.Name() : instance.Path;
            ++instance.UpdateErrors;
            // Печатаем ПЕРВУЮ ошибку целиком — в ней стек и номер строки.
            if (instance.UpdateErrors == 1) {
                LOG_ERROR("ScriptEngine") << "Ошибка в OnUpdate (" << who << "): " << err.what();
            }
            if (instance.UpdateErrors >= kMaxUpdateErrors) {
                instance.UpdateDisabled = true;
                LOG_ERROR("ScriptEngine")
                    << "OnUpdate скрипта " << instance.Path << " отключён после "
                    << instance.UpdateErrors
                    << " ошибок подряд — исправьте скрипт и перезапустите игру";
            }
        } else {
            // Кадр без ошибки обнуляет счётчик: скрипт, споткнувшийся один раз
            // на переходном состоянии, не должен копить путь к отключению
            // через всю сессию.
            instance.UpdateErrors = 0;
        }
    }

    // Убираем экземпляры с уничтоженным Object одним проходом ПОСЛЕ основного
    // цикла — как и ниже для таймеров/корутин, чтобы не мутировать m_instances
    // во время его же итерации (в т.ч. если DestroyObject был вызван изнутри
    // самого OnUpdate этого же экземпляра).
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [](const ScriptInstance& i) { return i.HasObject && !i.Object.Valid(); }),
        m_instances.end());

    CallHook(&ScriptInstance::LateUpdateFn, deltaTime);

    UpdateTimers(deltaTime);
    UpdateCoroutines(deltaTime);
    m_tweens.Update(deltaTime); // твины геймплея — в такт со скриптами
    // События сети — в ТОМ ЖЕ кадре, в котором пришли. Отложи их до следующего,
    // и ответ на команду игрока опаздывал бы на кадр на ровном месте.
    DispatchNetEvents();
}

// OnFixedUpdate: постоянный шаг с накоплением остатка. Потолок в восемь шагов
// не даёт тяжёлому кадру утянуть кадр следующий — иначе просадка нарастает
// сама себя.
void ScriptEngine::UpdateFixed(float deltaTime) {
    m_fixedAccum += deltaTime;
    const float cap = m_fixedStep * 8.0f;
    if (m_fixedAccum > cap) m_fixedAccum = cap;
    while (m_fixedAccum >= m_fixedStep) {
        CallHook(&ScriptInstance::FixedUpdateFn, m_fixedStep);
        m_fixedAccum -= m_fixedStep;
    }
}

void ScriptEngine::CallHook(sol::protected_function ScriptInstance::*hook, float dt) {
    for (auto& instance : m_instances) {
        if (instance.HasObject && !instance.Object.Valid()) continue;
        sol::protected_function& fn = instance.*hook;
        if (!fn.valid()) continue;
        auto r = instance.HasObject ? fn(instance.EntityRef, dt) : fn(dt);
        if (r.valid()) continue;
        const sol::error err = r;
        LOG_ERROR("ScriptEngine") << "Ошибка в хуке (" << instance.Path << "): " << err.what();
        fn = sol::protected_function{};   // гасим, чтобы не залить лог за кадр
    }
}

void ScriptEngine::UpdateTimers(float dt) {
    // Индекс, а не диапазон/итератор: колбэк таймера может сам вызвать
    // Schedule/Repeat (см. ниже), чей push_back способен реаллоцировать
    // m_scheduled — любой ранее взятый итератор/ссылка внутрь вектора после
    // этого висячий (undefined behavior). Индекс остаётся корректным для уже
    // пройденных и текущего элемента (до конца этой функции ничего не
    // удаляется поэлементно, только один проход erase-remove в самом конце).
    //
    // Сам колбэк ПЕРЕД вызовом копируем в локальную переменную, а не вызываем
    // прямо "m_scheduled[i].Fn()": sol2 после resume пишет результат обратно
    // в себя (в "this"), и если бы этот "this" жил внутри вектора, реаллокация
    // ВНУТРИ самого вызова (из-за вложенного Schedule/Repeat) оборвала бы его
    // раньше, чем вызов успеет завершиться — copy sol::protected_function
    // дешёвый (просто ссылка на тот же Lua-объект), поэтому это не накладно.
    //
    // Границу цикла ФИКСИРУЕМ до прохода: таймер, созданный колбэком ВНУТРИ
    // этого же прохода (Schedule из тела таймера), не должен тикать в текущем
    // кадре — иначе он теряет один dt и срабатывает на кадр раньше срока.
    const size_t scheduledAtFrameStart = m_scheduled.size();
    for (size_t i = 0; i < scheduledAtFrameStart; ++i) {
        if (m_scheduled[i].Cancelled) continue;
        m_scheduled[i].TimeLeft -= dt;
        if (m_scheduled[i].TimeLeft > 0.0f) continue;

        int id = m_scheduled[i].Id;
        sol::protected_function fn = m_scheduled[i].Fn;

        auto result = fn();
        if (!result.valid()) {
            sol::error err = result;
            // Тот же довод, что и у OnUpdate: повторяющийся таймер с ошибкой в
            // теле — это поток одинаковых строк с частотой своего интервала.
            ++m_scheduled[i].Errors;
            if (m_scheduled[i].Errors == 1) {
                LOG_ERROR("ScriptEngine") << "Ошибка в таймере (id " << id << "): " << err.what();
            }
            if (m_scheduled[i].Errors >= kMaxUpdateErrors) {
                LOG_ERROR("ScriptEngine") << "Таймер (id " << id << ") отменён после "
                                          << m_scheduled[i].Errors << " ошибок подряд";
                m_scheduled[i].Cancelled = true;
                continue;
            }
        } else {
            m_scheduled[i].Errors = 0;
        }

        // m_scheduled могла реаллоцироваться внутри fn() — обращаемся к
        // элементу заново по тому же индексу, не через старую ссылку.
        if (m_scheduled[i].Repeating) {
            m_scheduled[i].TimeLeft += m_scheduled[i].Interval; // "+=", не "=", чтобы не копить дрейф при просадках FPS
        } else {
            m_scheduled[i].Cancelled = true; // одноразовый — гасим после первого срабатывания
        }
    }

    // Чистим отменённые/сработавшие одноразовые вызовы одним проходом за кадр
    m_scheduled.erase(
        std::remove_if(m_scheduled.begin(), m_scheduled.end(), [](const ScheduledCall& c) { return c.Cancelled; }),
        m_scheduled.end());
}

void ScriptEngine::UpdateCoroutines(float dt) {
    // Индекс, а не итератор — по той же причине, что и в UpdateTimers: тело
    // корутины может само вызвать StartCoroutine, чей push_back способен
    // реаллоцировать m_coroutines. Резюмируем ЛОКАЛЬНУЮ КОПИЮ sol::coroutine
    // (дешёвая операция — копирует лишь ссылку на тот же Lua-поток), а не
    // объект, живущий прямо в векторе: sol2 после lua_resume пишет статус
    // обратно в себя (в "this"), и если бы "this" был указателем внутрь
    // вектора, реаллокация ВНУТРИ самого вызова оборвала бы его раньше, чем
    // вызов успеет завершиться. Индекс остаётся корректным после реаллокации
    // (erase здесь — только для текущего/уже пройденных элементов).
    // ДВА ПРАВИЛА, И ОБА КУПЛЕНЫ ДОРОГО.
    //
    // 1. КОРУТИНЫ ХРАНЯТСЯ ЧЕРЕЗ УКАЗАТЕЛЬ (см. m_coroutines), а не по
    //    значению. CoroutineInstance владеет двумя объектами sol2 —
    //    sol::coroutine и sol::thread, — и любое ПЕРЕМЕЩЕНИЕ такой структуры
    //    внутри вектора (удаление соседа, реаллокация) переставляет ссылки на
    //    Lua-поток. На практике это выглядело так: одна корутина упала с
    //    ошибкой, её сняли — и СЛЕДУЮЩАЯ при первом же резюме получала «cannot
    //    resume dead coroutine» и молча умирала навсегда. В игре это читается
    //    как «катсцена иногда не доигрывает», и найти концы почти невозможно:
    //    ошибка видна у одной корутины, а перестаёт работать другая.
    //    С указателем сами объекты sol2 не двигаются никогда.
    //
    // 2. ПОМЕТИТЬ И СМЕСТИ, а не «удалить по месту».
    //
    //    Отработавшая или упавшая корутина раньше удалялась ПРЯМО В ЦИКЛЕ
    //    (erase(begin + i); continue), то есть индексы ехали под ногами у
    //    обхода. Здесь тот же приём, что уже применён к скриптам и таймерам
    //    выше: во время прохода ничего не удаляем, только помечаем, а чистим
    //    одним erase-remove после.
    for (size_t i = 0; i < m_coroutines.size(); ++i) {
        if (m_coroutines[i]->Dead) continue;
        m_coroutines[i]->WaitTime -= dt;
        if (m_coroutines[i]->WaitTime > 0.0f) continue;

        sol::coroutine co = m_coroutines[i]->Co;
        bool failed = false;
        std::string error;
        {
            // Результат вызова живёт в своей области видимости и разрушается
            // ДО того, как мы тронем вектор: он держит ссылку на стек того же
            // Lua-потока, и трогать вектор, пока он жив, — значит смешивать
            // два владения одним и тем же.
            auto result = co();
            if (!result.valid()) {
                sol::error err = result;
                error = err.what();
                failed = true;
            } else if (co.status() == sol::call_status::yielded) {
                // wait(seconds) вернул через yield время следующей паузы.
                // m_coroutines могла реаллоцироваться внутри co() — обращаемся к
                // элементу заново по тому же индексу, не через старую ссылку.
                const float nextWait = result.get<sol::optional<float>>().value_or(0.0f);
                m_coroutines[i]->Co = co;
                m_coroutines[i]->WaitTime = nextWait;
                continue;
            }
        }

        if (failed) LOG_ERROR("ScriptEngine") << "Ошибка в корутине: " << error;
        m_coroutines[i]->Dead = true; // доиграла или упала — снимем в конце прохода
    }

    m_coroutines.erase(
        std::remove_if(m_coroutines.begin(), m_coroutines.end(),
                       [](const std::shared_ptr<CoroutineInstance>& c) { return c->Dead; }),
        m_coroutines.end());
}

const Texture* ScriptEngine::GetOrLoadBillboardTexture(const std::string& path) {
    auto it = m_billboardTextures.find(path);
    if (it != m_billboardTextures.end()) return it->second.get();

    // Bilinear — разумный дефолт для одиночных спрайтов (не атлас, в
    // отличие от блочного атласа вокселей, которому нужен Nearest)
    auto texture = std::make_unique<Texture>(path, TextureFilter::Bilinear);
    const Texture* raw = texture.get();
    m_billboardTextures[path] = std::move(texture);
    return raw;
}

