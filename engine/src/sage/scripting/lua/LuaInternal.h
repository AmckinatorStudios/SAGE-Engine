#pragma once
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

#include "sage/scene/Scene.h"
#include "sage/scripting/LuaHandlerStore.h"
#include "sage/scripting/LanguageBackend.h"
#include "sage/scripting/lua/LuaBackend.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// ВНУТРЕННЯЯ КУХНЯ LUA-БЭКЕНДА, общая для файлов LuaApi_*.cpp.
//
// Заголовок ВНУТРЕННИЙ: наружу торчит только MakeLuaBackend. Движку незачем
// знать ни про sol2, ни про то, как устроен экземпляр скрипта.
//
// ПРОКСИ ДЕРЖИТ СУЩНОСТЬ, А НЕ УКАЗАТЕЛЬ НА КОМПОНЕНТ, и это не мелочь.
// Компоненты лежат в плотных массивах entt: добавление любого компонента
// ЛЮБОЙ сущности переставляет их в памяти. Скрипт, запомнивший
// `self.Character` на Start и трогающий его через минуту, читал бы к тому
// времени чужую память — падение там, где ошибки нет. Дескриптор {реестр,
// сущность} переживает любые перестановки, а отсутствие компонента становится
// честным nil.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

// --- Прокси объектов и компонентов -------------------------------------------
struct ObjectRef {
    GameObject Obj;
    ObjectRef() = default;
    explicit ObjectRef(GameObject o) : Obj(o) {}
};
struct TransformRef {
    GameObject Obj;
    TransformRef() = default;
    explicit TransformRef(GameObject o) : Obj(o) {}
};
// ВЕКТОР TRANSFORM, ПРИВЯЗАННЫЙ К ОБЪЕКТУ: `transform.position.z = 5` пишет в
// объект. Обычный glm::vec3 из свойства был бы копией, и такая строка молча
// ничего не делала бы — самая частая ловушка скриптовых движков.
//
// Наследник glm::vec3 (sol::base_classes): такой вектор принимает ЛЮБАЯ
// функция, ждущая Vector3, и складывается с ними как обычный. Запись одной
// оси меняет только эту ось объекта (прочие берутся живыми, а не из снимка).
// Результат арифметики — обычный вектор, ни к чему не привязанный.
struct TransformVector : glm::vec3 {
    enum class Field { Position, Rotation, Scale };
    GameObject Obj;
    Field Which = Field::Position;
    TransformVector() = default;
    TransformVector(const glm::vec3& v, GameObject o, Field f) : glm::vec3(v), Obj(o), Which(f) {}
};

// `obj:get_component(...)` → `GetComponent`: нынешнее имя метода, которого
// у прокси нет, ищется в прежнем написании. Пусто — такого метода нет.
std::string SnakeToPascal(const std::string& name);
sol::object SnakeMethod(sol::state_view lua, const char* type, const sol::stack_object& key);

struct CharacterRef {
    GameObject Obj;
    CharacterRef() = default;
    explicit CharacterRef(GameObject o) : Obj(o) {}
};
struct AnimatorRef {
    GameObject Obj;
    AnimatorRef() = default;
    explicit AnimatorRef(GameObject o) : Obj(o) {}
};
struct AudioRef {
    GameObject Obj;
    AudioRef() = default;
    explicit AudioRef(GameObject o) : Obj(o) {}
};
struct CameraRef {
    GameObject Obj;
    CameraRef() = default;
    explicit CameraRef(GameObject o) : Obj(o) {}
};

// Сигнал объекта глазами скрипта: `button.clicked`, `Events` (объект 0).
// Держит НОМЕР объекта, а не сущность: сигнал — адрес в шине, и номер
// переживает любые перестановки реестра. Имя объекта запомнено для сообщений:
// «нельзя подписаться на clicked уничтоженного объекта Play» понятнее, чем
// «объекта 12».
struct SignalRef {
    int Object = 0;
    std::string Name;
    std::string ObjectName;
};

// Соединение: то, что вернул connect. disconnect() снимает его; повторно —
// безопасно (вернёт false).
struct ConnectionRef {
    int Id = 0;
    std::string Signal;
};

class Backend : public LanguageBackend {
public:
    explicit Backend(const LuaBackendConfig& config);
    ~Backend() override;

    const char* Name() const override { return "Lua"; }
    bool Handles(const std::string& extension) const override { return extension == ".lua"; }
    void Bind(const ScriptServices& services) override;

