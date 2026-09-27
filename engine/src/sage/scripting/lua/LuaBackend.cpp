#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptFields.h"

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
// (GetComponent, get_component, …), затем — компонент по имени типа
// (`self.Audio`). Метатаблица, а не копирование методов в каждый экземпляр:
// копия базы на сотне объектов — это сотня одинаковых таблиц в памяти и сотня
// мест, где можно разойтись.
//
// Таблица self создаётся ДО выполнения файла (простой скрипт видит `self` уже
// на верхнем уровне), а метатаблица ставится после, когда ясен вид скрипта.
// У простого скрипта «класс» — его окружение: из него self отдаёт только
// ЗНАЧЕНИЯ (`other:GetScript().speed`), а не функции — иначе `self:Destroy()`
// позвал бы хук Destroy() вместо уничтожения объекта.
constexpr const char* kInstanceFactory = R"LUA(
return function(self, class, base, simple)
    setmetatable(self, {
        __index = function(t, key)
            local v
            if simple then
                v = rawget(class, key)
                if v ~= nil and type(v) ~= "function" then return v end
            else
                v = class[key]
                if v ~= nil then return v end
            end
            v = base[key]
            if v ~= nil then return v end
            local go = rawget(t, "gameObject")
            if go == nil then return nil end
            if key == "name" then return go.name end
            -- self.Audio, self.Camera: компонент по имени типа; нет — nil.
            if type(key) == "string" and key:match("^%u") then return go:GetComponent(key) end
            return nil
        end
    })
    return self
end
)LUA";

// Старый стиль глобальных функций: OnStart/OnUpdate или первый параметр
// `entity` у On-функции. Простой (нынешний) — всё остальное без `return`.
bool IsLegacySource(const std::string& text) {
    static const char* kOnly[] = {"OnStart", "OnUpdate", "OnFixedUpdate", "OnLateUpdate"};
    for (const char* name : kOnly) {
        const std::string decl = std::string("function ") + name;
        size_t at = text.find(decl);
        while (at != std::string::npos) {
            const size_t end = at + decl.size();
            if (end < text.size() && !std::isalnum((unsigned char)text[end]) && text[end] != '_') return true;
            at = text.find(decl, end);
        }
    }
    size_t at = text.find("function On");
    while (at != std::string::npos) {
        const size_t paren = text.find('(', at);
        if (paren == std::string::npos) break;
        size_t j = paren + 1;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (text.compare(j, 6, "entity") == 0) return true;
        at = text.find("function On", paren);
    }
    return false;
}

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

    m_signals = std::make_unique<LuaHandlerStore>(
        [this](const sol::protected_function& fn, const sage::events::Event& e, int owner) {
            CallHandler(fn, e, owner);
        });

    m_base = m_lua->create_table();
    RegisterMath(*this);
    RegisterObject(*this);
    RegisterComponents(*this);
    RegisterGlobals(*this);
    RegisterSignals(*this);
    RegisterTest(*this);
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
            -- Сигналы своего объекта: self:emit("player_died") = self.gameObject:emit(...)
            function base:on(name, fn) return self.gameObject:on(name, fn) end
            function base:once(name, fn) return self.gameObject:once(name, fn) end
            function base:emit(name, data) return self.gameObject:emit(name, data) end
            function base:signal(name) return self.gameObject:signal(name) end
            -- Те же действия нынешними именами: self:get_component("Audio").
            function base:get_component(name) return self.gameObject:GetComponent(name) end
            function base:add_component(name, opts) return self.gameObject:AddComponent(name, opts) end
            function base:has_component(name) return self.gameObject:HasComponent(name) end
            function base:remove_component(name) return self.gameObject:RemoveComponent(name) end
            function base:destroy() return self.gameObject:Destroy() end
            function base:call(method, ...) return self.gameObject:Call(method, ...) end
        end
    )LUA").get<sol::protected_function>()(m_base);
}

