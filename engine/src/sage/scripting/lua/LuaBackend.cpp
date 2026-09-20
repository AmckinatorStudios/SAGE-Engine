#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scripting/ScriptEngine.h"

namespace sage::scripting::lua {

namespace {

// Как хук называется у скрипта СТАРОГО стиля (глобальные функции файла).
// Пусто — у старого стиля такого хука не было.
const char* LegacyHookName(Hook hook) {
    switch (hook) {
        case Hook::Start: return "OnStart";
        case Hook::Update: return "OnUpdate";
        case Hook::FixedUpdate: return "OnFixedUpdate";
        case Hook::LateUpdate: return "OnLateUpdate";
        case Hook::OnDestroy: return "OnDestroy";
        default: return nullptr;
    }
}

// Фабрика экземпляра: self ищет сначала в классе скрипта, затем в общей базе
// (GetComponent, GetCharacterController, …). Метатаблица, а не копирование
// методов в каждый экземпляр: копия базы на сотне объектов — это сотня
// одинаковых таблиц в памяти и сотня мест, где можно разойтись.
constexpr const char* kInstanceFactory = R"LUA(
return function(class, base)
    local self = {}
    setmetatable(self, {
        __index = function(_, key)
            local v = class[key]
            if v ~= nil then return v end
            return base[key]
        end
    })
    return self
end
)LUA";

} // namespace

Backend::Backend(const LuaBackendConfig& config) : m_interop(config.Interop) {
    if (m_interop) {
        // Состояние уже есть: на нём живут прежние привязки движка. Новый API
        // добавляется РЯДОМ с ними — скрипт нового стиля видит и то и другое.
        m_lua = &m_interop->Lua();
    } else {
        m_owned = std::make_unique<sol::state>();
        m_owned->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                                sol::lib::table, sol::lib::coroutine);
        m_lua = m_owned.get();
    }

    m_base = m_lua->create_table();
    RegisterMath(*this);
    RegisterObject(*this);
    RegisterComponents(*this);
    RegisterGlobals(*this);
    RegisterFields(*this);

    // Мост из прежнего движка: сообщения и выход обязаны доходить и до
    // скриптов объектов — их ведёт уже не он (см. ScriptEngine::SetMessageSink).
    if (m_interop) {
        m_interop->SetMessageSink([this](int targetId, const std::string& name, sol::object data) {
            DeliverMessage(targetId, name, data);
        });
        m_interop->SetQuitSink([this]() { DeliverQuit(); });
    }

    sol::protected_function_result made = m_lua->script(kInstanceFactory);
    m_makeInstance = made.get<sol::protected_function>();

    // Общая база экземпляра: короткие пути к своим компонентам. Написаны на
    // Lua и просто переадресуют объекту — иначе то же самое существовало бы
    // дважды (у объекта и у скрипта) и однажды разошлось бы.
    m_lua->script(R"LUA(
        return function(base)
            function base:GetComponent(name) return self.gameObject:GetComponent(name) end
            function base:GetCharacterController() return self.gameObject:GetCharacterController() end
            function base:GetAnimation() return self.gameObject:GetAnimation() end
            function base:GetAudio() return self.gameObject:GetAudio() end
            function base:GetCamera() return self.gameObject:GetCamera() end
            function base:GetTransform() return self.gameObject.transform end
            function base:GetScript() return self.gameObject:GetScript() end
            function base:Destroy() return self.gameObject:Destroy() end
        end
    )LUA").get<sol::protected_function>()(m_base);
}

Backend::~Backend() {
    // Мост снимается ПЕРВЫМ: прежний движок переживает бэкенд (он владеет
    // состоянием Lua), и оставленный указатель на мёртвый бэкенд сработал бы
    // на первом же сообщении — уже после того, как всё сделано.
    if (m_interop) {
        m_interop->SetMessageSink(nullptr);
        m_interop->SetQuitSink(nullptr);
    }
    Reset();
}

void Backend::Bind(const ScriptServices& services) { m_services = services; }

sage::vars::Table Backend::ParseFields(const std::string& source) const {
    return ParsePublicFields(source);
}

Backend::Instance* Backend::Get(InstanceId id) {
    auto it = m_instances.find(id);
    return it == m_instances.end() ? nullptr : &it->second;
}

