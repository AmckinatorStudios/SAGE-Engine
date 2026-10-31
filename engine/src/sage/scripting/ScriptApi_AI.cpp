#include "ScriptApiAIDetail.h"

#include <algorithm>
#include <stdexcept>

#include "lua/LuaInternal.h"
#include "sage/ai/World.h"
#include "sage/core/Log.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// SAGE AI В LUA: ai.task, ai.agent, ai.operation, ai.call, ai.emit, ai.shared.
//
//      ai.task("MoveTo", {
//          inputs  = {target = Vec3(0, 0, 0), speed = 3},
//          outputs = {distance = 0},
//          run = function(t)
//              while not near(t.self, t.inputs.target) do t.wait() end
//              return true
//          end,
//      })
//      local brain = ai.agent(self)
//      local h = brain.start("MoveTo", {target = door})
//
// Задача на Lua — корутина на своём потоке Lua: t.wait(...) отдаёт управление
// миру ИИ, и тот зовёт её снова, только когда ожидание кончилось. Поэтому
// долгая задача пишется линейным кодом и не начинается заново каждый кадр.
//
// Правила выполнения (состояния, дети, отмена, события) — целиком в
// sage::ai::World и одни на C++, Lua и граф: здесь только перевод значений и
// корутина. Вторая копия правил в привязке разошлась бы с первой.
//
// Контекст задачи `t` и ручки выполнения собраны на Lua (kAiLua ниже) поверх
// служебной таблицы __sage_ai: методы через точку (h.cancel(), t.wait(1)) —
// стиль SAGE, а у usertype методы зовутся только двоеточием.
// ---------------------------------------------------------------------------
namespace {

using sage::ai::Blackboard;
using sage::ai::BlackboardPtr;
using sage::ai::RunPtr;
using sage::ai::TaskStatus;
using sage::vars::Value;

} // namespace

namespace sage::scripting::aidetail {

// Значение Lua -> значение ИИ. Объект нового API (self.gameObject) и сам
// экземпляр скрипта (self) становятся ссылкой на объект, как и GameObject.
sage::vars::Value AiValue(ScriptEngine& se, const sol::object& o, const std::string& what) {
    if (o.is<sage::scripting::lua::ObjectRef>()) {
        const auto& r = o.as<sage::scripting::lua::ObjectRef>();
        return Value(sage::vars::EntityRef{r.Obj.Valid() ? r.Obj.Id() : 0});
    }
    if (o.get_type() == sol::type::table) {
        sol::table t = o.as<sol::table>();
        sol::object go = t["gameObject"];
        if (go.valid() && go.get_type() != sol::type::lua_nil) return AiValue(se, go, what);
        // Таблица — список или структура (см. ScriptEngine::TableToValue).
        return se.TableToValue(t, 0);
    }
    if (o.get_type() == sol::type::function)
        throw std::runtime_error(what + ": функцию положить нельзя — в переменных ИИ значения, "
                                        "списки и структуры");
    return se.ValueFromLua(o);
}

int OwnerId(const sol::object& o) {
    if (!o.valid() || o.get_type() == sol::type::lua_nil) return 0;
    if (o.is<GameObject>()) {
        GameObject g = o.as<GameObject>();
        return g.Valid() ? g.Id() : 0;
    }
    if (o.is<sage::scripting::lua::ObjectRef>()) {
        const auto& r = o.as<sage::scripting::lua::ObjectRef>();
        return r.Obj.Valid() ? r.Obj.Id() : 0;
    }
    if (o.get_type() == sol::type::table) return OwnerId(o.as<sol::table>()["gameObject"]);
    if (o.get_type() == sol::type::number) return o.as<int>();
    throw std::runtime_error("ai.agent: хозяин — объект сцены (self) или ничего");
}

} // namespace sage::scripting::aidetail

