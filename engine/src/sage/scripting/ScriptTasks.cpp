#include "sage/scripting/ScriptTasks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sol/sol.hpp>

#include "sage/core/Log.h"
#include "sage/jobs/Throttle.h"

// Устройство и правила — в ScriptTasks.h. Здесь две вещи: перенос значений
// между состояниями Lua (через сырой C API: на массивах в десятки тысяч чисел
// sol2 на каждый элемент стоил бы дороже самой работы) и пул воркеров.

namespace sage::scripting {

void RegisterTaskNoiseApi(sol::state_view L);   // ScriptApi_Procedural.cpp

namespace {

constexpr int kMaxDepth = 64;

bool ToValue(lua_State* L, int idx, TaskValue& out, std::string& error, int depth, const std::string& path);

bool TableToValue(lua_State* L, int idx, TaskValue& out, std::string& error, int depth, const std::string& path) {
    if (depth > kMaxDepth) {
        error = "таблица глубже " + std::to_string(kMaxDepth) + " уровней (цикл?) в " + path;
        return false;
    }
    idx = lua_absindex(L, idx);
    auto t = std::make_shared<TaskTable>();
    const lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    // Плотная часть: сначала пробуем целые, потом числа, иначе — общий путь.
    bool allInt = n > 0, allNum = n > 0;
    for (lua_Integer i = 1; i <= n && (allInt || allNum); ++i) {
        const int ty = lua_rawgeti(L, idx, i);
        if (ty != LUA_TNUMBER) allInt = allNum = false;
        else if (!lua_isinteger(L, -1)) allInt = false;
        lua_pop(L, 1);
    }
    if (allInt) {
        t->Ints.resize((size_t)n);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            t->Ints[(size_t)(i - 1)] = lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    } else if (allNum) {
        t->Nums.resize((size_t)n);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            t->Nums[(size_t)(i - 1)] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    } else if (n > 0) {
        t->Array.resize((size_t)n);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            const bool ok = ToValue(L, -1, t->Array[(size_t)(i - 1)], error, depth + 1,
                                    path + "[" + std::to_string(i) + "]");
            lua_pop(L, 1);
            if (!ok) return false;
        }
    }
    // Остальные ключи (строки, числа вне 1..n, булевы).
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        bool inArray = false;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            const lua_Integer k = lua_tointeger(L, -2);
            inArray = k >= 1 && k <= n;
        }
        if (!inArray) {
            const int kt = lua_type(L, -2);
            if (kt != LUA_TNUMBER && kt != LUA_TSTRING && kt != LUA_TBOOLEAN) {
                lua_pop(L, 2);
                error = "ключ таблицы не число, не строка и не булево в " + path;
                return false;
            }
            TaskValue key, val;
            std::string keyName = kt == LUA_TSTRING ? lua_tostring(L, -2) : "?";
            // lua_tostring на числовом ключе сломал бы lua_next — имя берём только у строк.
            if (!ToValue(L, -2, key, error, depth + 1, path) ||
                !ToValue(L, -1, val, error, depth + 1, path + "." + keyName)) {
                lua_pop(L, 2);
                return false;
            }
            t->Hash.emplace_back(std::move(key), std::move(val));
        }
        lua_pop(L, 1);
    }
    out.Kind = TaskValue::Type::Table;
    out.T = std::move(t);
    return true;
}

bool ToValue(lua_State* L, int idx, TaskValue& out, std::string& error, int depth, const std::string& path) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL: case LUA_TNONE: out.Kind = TaskValue::Type::Nil; return true;
        case LUA_TBOOLEAN: out.Kind = TaskValue::Type::Bool; out.B = lua_toboolean(L, idx) != 0; return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) { out.Kind = TaskValue::Type::Int; out.I = lua_tointeger(L, idx); }
            else { out.Kind = TaskValue::Type::Num; out.N = lua_tonumber(L, idx); }
            return true;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            out.Kind = TaskValue::Type::Str;
            out.S.assign(s, len);
            return true;
        }
        case LUA_TTABLE: return TableToValue(L, idx, out, error, depth, path);
        default:
            error = std::string("значение типа ") + lua_typename(L, lua_type(L, idx)) +
                    " нельзя передать в другой поток (" + path + ")";
            return false;
    }
}

} // namespace

bool ToTaskValue(lua_State* L, int index, TaskValue& out, std::string& error) {
    return ToValue(L, index, out, error, 0, "значение");
}

