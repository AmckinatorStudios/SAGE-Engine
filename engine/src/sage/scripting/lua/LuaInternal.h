#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

#include "sage/scene/Scene.h"
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
};

// Разделы API — по файлу на раздел (LuaApi_*.cpp).
void RegisterMath(Backend& backend);
void RegisterObject(Backend& backend);
void RegisterComponents(Backend& backend);
void RegisterGlobals(Backend& backend);
// `field.number(...)` и прочие — они нужны и при ВЫПОЛНЕНИИ файла, а не только
// при разборе объявления (см. LuaFields.cpp).
void RegisterFields(Backend& backend);

// Публичные поля скрипта: разбор `X.public = { ... }` ТЕКСТОМ (см. LuaFields.cpp).
sage::vars::Table ParsePublicFields(const std::string& source);

// Общие мелочи для файлов API.
Scene* SceneOrThrow(Backend& backend, const char* who);

} // namespace sage::scripting::lua