const Backend::Instance* Backend::Get(InstanceId id) const {
    auto it = m_instances.find(id);
    return it == m_instances.end() ? nullptr : &it->second;
}

void Backend::Explain(const sol::protected_function_result& result, const std::string& path,
                      ScriptError& err) const {
    err.File = path;
    err.Line = 0;
    sol::error e = result;
    std::string text = e.what();

    // Сообщение Lua начинается с "путь:строка: ". Разбираем его на части, чтобы
    // консоль редактора могла открыть файл на нужной строке.
    const size_t colon = text.rfind(':', text.find(": ") == std::string::npos
                                             ? text.size()
                                             : text.find(": "));
    size_t numEnd = text.find(": ");
    if (numEnd != std::string::npos) {
        size_t numStart = text.rfind(':', numEnd - 1);
        if (numStart != std::string::npos && numStart + 1 < numEnd) {
            const std::string number = text.substr(numStart + 1, numEnd - numStart - 1);
            bool digits = !number.empty();
            for (char c : number) digits = digits && std::isdigit((unsigned char)c);
            if (digits) {
                err.Line = std::atoi(number.c_str());
                err.File = text.substr(0, numStart);
                text = text.substr(numEnd + 2);
            }
        }
    }
    (void)colon;
    if (err.File.empty()) err.File = path;
    err.Message = text;
}

bool Backend::Compile(const ScriptSource& source, sol::protected_function& out, ScriptError& err) {
    auto it = m_chunks.find(source.Path);
    if (it != m_chunks.end() && it->second.Stamp == source.Stamp && it->second.Fn.valid()) {
        out = it->second.Fn;
        return true;
    }
    // Имя куска с '@' — чтобы ошибки внутри ссылались на файл и строку, а не на
    // «[string "..."]»: номер строки в чужом безымянном куске бесполезен.
    sol::load_result chunk = m_lua->load(source.Text, "@" + source.Path);
    if (!chunk.valid()) {
        sol::error e = chunk;
        err.File = source.Path;
        err.Message = e.what();
        // Синтаксическая ошибка тоже несёт "файл:строка:" — разбираем так же.
        const size_t numEnd = err.Message.find(": ");
        if (numEnd != std::string::npos) {
            const size_t numStart = err.Message.rfind(':', numEnd - 1);
            if (numStart != std::string::npos) {
                const std::string number = err.Message.substr(numStart + 1, numEnd - numStart - 1);
                bool digits = !number.empty();
                for (char c : number) digits = digits && std::isdigit((unsigned char)c);
                if (digits) {
                    err.Line = std::atoi(number.c_str());
                    err.Message = err.Message.substr(numEnd + 2);
                }
            }
        }
        return false;
    }
    Chunk cached;
    cached.Fn = chunk.get<sol::protected_function>();
    cached.Stamp = source.Stamp;
    m_chunks[source.Path] = cached;
    out = cached.Fn;
    return true;
}

void Backend::FillHooks(Instance& inst) {
    for (size_t i = 0; i < (size_t)Hook::Count; ++i) {
        const Hook hook = (Hook)i;
        sol::object fn = inst.Class[HookName(hook)];
        if (!fn.valid() && inst.Legacy) {
            if (const char* legacy = LegacyHookName(hook)) fn = inst.Class[legacy];
        }
        if (fn.get_type() == sol::type::function)
            inst.Hooks[i] = fn.as<sol::protected_function>();
        else
            inst.Hooks[i] = sol::protected_function();
    }
}

