#pragma once
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
    };

    // Скомпилированный файл. Кэш по пути + времени правки: один и тот же
    // скрипт на сотне объектов компилируется ОДИН раз, а не сто.
    struct Chunk {
        sol::protected_function Fn;
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
// `field.number(...)` и прочие — они нужны и при ВЫПОЛНЕНИИ файла, а не только
// при разборе объявления (см. LuaFields.cpp).
void RegisterFields(Backend& backend);

// Публичные поля скрипта: разбор `X.public = { ... }` ТЕКСТОМ (см. LuaFields.cpp).
sage::vars::Table ParsePublicFields(const std::string& source);

// Общие мелочи для файлов API.
Scene* SceneOrThrow(Backend& backend, const char* who);

} // namespace sage::scripting::lua