    sage::vars::Table ParseFields(const std::string& source) const override;

    InstanceId Create(const ScriptSource& source, GameObject owner, ScriptError& err) override;
    void Destroy(InstanceId id) override;
    // Снять подписки экземпляров, чей объект уже уничтожен. Сам экземпляр
    // убирает рантайм в следующем кадре, но подписки обязаны уйти СРАЗУ:
    // иначе Events.count() в том же кадре считает мёртвые обработчики.
    void DropDeadSubscriptions();
    void Tick(float dt) override;
    bool Has(InstanceId id, Hook hook) const override;
    bool Call(InstanceId id, Hook hook, float dt, ScriptError& err) override;
    bool CallWith(InstanceId id, Hook hook, GameObject other, ScriptError& err) override;
    bool CallNamed(InstanceId id, Hook hook, const std::string& name, ScriptError& err) override;
    bool Invoke(InstanceId id, const std::string& method,
                const std::vector<sage::vars::Value>& args, ScriptError& err) override;
    bool HasMethod(InstanceId id, const std::string& method) const override;
    bool InvokeEvent(InstanceId id, const std::string& method, const sage::events::Event& event,
                     ScriptError& err) override;
    void ApplyFields(InstanceId id, const sage::vars::Table& fields) override;
    void Reset() override;
    int QueueTests(InstanceId id, const std::string& path) override;
    void TickTests(float dt, std::vector<TestResult>& finished) override;
    int PendingTests() const override { return (int)m_tests.size(); }

    // --- Для файлов LuaApi_*.cpp ---------------------------------------------
    sol::state& Lua() { return *m_lua; }
    const ScriptServices& Services() const { return m_services; }
    Scene* ScenePtr() const { return m_services.ScenePtr; }
    ScriptClock* Clock() const { return m_services.Clock; }

    // Таблица экземпляра скрипта этой сущности — то, что `other:GetScript()`
    // отдаёт Lua. nil, если скрипта нет: спрашивать у камня его логику —
    // обычное дело, а не ошибка.
    sol::object ScriptOf(GameObject object);
    // Позвать метод чужого скрипта прямо из Lua (`enemy:Call("TakeDamage", 20)`).
    sol::object InvokeOn(GameObject object, const std::string& method, sol::variadic_args args);

    // Перевод значений движка в Lua и обратно — один на весь бэкенд.
    sol::object ToLua(const sage::vars::Value& value);
    sage::vars::Value FromLua(const sol::object& object);

    // Доставляет сообщение прежнего движка (SendMessage/Broadcast, адресные
    // события кнопок) скриптам объектов. targetId < 0 — всем.
    void DeliverMessage(int targetId, const std::string& name, sol::object data);
    // Игра закрывается: последний кадр, в котором скрипт ещё может сохраниться.
    void DeliverQuit();

    // Готовый прокси объекта (создаётся по требованию, живёт как userdata Lua).
    sol::object Wrap(GameObject object);

    // --- Сигналы (LuaApi_Signals.cpp) ----------------------------------------
    // Шина сцены или понятная ошибка «сцена не привязана».
    sage::events::Bus& BusOrThrow(const std::string& who);
    // Подписать функцию Lua на сигнал объекта (0 — глобальное событие).
    // Владелец — экземпляр скрипта, который сейчас исполняется: его подписки
    // уйдут вместе с ним.
    ConnectionRef ConnectLua(const SignalRef& signal, const sol::object& fn, bool once,
                             const std::string& who);
    // Послать сигнал из скрипта: data едет подписчикам Lua как есть, C++ —
    // значением движка.
    void EmitLua(int object, const std::string& name, const sol::object& data,
                 const std::string& who);
    // Событие в том виде, в каком его видит обработчик: {name, sender, data}.
    sol::table EventTable(const sage::events::Event& event);
    sol::object EventData(const sage::events::Event& event);
    // Позвать обработчик с событием: fn(event, data), ошибка — в лог с именем
    // сигнала и объекта, соседние обработчики не страдают.
    void CallHandler(const sol::protected_function& fn, const sage::events::Event& event, int owner);
    LuaHandlerStore& Signals() { return *m_signals; }
    InstanceId Current() const { return m_current; }