InstanceId Backend::Create(const ScriptSource& source, GameObject owner, ScriptError& err) {
    sol::protected_function chunk;
    if (!Compile(source, chunk, err)) return kInvalidInstance;

    // Своё окружение у каждого экземпляра: глобальная переменная одного скрипта
    // не имеет права быть видна другому, иначе два врага на уровне делят одно
    // состояние и ведут себя как один.
    sol::environment env(*m_lua, sol::create, m_lua->globals());
    sol::set_environment(env, chunk);
    sol::protected_function_result result = chunk();
    if (!result.valid()) {
        Explain(result, source.Path, err);
        return kInvalidInstance;
    }

    Instance inst;
    inst.Owner = owner;
    inst.Env = env;
    sol::object returned = result.get<sol::object>();
    if (returned.get_type() == sol::type::table) {
        inst.Class = returned.as<sol::table>();
    } else {
        // Старый стиль: файл ничего не вернул, а объявил глобальные функции.
        inst.Class = env;
        inst.Legacy = true;
    }

    sol::protected_function_result made = m_makeInstance(inst.Class, m_base);
    if (!made.valid()) {
        Explain(made, source.Path, err);
        return kInvalidInstance;
    }
    inst.Self = made.get<sol::table>();
    inst.Self["gameObject"] = Wrap(owner);
    inst.Self["transform"] = TransformRef(owner);
    inst.Self["name"] = owner.Valid() ? owner.Name() : std::string();
    if (m_interop)
        inst.LegacyEntity = sol::make_object(*m_lua, owner); // прежний usertype GameObject
    else
        inst.LegacyEntity = inst.Self["gameObject"];
    // `self` внутри файла старого стиля — СВОЯ сущность, как и было: на этом
    // стоят все написанные до сих пор игры, и разойтись здесь значит сломать
    // их молча.
    if (inst.Legacy) env["self"] = inst.LegacyEntity;

    FillHooks(inst);

    const InstanceId id = m_nextId++;
    m_instances[id] = std::move(inst);
    if (owner.Valid()) m_byEntity[(uint32_t)entt::to_integral(owner.Entity())] = id;
    return id;
}

void Backend::Destroy(InstanceId id) {
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    if (it->second.Owner.Valid()) {
        const uint32_t key = (uint32_t)entt::to_integral(it->second.Owner.Entity());
        auto e = m_byEntity.find(key);
        if (e != m_byEntity.end() && e->second == id) m_byEntity.erase(e);
    }
    m_instances.erase(it);
}

bool Backend::Has(InstanceId id, Hook hook) const {
    const Instance* inst = Get(id);
    return inst && inst->Hooks[(size_t)hook].valid();
}

bool Backend::CallHook(InstanceId id, Hook hook, sol::object arg, ScriptError& err) {
    Instance* inst = Get(id);
    if (!inst) return true;
    sol::protected_function& fn = inst->Hooks[(size_t)hook];
    if (!fn.valid()) return true;
    sol::object self = inst->Legacy ? inst->LegacyEntity : sol::object(inst->Self);
    sol::protected_function_result result = fn(self, arg);
    if (result.valid()) return true;
    Explain(result, std::string(), err);
    return false;
}

bool Backend::Call(InstanceId id, Hook hook, float dt, ScriptError& err) {
    return CallHook(id, hook, sol::make_object(*m_lua, dt), err);
}

bool Backend::CallWith(InstanceId id, Hook hook, GameObject other, ScriptError& err) {
    return CallHook(id, hook, Wrap(other), err);
}

bool Backend::CallNamed(InstanceId id, Hook hook, const std::string& name, ScriptError& err) {
    return CallHook(id, hook, sol::make_object(*m_lua, name), err);
}

bool Backend::Invoke(InstanceId id, const std::string& method,
                     const std::vector<sage::vars::Value>& args, ScriptError& err) {
    Instance* inst = Get(id);
    if (!inst) return false;
    sol::object fn = inst->Self[method];
    if (fn.get_type() != sol::type::function) return false;

    std::vector<sol::object> converted;
    converted.reserve(args.size());
    for (const sage::vars::Value& v : args) converted.push_back(ToLua(v));

    sol::protected_function call = fn.as<sol::protected_function>();
    sol::protected_function_result result = call(inst->Self, sol::as_args(converted));
    if (result.valid()) return true;
    Explain(result, std::string(), err);
    return false;
}

void Backend::ApplyFields(InstanceId id, const sage::vars::Table& fields) {
    Instance* inst = Get(id);
    if (!inst) return;
    for (const sage::vars::Var& var : fields.All()) {
        if (var.Name.empty()) continue;
        // Ссылка на КОМПОНЕНТ отдаётся скрипту сразу компонентом: `self.Animator`
        // обязан быть аниматором, а не объектом, из которого его каждый раз
        // достают. Пустая ссылка и объект без такого компонента — честный nil,
        // и скрипт проверяет его обычным `if self.Animator then`.
        if (var.Data.Type() == sage::vars::Kind::Entity && !var.Hint.empty())
            inst->Self[var.Name] = ComponentOf(var.Data.AsEntity(), var.Hint);
        else
            inst->Self[var.Name] = ToLua(var.Data);
    }
}