namespace {

using namespace sage::scripting::aidetail;

// Входы/выходы из таблицы {имя = умолчание}. По имени — порядок записи в
// таблице Lua не определён, а граф и журнал должны видеть одно и то же.
std::vector<sage::ai::Param> ReadParams(ScriptEngine& se, const sol::object& o,
                                        const std::string& task, const char* what) {
    std::vector<sage::ai::Param> out;
    if (!o.valid() || o.get_type() == sol::type::lua_nil) return out;
    if (o.get_type() != sol::type::table)
        throw std::runtime_error("ai.task('" + task + "'): " + what + " — таблица {имя = умолчание}");
    for (const auto& kv : o.as<sol::table>()) {
        if (kv.first.get_type() != sol::type::string)
            throw std::runtime_error("ai.task('" + task + "'): " + what + " — таблица {имя = умолчание}");
        const std::string name = kv.first.as<std::string>();
        out.push_back({name, AiValue(se, kv.second, "ai.task('" + task + "'): " + what + " '" + name + "'")});
    }
    std::sort(out.begin(), out.end(),
              [](const sage::ai::Param& a, const sage::ai::Param& b) { return a.Name < b.Name; });
    return out;
}

Blackboard ReadInputs(ScriptEngine& se, const sol::object& o, const std::string& task) {
    Blackboard b;
    if (!o.valid() || o.get_type() == sol::type::lua_nil) return b;
    if (o.get_type() != sol::type::table)
        throw std::runtime_error("задача '" + task + "': входы — таблица {имя = значение}");
    for (const auto& kv : o.as<sol::table>()) {
        if (kv.first.get_type() != sol::type::string)
            throw std::runtime_error("задача '" + task + "': входы — таблица {имя = значение}");
        const std::string name = kv.first.as<std::string>();
        b.Set(name, AiValue(se, kv.second, "задача '" + task + "', вход '" + name + "'"));
    }
    return b;
}

// --- Задача на Lua ----------------------------------------------------------
struct LuaTaskSpec {
    sol::protected_function Run;
    sol::protected_function Cancel;
    sol::protected_function MakeCtx;
};

class LuaTaskBehavior final : public sage::ai::TaskBehavior {
public:
    explicit LuaTaskBehavior(std::shared_ptr<LuaTaskSpec> spec) : m_spec(std::move(spec)) {}

    TaskStatus Step(sage::ai::TaskContext& ctx) override {
        sage::ai::Run& run = ctx.GetRun();
        if (m_started) {
            auto r = m_co();
            return Conclude(run, r);
        }
        m_started = true;
        // Свой поток Lua на каждое выполнение — по той же причине, что у
        // StartCoroutine (ScriptApi_Runtime.cpp): корутина, построенная прямо
        // из функции главного состояния, резюмилась бы на нём, и две задачи
        // продолжали бы одна другую.
        m_thread = sol::thread::create(m_spec->Run.lua_state());
        lua_State* ts = m_thread.state().lua_state();
        m_spec->Run.push();
        lua_xmove(m_spec->Run.lua_state(), ts, 1);
        m_co = sol::coroutine(ts, -1);
        // Корутина взяла на функцию свою ссылку; сама функция со стека потока
        // уходит. Останься она там — sol посчитал бы её первым результатом
        // задачи, и `return false` читался бы как успех.
        lua_pop(ts, 1);

        auto made = m_spec->MakeCtx(AiRunRef{ctx.GetWorld().FindRun(run.Id)}, run.Agent);
        if (!made.valid()) {
            sol::error e = made;
            return ctx.Fail(e.what());
        }
        m_ctx = made.get<sol::object>();
        auto r = m_co(m_ctx);
        return Conclude(run, r);
    }

    void Cancel(sage::ai::TaskContext& ctx) override {
        if (!m_started || !m_spec->Cancel.valid()) return;
        auto r = m_spec->Cancel(m_ctx);
        if (!r.valid()) {
            sol::error e = r;
            LOG_ERROR("AI") << "Отмена задачи '" << ctx.GetRun().Task << "': " << e.what();
        }
    }

private:
    TaskStatus Conclude(sage::ai::Run& run, sol::protected_function_result& r) {
        if (!r.valid()) {
            sol::error e = r;
            run.Error = e.what();
            // Ошибка скрипта — это не «не смогла дойти», а дефект: в журнал.
            LOG_ERROR("AI") << "Задача '" << run.Task << "': " << run.Error;
            return TaskStatus::Failed;
        }
        if (run.FailRequested) return TaskStatus::Failed;
        if (r.status() == sol::call_status::yielded) return TaskStatus::Running;
        if (r.return_count() > 0) {
            sol::object v = r.get<sol::object>(0);
            if (v.is<bool>() && !v.as<bool>()) {
                if (r.return_count() > 1) {
                    sol::object why = r.get<sol::object>(1);
                    if (why.is<std::string>()) run.Error = why.as<std::string>();
                }
                if (run.Error.empty()) run.Error = "задача вернула false";
                return TaskStatus::Failed;
            }
        }
        return TaskStatus::Succeeded;
    }