    // Прокси компонента по имени ("CharacterController", "Animation", "Audio",
    // "Camera", "Transform"). Один список имён на весь бэкенд — второй,
    // заведённый «только для полей», разошёлся бы с первым на третьем
    // компоненте. nil — объекта нет или у него нет такого компонента.
    sol::object ComponentOf(GameObject object, const std::string& component);
    sol::object ComponentOf(const sage::vars::EntityRef& ref, const std::string& component);

private:
    struct Instance {
        GameObject Owner;
        sol::table Self;         // то, что скрипт видит как self
        sol::environment Env;    // окружение файла (свои глобальные у каждого)
        sol::table Class;        // таблица, которую вернул файл
        sol::protected_function Hooks[(size_t)Hook::Count];
        // Старый стиль: файл ничего не вернул, а объявил глобальные
        // OnStart/OnUpdate. Такие хуки зовутся с СУЩНОСТЬЮ первым аргументом —
        // так написаны все существующие игры на движке, и ломать их ради
        // единообразия незачем.
        bool Legacy = false;
        sol::object LegacyEntity;
        // ПРОСТОЙ скрипт: тоже глобальные функции, но нынешние — Start(),
        // Update(dt), OnCollisionEnter(other). Хуки получают только свои
        // аргументы, `self` — глобальная переменная файла (сам объект), поля —
        // глобальные переменные. Отличается от Legacy по тексту: у старого
        // стиля OnStart/OnUpdate или первый параметр `entity`.
        bool Simple = false;
        // Методы в порядке объявления в файле: тесты идут в том порядке, в
        // каком их написали, а не в порядке хеш-таблицы.
        std::vector<std::string> Methods;
    };

    // Один тест в очереди: корутина метода test_* своего экземпляра.
    struct PendingTest {
        InstanceId Inst = kInvalidInstance;
        std::string Script, Name;
        sol::thread Thread;
        sol::coroutine Co;
        bool Started = false;
        float Wait = 0.0f;     // секунд до продолжения
        int WaitFrames = 0;    // кадров до продолжения
        float Elapsed = 0.0f;
        int Frames = 0;
        float Timeout = 30.0f;
    };
    // true — тест закончился (результат в out).
    bool StepTest(PendingTest& t, float dt, TestResult& out);
    std::deque<PendingTest> m_tests;

    // Events.emit("door_open") зовёт OnDoorOpen(data) у простых скриптов.
    // Подписка «на всё» у шины текущей сцены; переставляется, когда сцена
    // сменилась. Жетон — чтобы обработчик, переживший бэкенд (шина сцены
    // живёт дольше), не позвал мёртвый объект.
    void EnsureEventTap();
    void CallEventFunctions(const sage::events::Event& event);
    Scene* m_tapScene = nullptr;
    int m_tapId = 0;
    std::shared_ptr<Backend*> m_tapToken = std::make_shared<Backend*>(this);

    // Позвать функцию экземпляра в его соглашении: простому — без self,
    // остальным — с self (или сущностью у старого стиля) первым аргументом.
    template <typename... Args>
    sol::protected_function_result CallAs(const Instance& inst, const sol::protected_function& fn,
                                          Args&&... args) {
        if (inst.Simple) return fn(std::forward<Args>(args)...);
        sol::object self = inst.Legacy ? inst.LegacyEntity : sol::object(inst.Self);
        return fn(self, std::forward<Args>(args)...);
    }
    // Функция экземпляра по имени (метод скрипта-таблицы или глобальная
    // функция простого скрипта). Не функция — пустая.
    sol::protected_function MethodOf(const Instance& inst, const std::string& name) const;

    // Скомпилированный файл. Кэш по пути + времени правки: один и тот же
    // скрипт на сотне объектов компилируется ОДИН раз, а не сто.
    // ХРАНИТСЯ БАЙТКОД, А НЕ ФУНКЦИЯ. Окружение (_ENV) — upvalue функции
    // файла, ОБЩИЙ для всех замыканий, созданных ею. Пока кэш отдавал одну и
    // ту же функцию, каждому новому экземпляру ей переставляли окружение — и
    // у всех прежних экземпляров того же файла глобальные переменные молча
    // начинали указывать в окружение последнего (нашёл тест на Lua в
    // редакторе: второй объект со скриптом старого стиля сбрасывал счётчик
    // первому). Из байткода каждый экземпляр получает свою функцию со своим
    // _ENV, а разбор текста по-прежнему один на файл.
    struct Chunk {
        std::string Bytecode;
        long long Stamp = 0;
    };

