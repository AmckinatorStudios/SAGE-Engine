#include "ScriptEngine.h"
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
    RegisterEngineApi();
}

void ScriptEngine::AddScriptSearchPath(const std::string& dir) {
    if (dir.empty()) return;
    std::string pattern = dir;
    if (pattern.back() != '/' && pattern.back() != '\\') pattern += '/';
    // <dir>/foo.lua и <dir>/foo/init.lua — обе привычные раскладки модуля.
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
}

void ScriptEngine::AttachScript(GameObject object, const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
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

    m_instances.push_back({ object, /*HasObject=*/true, std::move(env), std::move(updateFn),
                            std::move(messageFn), std::move(entityRef), scriptPath });
}

void ScriptEngine::RunScript(const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
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

void ScriptEngine::UpdateAll(float deltaTime) {
    for (auto& instance : m_instances) {
        // Объект уничтожен через DestroyObject() — сущность больше не валидна.
        if (instance.HasObject && !instance.Object.Valid()) continue;
        if (!instance.UpdateFn.valid()) continue; // скрипт без OnUpdate — легитимно (см. AttachScript)

        // Объектные скрипты получают entity первым аргументом, уровневые — нет.
        // EntityRef — кэшированный userdata (см. ScriptInstance): без него sol2
        // аллоцировал бы новый объект на каждый скрипт каждый кадр.
        auto result = instance.HasObject ? instance.UpdateFn(instance.EntityRef, deltaTime)
                                         : instance.UpdateFn(deltaTime);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = (instance.HasObject && instance.Object.Valid()) ? instance.Object.Name() : instance.Path;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnUpdate (" << who << "): " << err.what();
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

    UpdateTimers(deltaTime);
    UpdateCoroutines(deltaTime);
    m_tweens.Update(deltaTime); // твины геймплея — в такт со скриптами
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
            LOG_ERROR("ScriptEngine") << "Ошибка в таймере (id " << id << "): " << err.what();
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
    for (size_t i = 0; i < m_coroutines.size(); ) {
        m_coroutines[i].WaitTime -= dt;
        if (m_coroutines[i].WaitTime > 0.0f) { ++i; continue; }

        sol::coroutine co = m_coroutines[i].Co;
        auto result = co();

        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в корутине: " << err.what();
            m_coroutines.erase(m_coroutines.begin() + i);
            continue;
        }

        if (co.status() == sol::call_status::yielded) {
            // wait(seconds) вернул через yield время следующей паузы.
            // m_coroutines могла реаллоцироваться внутри co() — обращаемся к
            // элементу заново по тому же индексу, не через старую ссылку.
            float nextWait = result.get<sol::optional<float>>().value_or(0.0f);
            m_coroutines[i].Co = co;
            m_coroutines[i].WaitTime = nextWait;
            ++i;
        } else {
            // Корутина дошла до конца функции — больше резюмировать нечего
            m_coroutines.erase(m_coroutines.begin() + i);
        }
    }
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

