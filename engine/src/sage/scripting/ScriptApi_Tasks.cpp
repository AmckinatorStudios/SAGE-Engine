#include "sage/scripting/ScriptEngine.h"

#include <algorithm>
#include <regex>
#include <set>

#include "sage/assets/Pack.h"
#include "sage/core/Profiler.h"
#include "sage/scripting/ScriptTasks.h"

// ---------------------------------------------------------------------------
// tasks.* — фоновые задачи скриптов (устройство — ScriptTasks.h) и
// profile.* — профилировщик кадра для игры (sage/core/Profiler.h).
//
//   tasks.preload("gen")                      -- модуль и всё, что он require
//   local id = tasks.run("gen", "build", seed, cx, cz)
//   local ok, a, b = tasks.result(id)         -- nil — ещё считается;
//                                             -- false, ошибка — упала
//   tasks.workers(), tasks.pending()
//
//   profile.set_enabled(true)                 -- то же, что SAGE_PROFILE=1
//   profile.push("свиньи") ... profile.pop()  -- свой участок кадра
//   for _, s in ipairs(profile.sections()) do print(s.name, s.cpu, s.gpu) end
// ---------------------------------------------------------------------------

namespace {

// Имена модулей из require "x" / require("x") / require 'x' в тексте модуля:
// их тоже надо отдать воркерам, иначе задача упадёт на первом require.
std::vector<std::string> RequiredModules(const std::string& source) {
    static const std::regex re(R"(require\s*\(?\s*["']([A-Za-z0-9_./]+)["'])");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1].str());
    return out;
}

} // namespace

sage::scripting::ScriptTasks& ScriptEngine::Tasks() {
    if (!m_tasks) m_tasks = std::make_unique<sage::scripting::ScriptTasks>();
    return *m_tasks;
}

bool ScriptEngine::PreloadTaskModule(const std::string& name, std::string& error) {
    std::set<std::string> seen;
    std::vector<std::string> todo{name};
    while (!todo.empty()) {
        const std::string mod = todo.back();
        todo.pop_back();
        if (!seen.insert(mod).second) continue;
        std::string rel = mod;
        std::replace(rel.begin(), rel.end(), '.', '/');
        bool found = false;
        for (const std::string& dir : m_scriptSearchPaths) {
            for (const std::string& path : {dir + rel + ".lua", dir + rel + "/init.lua"}) {
                std::string source;
                if (!sage::assets::vfs::ReadText(path, source)) continue;
                Tasks().Preload(mod, path, source);
                for (const std::string& dep : RequiredModules(source)) todo.push_back(dep);
                found = true;
                break;
            }
            if (found) break;
        }
        if (!found) {
            // Зависимость может быть модулем движка (их у воркера нет) — это
            // ошибка, только если не нашёлся сам запрошенный модуль.
            if (mod == name) {
                error = "tasks.preload: модуль '" + name + "' не найден в путях скриптов";
                return false;
            }
        }
    }
    return true;
}

void ScriptEngine::RegisterTasksApi() {
    using sage::scripting::TaskState;
    using sage::scripting::TaskValue;

    Bind("tasks", "preload", nullptr, [this](const std::string& name) {
        std::string error;
        if (!PreloadTaskModule(name, error)) throw std::runtime_error(error);
    });
    Bind("tasks", "run", nullptr, [this](sol::this_state ts, const std::string& module, const std::string& fn,
                                         sol::variadic_args va) -> int64_t {
        lua_State* L = ts;
        if (!Tasks().HasModule(module)) {
            std::string error;
            if (!PreloadTaskModule(module, error)) throw std::runtime_error(error);
        }
        std::vector<TaskValue> args((size_t)va.size());
        for (int i = 0; i < (int)va.size(); ++i) {
            std::string error;
            if (!sage::scripting::ToTaskValue(L, va.stack_index() + i, args[(size_t)i], error))
                throw std::runtime_error("tasks.run: аргумент " + std::to_string(i + 1) + ": " + error);
        }
        return Tasks().Submit(module, fn, std::move(args));
    });
    // Результат задачи: nil — ещё считается; true, результаты… — готово;
    // false, текст — упала. Забранный результат второй раз не отдаётся.
    Bind("tasks", "result", nullptr, [this](sol::this_state ts, int64_t id) -> sol::variadic_results {
        lua_State* L = ts;
        sol::variadic_results out;
        std::vector<TaskValue> results;
        std::string error;
        switch (Tasks().Take(id, results, error)) {
            case TaskState::Pending:
                out.push_back(sol::make_object(L, sol::lua_nil));
                return out;
            case TaskState::Unknown:
                out.push_back(sol::make_object(L, false));
                out.push_back(sol::make_object(L, "tasks.result: нет такой задачи (или результат уже забран)"));
                return out;
            case TaskState::Failed:
                out.push_back(sol::make_object(L, false));
                out.push_back(sol::make_object(L, error));
                return out;
            case TaskState::Done: break;
        }
        out.push_back(sol::make_object(L, true));
        for (const TaskValue& v : results) {
            sage::scripting::PushTaskValue(L, v);
            out.push_back(sol::stack::pop<sol::object>(L));
        }
        return out;
    });
    Bind("tasks", "workers", nullptr, [this]() { return Tasks().Workers(); });
    Bind("tasks", "pending", nullptr, [this]() { return Tasks().Pending(); });
    Bind("tasks", "busy_ms", nullptr, [this]() { return Tasks().BusyMs(); });
    Bind("tasks", "set_workers", nullptr, [this](int n) { Tasks().SetWorkerCount(n); });
}

void ScriptEngine::RegisterProfileApi() {
    namespace prof = sage::profile;
    Bind("profile", "set_enabled", nullptr, [](bool on) { prof::SetEnabled(on); });
    Bind("profile", "enabled", nullptr, []() { return prof::Enabled(); });
    // Имена участков скриптов держит сам профилировщик (копия строки).
    Bind("profile", "push", nullptr, [](const std::string& name) { prof::Push(name.c_str()); });
    Bind("profile", "pop", nullptr, []() { prof::Pop(); });
    Bind("profile", "frame_cpu_ms", nullptr, []() { return prof::FrameCpuMs(); });
    Bind("profile", "frame_gpu_ms", nullptr, []() { return prof::FrameGpuMs(); });
    Bind("profile", "memory_mb", nullptr, []() { return (double)prof::ResidentBytes() / (1024.0 * 1024.0); });
    Bind("profile", "summary", nullptr, []() { return prof::Summary(); });
    // Участки кадра (сглаженные): {name, depth, cpu, gpu, calls} по порядку.
    Bind("profile", "sections", nullptr, [](sol::this_state ts) {
        sol::state_view L(ts);
        sol::table out = L.create_table();
        int i = 1;
        for (const prof::Entry& e : prof::Average()) {
            sol::table t = L.create_table_with("name", e.Name, "depth", e.Depth, "cpu", e.CpuMs, "gpu", e.GpuMs,
                                               "calls", e.Calls);
            out[i++] = t;
        }
        return out;
    });
}