    std::shared_ptr<LuaTaskSpec> m_spec;
    sol::thread m_thread;
    sol::coroutine m_co;
    sol::object m_ctx;
    bool m_started = false;
};

class LuaTaskDefinition final : public sage::ai::TaskDefinition {
public:
    explicit LuaTaskDefinition(std::shared_ptr<LuaTaskSpec> spec) : m_spec(std::move(spec)) {}
    std::unique_ptr<sage::ai::TaskBehavior> Create() override {
        return std::make_unique<LuaTaskBehavior>(m_spec);
    }

private:
    std::shared_ptr<LuaTaskSpec> m_spec;
};

// Ждать можно только из шага своей задачи: из обработчика события или чужой
// задачи «ожидание» молча назначилось бы не тому.
sage::ai::Run& Stepping(AiRunRef& r, const char* who) {
    if (!r.R || !r.R->InStep || r.R->Status != TaskStatus::Running)
        throw std::runtime_error(std::string(who) + ": звать только внутри run своей задачи");
    return *r.R;
}

// Контекст задачи и ручки — на Lua. Служебная таблица N — __sage_ai.
const char* kAiLua = R"LUA(
local N = __sage_ai
local yield, setmetatable, ipairs, error, tostring = coroutine.yield, setmetatable, ipairs, error, tostring

-- Ручка выполнения -> выполнение в C++. Слабые ключи: ручку забыли — запись ушла.
local RUN = setmetatable({}, {__mode = "k"})

local function handle(r)
    local h = {}
    RUN[h] = r
    h.cancel = function() return N.cancel(r) end
    return setmetatable(h, {
        __index = function(_, k) return N.field(r, k) end,
        __newindex = function(_, k) error("у выполнения задачи нельзя задать '" .. tostring(k) .. "'", 2) end,
    })
end

N.handle = handle

local function runs_of(list)
    local out = {}
    for i, h in ipairs(list) do
        out[i] = RUN[h] or error("ждать можно только выполнение задачи (то, что вернул start)", 3)
    end
    return out
end

-- Экземпляры по номеру. Слабые значения: таблицу держит тот, кто её создал, и
-- каждая идущая задача (через t.agent); забыли все — соберётся заново.
local AGENTS = setmetatable({}, {__mode = "v"})

function N.make_agent(id, owner, space)
    local a = {id = id, owner = owner, memory = N.memory(id), shared = N.shared(space)}
    a.start = function(name, inputs) return handle(N.start(id, name, inputs, nil)) end
    a.stop = function() N.stop(id) end
    a.destroy = function() AGENTS[id] = nil; N.destroy(id) end
    a.emit = function(event, data) return N.emit(event, data, id) end
    a.running = function() return N.running(id) end
    AGENTS[id] = a
    return a
end

local function agent_of(id)
    return AGENTS[id] or N.make_agent(id, N.owner(id), N.space(id))
end
N.agent_table = agent_of

function N.make_ctx(r, id)
    local a = agent_of(id)
    local t = {
        inputs = N.field(r, "inputs"), outputs = N.field(r, "outputs"), locals = N.field(r, "locals"),
        instance = a.memory, shared = a.shared, agent = a, self = a.owner, task = N.field(r, "task"),
    }
    function t.wait(seconds)
        if seconds then N.wait_time(r, seconds) else N.wait_frame(r) end
        yield()
    end
    function t.wait_until(fn, timeout)
        local limit = timeout and (N.time() + timeout)
        while not fn() do
            if limit and N.time() >= limit then return false end
            N.wait_frame(r)
            yield()
        end
        return true
    end
    function t.wait_event(name, timeout)
        N.wait_event(r, name, timeout or 0)
        yield()
        return N.event(r)
    end
    function t.start(name, inputs) return handle(N.start(id, name, inputs, r)) end
    function t.wait_for(h)
        N.wait_runs(r, runs_of({h}), true)
        yield()
        return h.ok
    end
    function t.wait_all(...)
        local list = {...}
        N.wait_runs(r, runs_of(list), true)
        yield()
        for _, h in ipairs(list) do if not h.ok then return false end end
        return true
    end
    function t.wait_any(...)
        local list = {...}
        N.wait_runs(r, runs_of(list), false)
        yield()
        for _, h in ipairs(list) do if h.done then return h end end
    end
    function t.run(name, inputs)
        local h = t.start(name, inputs)
        t.wait_for(h)
        return h.ok, h
    end
    function t.fail(reason)
        N.fail(r, reason)
        yield()
    end
    function t.emit(event, data, target) return N.emit(event, data, target) end
    t.call = sage.ai.call
    return t
end
)LUA";

} // namespace