void PushTaskValue(lua_State* L, const TaskValue& v) {
    switch (v.Kind) {
        case TaskValue::Type::Nil: lua_pushnil(L); return;
        case TaskValue::Type::Bool: lua_pushboolean(L, v.B ? 1 : 0); return;
        case TaskValue::Type::Int: lua_pushinteger(L, (lua_Integer)v.I); return;
        case TaskValue::Type::Num: lua_pushnumber(L, v.N); return;
        case TaskValue::Type::Str: lua_pushlstring(L, v.S.data(), v.S.size()); return;
        case TaskValue::Type::Table: break;
    }
    const TaskTable& t = *v.T;
    const size_t n = t.Ints.size() + t.Nums.size() + t.Array.size();
    lua_createtable(L, (int)n, (int)t.Hash.size());
    lua_Integer i = 1;
    for (int64_t x : t.Ints) { lua_pushinteger(L, (lua_Integer)x); lua_rawseti(L, -2, i++); }
    for (double x : t.Nums) { lua_pushnumber(L, x); lua_rawseti(L, -2, i++); }
    for (const TaskValue& x : t.Array) { PushTaskValue(L, x); lua_rawseti(L, -2, i++); }
    for (const auto& kv : t.Hash) {
        PushTaskValue(L, kv.first);
        PushTaskValue(L, kv.second);
        lua_rawset(L, -3);
    }
}

// --- Пул воркеров ---------------------------------------------------------------

namespace {

// Короткие имена шума и хэша, как у главного состояния (ScriptApi_ProcGen):
// noise{kind = "simplex", seed = 1, scale = 0.01} и hash(x, y, z, seed).
const char* kTaskPrelude = R"LUA(
local N, M = sage.noise, sage.math
local NOISE_KEYS = { scale = "frequency", kind = "type" }
local function make_noise(opts)
    if type(opts) == "string" then opts = { kind = opts } end
    opts = opts or {}
    local raw = {}
    for k, v in pairs(opts) do raw[NOISE_KEYS[k] or k] = v end
    local g = N.New(raw)
    local o = {
        ridged = function(x, y, z) return g:Ridged(x, y or 0, z or 0) end,
        turbulence = function(x, y, z) return g:Turbulence(x, y or 0, z or 0) end,
        raw = function(x, y, z) return g:Sample(x, y, z) end,
        hash = function(x, y, z) return g:Hash(x, y, z) end,
        grid = function(x, y, z, nx, ny, nz, step, mode) return g:Grid(x, y, z, nx, ny, nz, step, mode) end,
    }
    return setmetatable(o, {
        __call = function(_, x, y, z) return g:Fbm(x, y or 0, z or 0) end,
        __index = function(_, k) return g[NOISE_KEYS[k] or k] end,
        __newindex = function(_, k, v) g[NOISE_KEYS[k] or k] = v end,
    })
end
setmetatable(N, { __call = function(_, opts) return make_noise(opts) end })
noise = N
function hash(x, y, z, seed) return M.Hash(x, y, z, seed) end
)LUA";

struct Job {
    int64_t Id = 0;
    std::string Module, Fn;
    std::vector<TaskValue> Args;
};

struct Finished {
    bool Ok = false;
    std::vector<TaskValue> Results;
    std::string Error;
};

} // namespace

// Состояние Lua потока, на котором идут задачи. Своё у каждого потока
// системы задач, взявшего задачу этого набора: lua_State нельзя делить между
// потоками, а создавать его на каждую задачу — дорого (библиотеки, прелюдия,
// модули). Живёт до Shutdown.
struct TaskLuaState {
    sol::state Lua;
    uint64_t SeenVersion = 0;
    bool Busy = false;
    int Index = 0;
};

struct ScriptTasks::Impl {
    mutable std::mutex Mutex;
    std::unordered_map<int64_t, Finished> Done;
    std::unordered_map<int64_t, bool> Outstanding;   // поставлены и не забраны
    std::unordered_map<std::string, std::pair<std::string, std::string>> Modules;   // имя -> (кусок, текст)
    uint64_t ModulesVersion = 0;
    std::unordered_map<std::thread::id, std::unique_ptr<TaskLuaState>> States;
    std::atomic<int64_t> BusyMicros{0};
    int64_t NextId = 1;
    int Pending = 0;
    int WantWorkers = 0;
    // Не больше Workers() задач разом — в общей системе задач, на долгой
    // полосе (кадру всегда остаются ядра). Своих потоков у задач скриптов
    // больше нет.
    std::unique_ptr<sage::jobs::Throttle> Limiter;