    Instance* Get(InstanceId id);
    const Instance* Get(InstanceId id) const;
    bool Compile(const ScriptSource& source, sol::protected_function& out, ScriptError& err);
    void FillHooks(Instance& inst);
    // Разбирает "player.lua:42: сообщение" на файл, строку и текст: консоль
    // обязана уметь открыть файл на строке, а не показать человеку строку, из
    // которой он выковыривает номер глазами.
    void Explain(const sol::protected_function_result& result, const std::string& path,
                 ScriptError& err) const;
    bool CallHook(InstanceId id, Hook hook, sol::object arg, ScriptError& err);

    std::unique_ptr<sol::state> m_owned;
    sol::state* m_lua = nullptr;
    // Функции Lua, подписанные на сигналы. ПОСЛЕ состояния: уничтожается раньше
    // него (см. LuaHandlerStore.h).
    std::unique_ptr<LuaHandlerStore> m_signals;
    // Экземпляр, чей код сейчас исполняется: ему принадлежат подписки,
    // сделанные в этот момент (см. ConnectLua).
    InstanceId m_current = kInvalidInstance;
    struct CurrentScope {
        Backend& B;
        InstanceId Saved;
        CurrentScope(Backend& b, InstanceId id) : B(b), Saved(b.m_current) { b.m_current = id; }
        ~CurrentScope() { B.m_current = Saved; }
    };
    ScriptEngine* m_interop = nullptr;
    ScriptServices m_services;

    std::unordered_map<InstanceId, Instance> m_instances;
    std::unordered_map<uint32_t, InstanceId> m_byEntity;
    std::unordered_map<std::string, Chunk> m_chunks;
    InstanceId m_nextId = 1;

    // Фабрика экземпляров (Lua): self с метатаблицей, ищущей сначала в классе
    // скрипта, затем в общей базе (GetComponent, GetCharacterController, …).
    sol::protected_function m_makeInstance;
    sol::table m_base;

    friend void RegisterMath(Backend&);
    friend void RegisterObject(Backend&);
    friend void RegisterComponents(Backend&);
    friend void RegisterGlobals(Backend&);
    friend void RegisterSignals(Backend&);
    friend void RegisterTest(Backend&);
    friend void RegisterTweens(Backend&);

    // on_complete твинов из Lua: номер твина → функция. Функции живут здесь, а
    // не в проигрывателе сцены: сцена переживает состояние Lua, и функция
    // Lua внутри проигрывателя отпускалась бы уже из мёртвого состояния.
    sol::table m_tweenCallbacks;
public:
    void SetTweenCallback(uint32_t tween, const sol::protected_function& fn);
    void FireTweenCallback(uint32_t tween);
};

// Разделы API — по файлу на раздел (LuaApi_*.cpp).
void RegisterMath(Backend& backend);
void RegisterObject(Backend& backend);
void RegisterComponents(Backend& backend);
void RegisterGlobals(Backend& backend);
// Сигналы и связи: obj.clicked:connect, obj:on/emit, Events.*, UI.get и
// свойства интерфейса у объекта (text, visible, enabled).
void RegisterSignals(Backend& backend);
// Сигналы и свойства интерфейса у прокси объекта — зовётся из RegisterObject,
// у которого в руках сам тип.
void RegisterObjectSignals(Backend& backend, sol::usertype<ObjectRef>& type);
// Тесты на Lua: библиотека Test (проверки, ожидание кадров, щелчок по
// интерфейсу, клавиши) и UI.create (см. LuaApi_Test.cpp).
void RegisterTest(Backend& backend);
// Твины: Tween.to/from/sequence/play/cancel/pause/resume/is_playing и Ease
// (см. LuaApi_Tweens.cpp). Проигрыватель — общий у сцены (Scene::Tweens).
void RegisterTweens(Backend& backend);
// `field.number(...)` и прочие — они нужны и при ВЫПОЛНЕНИИ файла, а не только
// при разборе объявления (см. LuaFields.cpp).
void RegisterFields(Backend& backend);

// Публичные поля скрипта: разбор `X.public = { ... }` ТЕКСТОМ (см. LuaFields.cpp).
sage::vars::Table ParsePublicFields(const std::string& source);

// Общие мелочи для файлов API.
Scene* SceneOrThrow(Backend& backend, const char* who);

} // namespace sage::scripting::lua
