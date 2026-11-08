#include "TestFramework.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "sage/scripting/ScriptEngine.h"

// Фоновые задачи скриптов (tasks.*, ScriptTasks): работа уходит в потоки
// воркеров, результат — копией; модуль задачи и его require передаются
// воркерам; шум у воркера тот же, что у главного состояния.

namespace {

void WriteScript(const std::string& name, const std::string& text) {
    std::ofstream(name + ".lua") << text;
}

// Опрос tasks.result, пока задача не готова (не дольше 10 с).
sol::object WaitTask(ScriptEngine& se, const std::string& expr) {
    for (int i = 0; i < 1000; ++i) {
        sol::object r = se.Lua().script(expr);
        if (r.get_type() != sol::type::lua_nil) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return sol::make_object(se.Lua(), sol::lua_nil);
}

} // namespace

TEST(Tasks_run_in_other_thread_and_return_copies) {
    WriteScript("sage_task_dep", "local D = {}\nfunction D.square(x) return x * x end\nreturn D\n");
    WriteScript("sage_task_mod", R"LUA(
local D = require "sage_task_dep"
local M = {}
function M.sum(t, k)
    local s = 0
    for i = 1, #t do s = s + D.square(t[i]) end
    return s * k, {name = "ok", list = {1, 2.5, 3}, flag = true}
end
function M.fail() error("boom") end
function M.noise(seed)
    local n = noise{kind = "simplex", seed = seed, scale = 0.05}
    return n(10, 0, 20), n.grid(0, 0, 0, 4, 1, 4), hash(3, 4, 5, seed)
end
return M
)LUA");
    ScriptEngine se;
    se.AddScriptSearchPath(".");
    CHECK_TRUE(se.Lua().script("return tasks.workers()").get<int>() >= 1);
    se.Lua().script("TID = tasks.run('sage_task_mod', 'sum', {1, 2, 3}, 2)");
    sol::object r = WaitTask(se, "R = table.pack(tasks.result(TID)) return R[1]");
    CHECK_TRUE(r.get_type() == sol::type::boolean && r.as<bool>());
    CHECK_EQ(se.Lua().script("return R[2]").get<int>(), 28);
    CHECK_EQ(se.Lua().script("return R[3].name").get<std::string>(), std::string("ok"));
    CHECK_NEAR(se.Lua().script("return R[3].list[2]").get<double>(), 2.5, 1e-9);
    CHECK_TRUE(se.Lua().script("return R[3].flag").get<bool>());
    // Забранный результат второй раз не отдаётся.
    CHECK_TRUE(!se.Lua().script("return (tasks.result(TID))").get<bool>());

    // Ошибка задачи — false и текст, игра не падает.
    se.Lua().script("FID = tasks.run('sage_task_mod', 'fail')");
    sol::object f = WaitTask(se, "F = table.pack(tasks.result(FID)) return F[1]");
    CHECK_TRUE(f.get_type() == sol::type::boolean && !f.as<bool>());
    CHECK_TRUE(se.Lua().script("return F[2]").get<std::string>().find("boom") != std::string::npos);

    // Шум у воркера — тот же, что у главного состояния.
    se.Lua().script("NID = tasks.run('sage_task_mod', 'noise', 7)");
    sol::object n = WaitTask(se, "NR = table.pack(tasks.result(NID)) return NR[1]");
    CHECK_TRUE(n.get_type() == sol::type::boolean && n.as<bool>());
    const double main = se.Lua().script("return noise{kind = 'simplex', seed = 7, scale = 0.05}(10, 0, 20)");
    CHECK_NEAR(se.Lua().script("return NR[2]").get<double>(), main, 1e-6);
    const double g = se.Lua().script("return noise{kind = 'simplex', seed = 7, scale = 0.05}.grid(0, 0, 0, 4, 1, 4)[6]");
    CHECK_NEAR(se.Lua().script("return NR[3][6]").get<double>(), g, 1e-6);
    CHECK_NEAR(se.Lua().script("return NR[4]").get<double>(), se.Lua().script("return hash(3, 4, 5, 7)").get<double>(),
               1e-9);
    std::remove("sage_task_dep.lua");
    std::remove("sage_task_mod.lua");
}

// Много задач — параллельно: суммарное время работы воркеров больше
// времени ожидания (при нескольких ядрах), все результаты на месте.
TEST(Tasks_many_jobs_all_return) {
    WriteScript("sage_task_heavy", R"LUA(
local M = {}
function M.work(i)
    local a = {}
    for k = 1, 20000 do a[k] = (k * i) % 97 end
    local s = 0
    for k = 1, #a do s = s + a[k] end
    return i, s, a
end
return M
)LUA");
    ScriptEngine se;
    se.AddScriptSearchPath(".");
    se.Lua().script("IDS = {} for i = 1, 24 do IDS[i] = tasks.run('sage_task_heavy', 'work', i) end");
    int done = 0;
    for (int tries = 0; tries < 1000 && done < 24; ++tries) {
        done = se.Lua().script(R"LUA(
            OUT = OUT or {}
            for i = 1, 24 do
                if not OUT[i] then
                    local ok, idx, s, a = tasks.result(IDS[i])
                    if ok then OUT[i] = {idx, s, #a, a[20000]} end
                end
            end
            local n = 0 for _ in pairs(OUT) do n = n + 1 end return n
        )LUA");
        if (done < 24) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(done, 24);
    CHECK_EQ(se.Lua().script("return OUT[5][1]").get<int>(), 5);
    CHECK_EQ(se.Lua().script("return OUT[5][3]").get<int>(), 20000);
    CHECK_EQ(se.Lua().script("return OUT[5][4]").get<int>(), (20000 * 5) % 97);
    CHECK_EQ(se.Lua().script("return tasks.pending()").get<int>(), 0);
    // Функции и объекты в другой поток не уходят — понятная ошибка.
    auto bad = se.Lua().safe_script("tasks.run('sage_task_heavy', 'work', function() end)", sol::script_pass_on_error);
    CHECK_TRUE(!bad.valid());
    std::remove("sage_task_heavy.lua");
}

// Профилировщик из скрипта: включение, свой участок, данные кадра.
TEST(Profile_api_from_script) {
    ScriptEngine se;
    se.Lua().script("profile.set_enabled(false)");
    CHECK_TRUE(!se.Lua().script("return profile.enabled()").get<bool>());
    // Без включения push/pop безопасны и ничего не пишут.
    se.Lua().script("profile.push('x') profile.pop()");
    CHECK_TRUE(se.Lua().script("return type(profile.sections())").get<std::string>() == "table");
    CHECK_TRUE(se.Lua().script("return profile.memory_mb() >= 0").get<bool>());
    CHECK_TRUE(!se.Lua().script("return profile.summary()").get<std::string>().empty());
}