    // По умолчанию — потоков системы задач без одного: кадру остаётся ядро.
    // Сколько из них реально займут долгие задачи разом, всё равно решает
    // система задач (BackgroundLimit).
    static int AutoWorkers() { return std::max(1, sage::jobs::Parallelism() - 1); }

    void EnsureStarted() {
        if (Limiter) return;
        const int n = WantWorkers > 0 ? WantWorkers : AutoWorkers();
        sage::jobs::JobOptions o("script.task", sage::jobs::Category::Scripts, sage::jobs::Lane::Background);
        Limiter = std::make_unique<sage::jobs::Throttle>(n, o);
    }

    std::unique_ptr<TaskLuaState> MakeState(int index) {
        auto st = std::make_unique<TaskLuaState>();
        st->Index = index;
        sol::state& lua = st->Lua;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                           sol::lib::utf8, sol::lib::coroutine, sol::lib::package);
        lua["package"]["path"] = "";
        lua["package"]["cpath"] = "";
        // log из задачи — в общий журнал (он под замком), с номером потока.
        lua["log"] = [index](sol::this_state ts, sol::variadic_args va) {
            lua_State* Ls = ts;
            std::string line;
            for (int i = va.stack_index(); i < va.stack_index() + (int)va.size(); ++i) {
                if (!line.empty()) line += " ";
                size_t len = 0;
                const char* s = luaL_tolstring(Ls, i, &len);
                line.append(s, len);
                lua_pop(Ls, 1);
            }
            LOG_INFO("Task") << "[" << index << "] " << line;
        };
        RegisterTaskNoiseApi(lua);
        lua.safe_script(kTaskPrelude, sol::script_pass_on_error, "tasks");
        // require — только из модулей, которые главный поток прочёл заранее.
        sol::table searchers = lua["package"]["searchers"];
        TaskLuaState* raw = st.get();
        searchers[2] = [this, raw](const std::string& name) -> sol::object {
            sol::state& L = raw->Lua;
            std::string chunk, source;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                auto it = Modules.find(name);
                if (it == Modules.end())
                    return sol::make_object(L, "\n\tмодуль '" + name +
                                                   "' не передан задачам (tasks.preload(\"" + name + "\"))");
                chunk = it->second.first;
                source = it->second.second;
            }
            sol::load_result f = L.load(source, "@" + chunk);
            if (!f.valid()) {
                sol::error e = f;
                return sol::make_object(L, std::string("\n\t") + e.what());
            }
            return sol::make_object(L, f.get<sol::protected_function>());
        };
        return st;
    }

    // Состояние этого потока. Поток, уже выполняющий задачу набора (быть не
    // должно: долгие задачи не берутся «на помощь» при ожидании), получает
    // временное — одно lua_State не входит само в себя.
    TaskLuaState* Acquire(std::unique_ptr<TaskLuaState>& temp) {
        std::lock_guard<std::mutex> lock(Mutex);
        auto& slot = States[std::this_thread::get_id()];
        if (!slot) slot = MakeState((int)States.size() - 1);
        if (slot->Busy) {
            temp = MakeState(-1);
            temp->Busy = true;
            return temp.get();
        }
        slot->Busy = true;
        return slot.get();
    }

    void RunJob(const Job& job) {
        std::unique_ptr<TaskLuaState> temp;
        TaskLuaState* st = Acquire(temp);
        sol::state& lua = st->Lua;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            // Модули обновились (горячая перезагрузка) — забыть загруженные.
            if (st->SeenVersion != ModulesVersion) {
                st->SeenVersion = ModulesVersion;
                sol::table loaded = lua["package"]["loaded"];
                for (const auto& kv : Modules) loaded[kv.first] = sol::lua_nil;
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        Finished fin;
        lua_State* L = lua.lua_state();
        const int top = lua_gettop(L);
        // require(module)[fn](args...) под pcall с трассировкой.
        lua_getglobal(L, "require");
        lua_pushlstring(L, job.Module.data(), job.Module.size());
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            fin.Error = std::string("tasks: модуль '") + job.Module + "': " + lua_tostring(L, -1);
        } else if (!lua_istable(L, -1)) {
            fin.Error = "tasks: модуль '" + job.Module + "' не вернул таблицу функций";
        } else {
            lua_getfield(L, -1, job.Fn.c_str());
            if (!lua_isfunction(L, -1)) {
                fin.Error = "tasks: в модуле '" + job.Module + "' нет функции '" + job.Fn + "'";
            } else {
                for (const TaskValue& a : job.Args) PushTaskValue(L, a);
                const int base = lua_gettop(L) - (int)job.Args.size() - 1;
                if (lua_pcall(L, (int)job.Args.size(), LUA_MULTRET, 0) != LUA_OK) {
                    fin.Error = std::string("tasks: ") + job.Module + "." + job.Fn + ": " + lua_tostring(L, -1);
                } else {
                    const int nres = lua_gettop(L) - base;
                    fin.Ok = true;
                    fin.Results.resize((size_t)nres);
                    for (int r = 0; r < nres && fin.Ok; ++r) {
                        if (!ToTaskValue(L, base + 1 + r, fin.Results[(size_t)r], fin.Error)) {
                            fin.Ok = false;
                            fin.Error = "tasks: " + job.Module + "." + job.Fn + " вернула " + fin.Error;
                        }
                    }
                }
            }
        }
        lua_settop(L, top);
        // Мусор задачи — сразу: память потока не копится между задачами.
        lua_gc(L, LUA_GCSTEP, 0);
        BusyMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(Mutex);
        st->Busy = false;
        Done[job.Id] = std::move(fin);
    }

    void StopAll() {
        // Не начатые — отменить, идущие — дождаться: только потом можно
        // разрушать состояния Lua, на которых они выполняются.
        if (Limiter) {
            Limiter->CancelAll();
            Limiter->Wait();
            Limiter.reset();
        }
        std::lock_guard<std::mutex> lock(Mutex);
        // Отменённые задачи не придут никогда — ждущий скрипт получит ошибку,
        // а не вечное «в пути».
        for (const auto& kv : Outstanding)
            if (!Done.count(kv.first)) Done[kv.first] = Finished{false, {}, "tasks: остановлено"};
        States.clear();
    }
};