Backend::~Backend() {
    // Обработчик шины, переживший бэкенд, видит пустой жетон и молчит.
    *m_tapToken = nullptr;
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
    if (it != m_chunks.end() && it->second.Stamp == source.Stamp && !it->second.Bytecode.empty()) {
        sol::load_result fresh = m_lua->load(it->second.Bytecode, "@" + source.Path, sol::load_mode::binary);
        if (fresh.valid()) {
            out = fresh.get<sol::protected_function>();
            return true;
        }
        m_chunks.erase(it);   // байткод не принялся — собираем из текста заново
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
    out = chunk.get<sol::protected_function>();
    Chunk cached;
    cached.Stamp = source.Stamp;
    // Без отладочной информации байткод терял бы имена файлов и номера строк в
    // ошибках — а ради них ошибки и разбираются (strip = false).
    sol::bytecode bc = out.dump();
    cached.Bytecode.assign(bc.as_string_view().begin(), bc.as_string_view().end());
    m_chunks[source.Path] = std::move(cached);
    return true;
}

void Backend::FillHooks(Instance& inst) {
    for (size_t i = 0; i < (size_t)Hook::Count; ++i) {
        const Hook hook = (Hook)i;
        // Простой скрипт — только СВОИ функции (raw): окружение смотрит в
        // глобальные, и глобальная функция движка с именем хука стала бы его
        // хуком молча.
        sol::object fn = inst.Simple ? inst.Class.raw_get<sol::object>(HookName(hook))
                                     : sol::object(inst.Class[HookName(hook)]);
        // Destroy() — нынешнее имя OnDestroy у простого скрипта.
        if (inst.Simple && hook == Hook::OnDestroy && fn.get_type() != sol::type::function)
            fn = inst.Class.raw_get<sol::object>("Destroy");
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

    // Номер — ДО выполнения файла: код верхнего уровня уже может подписаться
    // (`ui.play.clicked:connect(...)` прямо в теле), и подписка обязана
    // принадлежать этому экземпляру, а не «никому».
    const InstanceId id = m_nextId++;
    CurrentScope scope(*this, id);
    // Файл не собрался до конца — всё, что он успел подписать, снимается:
    // обработчики недособранного экземпляра звали бы код, которого нет.
    auto abandon = [&] {
        if (Scene* scene = ScenePtr()) scene->Events.DisconnectOwner(m_signals->Group(), (int)id);
        return kInvalidInstance;
    };

    // Своё окружение у каждого экземпляра: глобальная переменная одного скрипта
    // не имеет права быть видна другому, иначе два врага на уровне делят одно
    // состояние и ведут себя как один.
    sol::environment env(*m_lua, sol::create, m_lua->globals());
    // self — ДО выполнения файла: простой скрипт вправе обратиться к своему
    // объекту уже на верхнем уровне (`local body = self.transform`).
    sol::table selfTable = m_lua->create_table();
    selfTable["gameObject"] = Wrap(owner);
    selfTable["game_object"] = selfTable["gameObject"];
    selfTable["transform"] = owner.Valid() ? sol::make_object(*m_lua, TransformRef(owner)) : sol::object(sol::nil);
    env["self"] = selfTable;
    sol::set_environment(env, chunk);
    sol::protected_function_result result = chunk();
    if (!result.valid()) {
        Explain(result, source.Path, err);
        return abandon();
    }

    Instance inst;
    inst.Owner = owner;
    inst.Env = env;
    inst.Methods = sage::scripting::ParseMethods(source.Path, source.Text);
    sol::object returned = result.get<sol::object>();
    if (returned.get_type() == sol::type::table) {
        inst.Class = returned.as<sol::table>();
    } else {
        // Файл ничего не вернул, а объявил глобальные функции. Старый стиль
        // (OnUpdate(entity, dt)) или нынешний простой (Update(dt)) — по тексту.
        inst.Class = env;
        if (IsLegacySource(source.Text)) inst.Legacy = true;
        else inst.Simple = true;
    }

    sol::protected_function_result made = m_makeInstance(selfTable, inst.Class, m_base, inst.Simple);
    if (!made.valid()) {
        Explain(made, source.Path, err);
        return abandon();
    }
    inst.Self = selfTable;
    if (m_interop)
        inst.LegacyEntity = sol::make_object(*m_lua, owner); // прежний usertype GameObject
    else
        inst.LegacyEntity = inst.Self["gameObject"];
    // `self` внутри файла старого стиля — СВОЯ сущность, как и было: на этом
    // стоят все написанные до сих пор игры, и разойтись здесь значит сломать
    // их молча.
    if (inst.Legacy) env["self"] = inst.LegacyEntity;

    FillHooks(inst);

    m_instances[id] = std::move(inst);
    EnsureEventTap();
    if (owner.Valid()) m_byEntity[(uint32_t)entt::to_integral(owner.Entity())] = id;
    return id;
}

void Backend::Destroy(InstanceId id) {
    auto it = m_instances.find(id);
    if (it == m_instances.end()) return;
    // Подписки уничтоженного скрипта уходят вместе с ним: иначе кнопка,
    // нажатая после удаления объекта со скриптом меню, звала бы его код.
    if (Scene* scene = ScenePtr()) scene->Events.DisconnectOwner(m_signals->Group(), (int)id);
    // Запись индекса снимается и у УЖЕ уничтоженного объекта: номер сущности
    // entt переиспользует, и новая сущность с тем же номером иначе «нашла»
    // бы чужой скрипт.
    {
        const uint32_t key = (uint32_t)entt::to_integral(it->second.Owner.Entity());
        auto e = m_byEntity.find(key);
        if (e != m_byEntity.end() && e->second == id) m_byEntity.erase(e);
    }
    m_instances.erase(it);
}

void Backend::DropDeadSubscriptions() {
    Scene* scene = ScenePtr();
    if (!scene) return;
    for (const auto& [id, inst] : m_instances)
        if (inst.Owner.Registry() && !inst.Owner.Valid())
            scene->Events.DisconnectOwner(m_signals->Group(), (int)id);
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
    CurrentScope scope(*this, id);
    // Простой скрипт получает только то, что относится к событию: Update(dt),
    // OnCollisionEnter(other). Start()/Destroy() — вовсе без аргументов.
    const bool bare = hook == Hook::Start || hook == Hook::OnEnable || hook == Hook::OnDisable ||
                      hook == Hook::OnDestroy;
    sol::protected_function_result result = inst->Simple
        ? (bare ? fn() : fn(arg))
        : CallAs(*inst, fn, arg);
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
    sol::protected_function call = MethodOf(*inst, method);
    if (!call.valid()) return false;

    std::vector<sol::object> converted;
    converted.reserve(args.size());
    for (const sage::vars::Value& v : args) converted.push_back(ToLua(v));

    CurrentScope scope(*this, id);
    sol::protected_function_result result =
        inst->Simple ? call(sol::as_args(converted)) : call(inst->Self, sol::as_args(converted));
    if (result.valid()) return true;
    Explain(result, std::string(), err);
    return false;
}

bool Backend::HasMethod(InstanceId id, const std::string& method) const {
    const Instance* inst = Get(id);
    if (!inst || method.empty()) return false;
    return MethodOf(*inst, method).valid();
}

sol::protected_function Backend::MethodOf(const Instance& inst, const std::string& name) const {
    // Простой скрипт: своя глобальная функция (raw — не функции движка из
    // общих глобальных). Остальные: метод через self (класс, затем база).
    sol::object fn = inst.Simple ? inst.Class.raw_get<sol::object>(name) : sol::object(inst.Self[name]);
    if (fn.get_type() != sol::type::function) return sol::protected_function();
    return fn.as<sol::protected_function>();
}

bool Backend::InvokeEvent(InstanceId id, const std::string& method,
                          const sage::events::Event& event, ScriptError& err) {
    Instance* inst = Get(id);
    if (!inst) return false;
    sol::protected_function call = MethodOf(*inst, method);
    if (!call.valid()) return false;
    CurrentScope scope(*this, id);
    // Метод, позванный связью, получает ТО ЖЕ, что функция, подписанная
    // кодом: (self, event, data). Скрипт старого стиля — СВОЮ сущность
    // первым аргументом, простой — без self: Open(event, data).
    sol::protected_function_result result = CallAs(*inst, call, EventTable(event), EventData(event));
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
        // Поля простого скрипта — его глобальные переменные (`speed`), а не
        // ключи self: значение из инспектора ложится ПОВЕРХ присваивания в
        // файле (файл уже выполнен, Start ещё нет).
        sol::table target = inst->Simple ? sol::table(inst->Class) : inst->Self;
        if (var.Data.Type() == sage::vars::Kind::Entity && !var.Hint.empty())
            target[var.Name] = ComponentOf(var.Data.AsEntity(), var.Hint);
        else
            target[var.Name] = ToLua(var.Data);
    }
}

void Backend::Reset() {
    // Подписки всех экземпляров — вон из шины, функции — из хранилища: после
    // Reset в шине не должно остаться ни одного обработчика, зовущего код,
    // которого больше нет.
    if (m_signals) m_signals->Clear(ScenePtr() ? &ScenePtr()->Events : nullptr);
    // Корутины тестов держат ссылки в состояние Lua — уходят до экземпляров.
    m_tests.clear();
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
    sol::protected_function call = MethodOf(*inst, method);
    if (!call.valid()) return sol::nil;
    CurrentScope scope(*this, it->second);
    sol::protected_function_result result = inst->Simple ? call(args) : call(inst->Self, args);
    if (!result.valid()) {
        ScriptError err;
        Explain(result, std::string(), err);
        LOG_ERROR("Lua") << err.Format() << " (" << object.Name() << ":call(\"" << method << "\"))";
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
        InstanceId Id;
        bool Simple = false;
    };
    std::vector<Target> targets;
    for (auto& [id, inst] : m_instances) {
        if (!inst.Owner.Valid()) continue;
        if (targetId >= 0 && inst.Owner.Id() != targetId) continue;
        sol::protected_function fn = inst.Simple ? MethodOf(inst, "OnMessage")
                                                 : (inst.Class["OnMessage"].get_type() == sol::type::function
                                                        ? inst.Class["OnMessage"].get<sol::protected_function>()
                                                        : sol::protected_function());
        if (!fn.valid()) continue;
        targets.push_back({fn, inst.Legacy ? inst.LegacyEntity : sol::object(inst.Self), inst.Owner, id,
                           inst.Simple});
    }
    for (Target& t : targets) {
        if (!t.Owner.Valid()) continue; // мог быть уничтожен предыдущим обработчиком
        CurrentScope scope(*this, t.Id);
        sol::protected_function_result result = t.Simple ? t.Fn(name, data) : t.Fn(t.Self, name, data);
        if (result.valid()) continue;
        sol::error err = result;
        LOG_ERROR("Lua") << "OnMessage (" << (t.Owner.Valid() ? t.Owner.Name() : std::string("?"))
                         << "): " << err.what();
    }
}

void Backend::DeliverQuit() {
    for (auto& [id, inst] : m_instances) {
        sol::protected_function call = inst.Simple ? MethodOf(inst, "OnQuit")
                                                   : (inst.Class["OnQuit"].get_type() == sol::type::function
                                                          ? inst.Class["OnQuit"].get<sol::protected_function>()
                                                          : sol::protected_function());
        if (!call.valid()) continue;
        CurrentScope scope(*this, id);
        sol::protected_function_result result = CallAs(inst, call);
        if (result.valid()) continue;
        // Ошибка в OnQuit НЕ мешает выходу: игра уже закрывается, и падать на
        // прощание — худшее, что можно сделать.
        sol::error err = result;
        LOG_ERROR("Lua") << "OnQuit: " << err.what();
    }
}

// --- On<Событие>: глобальные события без подписки ----------------------------
//
// Events.emit("door_open") — и у каждого простого скрипта, где есть функция
// OnDoorOpen(data), она зовётся сама. То же правило, что у OnCollisionEnter:
// «функция с именем события — и есть обработчик». Имя: On + слова события с
// большой буквы («door_open», «door-open», «doorOpen» → OnDoorOpen).
namespace {
std::string EventFunctionName(const std::string& event) {
    std::string out = "On";
    bool upper = true;
    for (char c : event) {
        if (c == '_' || c == '-' || c == ' ' || c == '.' || c == ':') { upper = true; continue; }
        if (!std::isalnum((unsigned char)c)) continue;
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out.size() > 2 ? out : std::string();
}
} // namespace

void Backend::Tick(float dt) {
    (void)dt;
    EnsureEventTap();
}

void Backend::EnsureEventTap() {
    Scene* scene = ScenePtr();
    if (scene == m_tapScene) return;
    // Шина прежней сцены могла уже умереть вместе с ней — снимать с неё нечего
    // и нельзя. Её обработчик всё равно безопасен: он смотрит в жетон.
    m_tapScene = scene;
    m_tapId = 0;
    if (!scene) return;
    std::weak_ptr<Backend*> token = m_tapToken;
    // Подписка остаётся на шине до конца сцены (снять её с шины, которой уже
    // может не быть, нельзя), поэтому обработчик сам проверяет, что бэкенд жив
    // и что эта сцена — всё ещё его.
    m_tapId = scene->Events.OnAny([token, scene](const sage::events::Event& e) {
        std::shared_ptr<Backend*> alive = token.lock();
        if (!alive || !*alive || (*alive)->m_tapScene != scene) return;
        if (e.Object != sage::events::Bus::kGlobal) return;
        (*alive)->CallEventFunctions(e);
    });
}

void Backend::CallEventFunctions(const sage::events::Event& event) {
    const std::string fnName = EventFunctionName(event.Name);
    if (fnName.empty()) return;
    // Цели — до вызовов: обработчик вправе создавать и удалять объекты.
    std::vector<std::pair<InstanceId, sol::protected_function>> targets;
    for (auto& [id, inst] : m_instances) {
        if (!inst.Simple || !inst.Owner.Valid()) continue;
        sol::protected_function fn = MethodOf(inst, fnName);
        if (fn.valid()) targets.emplace_back(id, fn);
    }
    if (targets.empty()) return;
    sol::object data = EventData(event);
    for (auto& [id, fn] : targets) {
        const Instance* inst = Get(id);
        if (!inst || !inst->Owner.Valid()) continue;
        CurrentScope scope(*this, id);
        sol::protected_function_result r = fn(data, EventTable(event));
        if (r.valid()) continue;
        ScriptError err;
        Explain(r, std::string(), err);
        LOG_ERROR("Lua") << err.Format() << " (" << fnName << " у '" << inst->Owner.Name() << "')";
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