sage::ai::World& ScriptEngine::AI() {
    if (!m_ai) m_ai = std::make_unique<sage::ai::World>();
    return *m_ai;
}

void ScriptEngine::RegisterAiApi() {
    using sage::ai::Run;
    sage::ai::World& world = AI();
    // Объект уничтожили — его ИИ уходит вместе с задачами. Без сцены (тест,
    // инструмент) хозяева считаются живыми: судить не по чему.
    world.SetOwnerAlive([this](int id) { return !m_scene || m_scene->Get(id).Valid(); });

    m_lua.new_usertype<AiRunRef>("AiRun", sol::no_constructor);
    m_lua.new_usertype<AiVars>(
        "AiVars", sol::no_constructor,
        sol::meta_function::index,
        [this](AiVars& v, const std::string& name) -> sol::object {
            const Value* x = v.B->Find(name);
            return x ? ValueToLua(*x) : sol::object(sol::lua_nil);
        },
        sol::meta_function::new_index,
        [this](AiVars& v, const std::string& name, sol::object value) {
            if (!value.valid() || value.get_type() == sol::type::lua_nil) {
                v.B->Erase(name);
                return;
            }
            std::string err;
            if (!v.B->Set(name, AiValue(*this, value, "переменная '" + name + "'"), &err)) {
                std::string known;
                for (const std::string& n : v.B->Names()) known += (known.empty() ? "" : ", ") + n;
                throw std::runtime_error("переменная ИИ: " + err + " (объявлены: " + known + ")");
            }
        });

    sol::table N = m_lua.create_named_table("__sage_ai");
    m_aiNative = N;

    N["start"] = [this](int agent, const std::string& name, sol::object inputs, sol::object parent) {
        int parentId = 0;
        if (parent.is<AiRunRef>()) parentId = parent.as<AiRunRef>().R->Id;
        RunPtr run = AI().Start(agent, name, ReadInputs(*this, inputs, name), parentId);
        if (run->Rejected) throw std::runtime_error("ai: " + run->Error);
        return AiRunRef{run};
    };
    N["cancel"] = [this](AiRunRef& r) { return AI().Cancel(r.R->Id); };
    N["field"] = [this](AiRunRef& r, const std::string& k) -> sol::object {
        const Run& x = *r.R;
        if (k == "id") return sol::make_object(m_lua, x.Id);
        if (k == "task") return sol::make_object(m_lua, x.Task);
        if (k == "status") return sol::make_object(m_lua, std::string(sage::ai::StatusName(x.Status)));
        if (k == "done") return sol::make_object(m_lua, sage::ai::IsFinished(x.Status));
        if (k == "ok") return sol::make_object(m_lua, x.Status == TaskStatus::Succeeded);
        if (k == "error") return x.Error.empty() ? sol::object(sol::lua_nil) : sol::make_object(m_lua, x.Error);
        if (k == "inputs") return sol::make_object(m_lua, AiVars{x.Inputs});
        if (k == "outputs") return sol::make_object(m_lua, AiVars{x.Outputs});
        if (k == "locals") return sol::make_object(m_lua, AiVars{x.Locals});
        if (k == "agent") return sol::make_object(m_lua, x.Agent);
        throw std::runtime_error("у выполнения задачи нет поля '" + k +
                                 "' (есть: id, task, status, done, ok, error, inputs, outputs, locals, "
                                 "cancel)");
    };
    N["wait_frame"] = [this](AiRunRef& r) {
        Run& run = Stepping(r, "t.wait");
        sage::ai::TaskContext(AI(), run).WaitFrame();
    };
    N["wait_time"] = [this](AiRunRef& r, float seconds) {
        Run& run = Stepping(r, "t.wait");
        sage::ai::TaskContext(AI(), run).WaitSeconds(seconds);
    };
    N["wait_event"] = [this](AiRunRef& r, const std::string& name, float timeout) {
        Run& run = Stepping(r, "t.wait_event");
        sage::ai::TaskContext(AI(), run).WaitEvent(name, timeout);
    };
    N["wait_runs"] = [this](AiRunRef& r, sol::table list, bool all) {
        Run& run = Stepping(r, "t.wait_for");
        std::vector<RunPtr> runs;
        for (const auto& kv : list)
            if (kv.second.is<AiRunRef>()) runs.push_back(kv.second.as<AiRunRef>().R);
        sage::ai::TaskContext(AI(), run).WaitRuns(std::move(runs), all);
    };
    N["fail"] = [this](AiRunRef& r, sol::optional<std::string> reason) {
        Run& run = Stepping(r, "t.fail");
        sage::ai::TaskContext(AI(), run).Fail(reason.value_or("задача не удалась"));
    };
    N["event"] = [this](AiRunRef& r) -> std::tuple<bool, sol::object> {
        const Run& run = *r.R;
        if (run.TimedOut) return {false, sol::lua_nil};
        return {true, ValueToLua(run.EventPayload)};
    };
    N["time"] = [this]() { return AI().Time(); };
    N["memory"] = [this](int id) -> sol::object {
        sage::ai::Agent* a = AI().FindAgent(id);
        if (!a) throw std::runtime_error("ai: нет экземпляра ИИ #" + std::to_string(id));
        return sol::make_object(m_lua, AiVars{a->Memory});
    };
    N["shared"] = [this](const std::string& space) { return AiVars{AI().Shared(space)}; };
    N["owner"] = [this](int id) -> sol::object {
        sage::ai::Agent* a = AI().FindAgent(id);
        if (!a || a->Owner <= 0) return sol::lua_nil;
        return ValueToLua(Value(sage::vars::EntityRef{a->Owner}));
    };
    N["space"] = [this](int id) -> std::string {
        sage::ai::Agent* a = AI().FindAgent(id);
        return a ? a->SharedSpace : "world";
    };
    N["stop"] = [this](int id) { AI().StopAgent(id); };
    N["destroy"] = [this](int id) { AI().DestroyAgent(id); };
    N["running"] = [this](int id) -> int {
        sage::ai::Agent* a = AI().FindAgent(id);
        return a ? (int)a->Runs.size() : 0;
    };
    auto emit = [this](const std::string& event, sol::object data, sol::object target) -> int {
        int agent = 0;
        if (target.get_type() == sol::type::number) agent = target.as<int>();
        else if (target.get_type() == sol::type::table) agent = target.as<sol::table>().get_or("id", 0);
        const Value payload = (!data.valid() || data.get_type() == sol::type::lua_nil)
                                  ? Value()
                                  : AiValue(*this, data, "данные события '" + event + "'");
        return AI().Emit(event, payload, agent);
    };
    N["emit"] = emit;

    // --- Открытое API (подсказка редактора собирается из этих Bind) --------

    // Описать задачу: ai.task("MoveTo", {inputs = {...}, outputs = {...}, run = fn, cancel = fn}).
    // То же имя второй раз — замена (горячая перезагрузка скрипта).
    Bind("ai", "task", nullptr, [this](const std::string& name, sol::table spec) {
        if (name.empty()) throw std::runtime_error("ai.task: у задачи должно быть имя");
        for (const auto& kv : spec) {
            const std::string k = kv.first.is<std::string>() ? kv.first.as<std::string>() : "?";
            if (k != "run" && k != "cancel" && k != "inputs" && k != "outputs")
                throw std::runtime_error("ai.task('" + name + "'): неизвестное поле '" + k +
                                         "' (есть: run, cancel, inputs, outputs)");
        }
        sol::object run = spec["run"];
        if (run.get_type() != sol::type::function)
            throw std::runtime_error("ai.task('" + name + "'): нужна функция run(t)");
        auto ls = std::make_shared<LuaTaskSpec>();
        ls->Run = run.as<sol::protected_function>();
        sol::object cancel = spec["cancel"];
        if (cancel.get_type() == sol::type::function) ls->Cancel = cancel.as<sol::protected_function>();
        ls->MakeCtx = m_aiNative["make_ctx"];
        auto def = std::make_shared<LuaTaskDefinition>(ls);
        def->Name = name;
        def->Origin = "Lua";
        def->Signature.Inputs = ReadParams(*this, spec["inputs"], name, "inputs");
        def->Signature.Outputs = ReadParams(*this, spec["outputs"], name, "outputs");
        AI().RegisterTask(def);
    });

    // Экземпляр ИИ: ai.agent(self, {shared = "guards"}). Хозяин необязателен.
    Bind("ai", "agent", nullptr, [this](sol::object owner, sol::optional<sol::table> opts) -> sol::table {
        std::string space = "world";
        if (opts) space = opts->get_or<std::string>("shared", "world");
        const int id = AI().CreateAgent(OwnerId(owner), space);
        sol::protected_function make = m_aiNative["make_agent"];
        auto made = make(id, owner, space);
        if (!made.valid()) {
            sol::error e = made;
            throw std::runtime_error(e.what());
        }
        return made.get<sol::table>();
    });

    // Разбудить ждущих события: ai.emit("noise", where, guard). Без адресата — всех.
    Bind("ai", "emit", nullptr, [emit](const std::string& event, sol::object data, sol::object target) -> int {
        return emit(event, data, target);
    });

    // Общие данные области: ai.shared("guards").alarm = true. Без имени — "world".
    Bind("ai", "shared", nullptr, [this](sol::optional<std::string> space) {
        return AiVars{AI().Shared(space.value_or("world"))};
    });

    // Своя операция: ai.operation("distance", fn, {inputs = {{"a", Vec3()}, {"b", Vec3()}}}).
    // Порядок входов важен (операцию зовут по позициям), поэтому — список пар.
    Bind("ai", "operation", nullptr, [this](const std::string& name, sol::protected_function fn,
                                            sol::optional<sol::table> spec) {
        sage::ai::Operation op;
        op.Name = name;
        op.Origin = "Lua";
        auto pairs = [&](const char* key) {
            std::vector<sage::ai::Param> out;
            if (!spec) return out;
            sol::object o = (*spec)[key];
            if (o.get_type() != sol::type::table) return out;
            for (const auto& kv : o.as<sol::table>()) {
                if (kv.second.get_type() != sol::type::table)
                    throw std::runtime_error("ai.operation('" + name + "'): " + key + " — список {имя, умолчание}");
                sol::table p = kv.second;
                out.push_back({p.get_or<std::string>(1, ""),
                               AiValue(*this, p[2], "ai.operation('" + name + "')")});
            }
            return out;
        };
        op.Inputs = pairs("inputs");
        op.Outputs = pairs("outputs");
        op.Fn = [this, fn](const sage::ai::Values& in, sage::ai::Values& out, std::string& err) {
            std::vector<sol::object> args;
            for (const Value& v : in) args.push_back(ValueToLua(v));
            auto r = fn(sol::as_args(args));
            if (!r.valid()) {
                sol::error e = r;
                err = e.what();
                return false;
            }
            for (int i = 0; i < r.return_count(); ++i)
                out.push_back(AiValue(*this, r.get<sol::object>(i), "результат операции"));
            return true;
        };
        AI().RegisterOperation(std::move(op));
    });

    // Вызвать операцию по имени — всё равно, на чём она написана: ai.call("distance", a, b).
    Bind("ai", "call", nullptr, [this](const std::string& name, sol::variadic_args args) -> sol::variadic_results {
        sage::ai::Values in, out;
        for (auto a : args) in.push_back(AiValue(*this, a, "ai.call('" + name + "')"));
        std::string err;
        if (!AI().Call(name, in, out, err)) throw std::runtime_error("ai.call('" + name + "'): " + err);
        sol::variadic_results res;
        for (const Value& v : out) res.push_back(ValueToLua(v));
        return res;
    });

    // Имена описанных задач и операций — для отладки и своих инструментов.
    Bind("ai", "tasks", nullptr, [this]() -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const std::string& n : AI().TaskNames()) t[i++] = n;
        return t;
    });
    Bind("ai", "operations", nullptr, [this]() -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const std::string& n : AI().OperationNames()) t[i++] = n;
        return t;
    });
    // Имена переменных области: ai.names(t.instance).
    Bind("ai", "names", nullptr, [this](AiVars& vars) -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const std::string& n : vars.B->Names()) t[i++] = n;
        return t;
    });

    sol::protected_function_result ok = m_lua.safe_script(kAiLua, sol::script_pass_on_error);
    if (!ok.valid()) {
        sol::error e = ok;
        LOG_ERROR("AI") << "Не удалось собрать Lua-часть SAGE AI: " << e.what();
    }
    RegisterAiGraphApi();
    RegisterAiWorldNodes();
    RegisterVisualApi();
}