ScriptTasks::ScriptTasks() : m_impl(std::make_unique<Impl>()) {}
ScriptTasks::~ScriptTasks() { Shutdown(); }

void ScriptTasks::Preload(const std::string& name, const std::string& chunkName, const std::string& source) {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    m_impl->Modules[name] = {chunkName, source};
    ++m_impl->ModulesVersion;
}

bool ScriptTasks::HasModule(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    return m_impl->Modules.count(name) > 0;
}

int64_t ScriptTasks::Submit(const std::string& module, const std::string& fn, std::vector<TaskValue> args) {
    int64_t id;
    {
        std::lock_guard<std::mutex> lock(m_impl->Mutex);
        m_impl->EnsureStarted();
        id = m_impl->NextId++;
        m_impl->Outstanding[id] = true;
        ++m_impl->Pending;
    }
    auto job = std::make_shared<Job>(Job{id, module, fn, std::move(args)});
    Impl* impl = m_impl.get();  // живёт дольше задач: StopAll дожидается их
    sage::jobs::Future<void> f = m_impl->Limiter->Async([impl, job] { impl->RunJob(*job); });
    (void)f;
    return id;
}

TaskState ScriptTasks::Take(int64_t id, std::vector<TaskValue>& results, std::string& error) {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    auto it = m_impl->Done.find(id);
    if (it == m_impl->Done.end())
        return m_impl->Outstanding.count(id) ? TaskState::Pending : TaskState::Unknown;
    const bool ok = it->second.Ok;
    results = std::move(it->second.Results);
    error = std::move(it->second.Error);
    m_impl->Done.erase(it);
    m_impl->Outstanding.erase(id);
    --m_impl->Pending;
    return ok ? TaskState::Done : TaskState::Failed;
}

int ScriptTasks::Workers() const {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    if (m_impl->Limiter) return m_impl->Limiter->Limit();
    if (m_impl->WantWorkers > 0) return m_impl->WantWorkers;
    return Impl::AutoWorkers();
}

int ScriptTasks::Pending() const {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    return m_impl->Pending;
}

double ScriptTasks::BusyMs() const { return (double)m_impl->BusyMicros.load() / 1000.0; }

void ScriptTasks::SetWorkerCount(int n) {
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    m_impl->WantWorkers = std::max(0, n);
    if (m_impl->Limiter) m_impl->Limiter->SetLimit(n > 0 ? n : Impl::AutoWorkers());
}

void ScriptTasks::Shutdown() {
    if (m_impl) m_impl->StopAll();
}

} // namespace sage::scripting