void Backend::Reset() {
    m_instances.clear();
    m_byEntity.clear();
    m_chunks.clear();
}

sol::object Backend::ScriptOf(GameObject object) {
    if (!object.Valid()) return sol::nil;
    auto it = m_byEntity.find((uint32_t)entt::to_integral(object.Entity()));
    if (it == m_byEntity.end()) return sol::nil;
    Instance* inst = Get(it->second);
    if (!inst) return sol::nil;
    return inst->Self;
}

sol::object Backend::InvokeOn(GameObject object, const std::string& method,
                              sol::variadic_args args) {
    if (!object.Valid()) return sol::nil;
    auto it = m_byEntity.find((uint32_t)entt::to_integral(object.Entity()));
    if (it == m_byEntity.end()) return sol::nil;
    Instance* inst = Get(it->second);
    if (!inst) return sol::nil;
    sol::object fn = inst->Self[method];
    if (fn.get_type() != sol::type::function) return sol::nil;
    sol::protected_function call = fn.as<sol::protected_function>();
    sol::protected_function_result result = call(inst->Self, args);
    if (!result.valid()) {
        sol::error e = result;
        LOG_ERROR("Lua") << method << ": " << e.what();
        return sol::nil;
    }
    return result.get<sol::object>();
}

// --- Мост из прежнего движка -------------------------------------------------

void Backend::DeliverMessage(int targetId, const std::string& name, sol::object data) {
    // Цели собираются ДО вызова: обработчик может слать сообщения, создавать и
    // удалять объекты, а значит — менять сам список экземпляров под ногами.
    struct Target {
        sol::protected_function Fn;
        sol::object Self;
        GameObject Owner;
    };
    std::vector<Target> targets;
    for (auto& [id, inst] : m_instances) {
        if (!inst.Owner.Valid()) continue;
        if (targetId >= 0 && inst.Owner.Id() != targetId) continue;
        sol::object fn = inst.Class["OnMessage"];
        if (fn.get_type() != sol::type::function) continue;
        targets.push_back({fn.as<sol::protected_function>(),
                           inst.Legacy ? inst.LegacyEntity : sol::object(inst.Self), inst.Owner});
    }
    for (Target& t : targets) {
        if (!t.Owner.Valid()) continue; // мог быть уничтожен предыдущим обработчиком
        sol::protected_function_result result = t.Fn(t.Self, name, data);
        if (result.valid()) continue;
        sol::error err = result;
        LOG_ERROR("Lua") << "OnMessage (" << (t.Owner.Valid() ? t.Owner.Name() : std::string("?"))
                         << "): " << err.what();
    }
}

void Backend::DeliverQuit() {
    for (auto& [id, inst] : m_instances) {
        sol::object fn = inst.Class["OnQuit"];
        if (fn.get_type() != sol::type::function) continue;
        sol::protected_function call = fn.as<sol::protected_function>();
        sol::protected_function_result result =
            call(inst.Legacy ? inst.LegacyEntity : sol::object(inst.Self));
        if (result.valid()) continue;
        // Ошибка в OnQuit НЕ мешает выходу: игра уже закрывается, и падать на
        // прощание — худшее, что можно сделать.
        sol::error err = result;
        LOG_ERROR("Lua") << "OnQuit: " << err.what();
    }
}

sol::object Backend::Wrap(GameObject object) {
    if (!object.Valid()) return sol::nil;
    return sol::make_object(*m_lua, ObjectRef(object));
}

std::unique_ptr<LanguageBackend> MakeLuaBackendImpl(const LuaBackendConfig& config) {
    return std::make_unique<Backend>(config);
}

} // namespace sage::scripting::lua

namespace sage::scripting {

std::unique_ptr<LanguageBackend> MakeLuaBackend(const LuaBackendConfig& config) {
    return lua::MakeLuaBackendImpl(config);
}

} // namespace sage::scripting
