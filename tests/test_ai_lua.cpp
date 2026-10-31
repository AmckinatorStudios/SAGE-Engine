// SAGE AI в Lua: ai.task / ai.agent / контекст t (ScriptApi_AI.cpp). Правила
// выполнения — те же, что у ядра (test_ai.cpp); здесь проверяется, что Lua
// их не обходит и что задачи на Lua и на C++ зовут друг друга по имени.
#include "TestFramework.h"

#include <string>

#include "sage/ai/World.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

using namespace sage::ai;
using sage::vars::Value;

namespace {

bool Lua(ScriptEngine& se, const char* code) {
    auto r = se.Lua().safe_script(code, sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        std::printf("    lua: %s\n", e.what());
        return false;
    }
    return true;
}

template <class T>
T Get(ScriptEngine& se, const char* expr) {
    return se.Lua().safe_script(std::string("return ") + expr).get<T>();
}

void Frames(ScriptEngine& se, int n, float dt = 0.1f) {
    for (int i = 0; i < n; ++i) se.AI().Tick(dt);
}

} // namespace

TEST(AILua_long_task_keeps_state_between_frames) {
    ScriptEngine se;
    CHECK_TRUE(Lua(se, R"(
        starts, steps = 0, 0
        ai.task("Patrol", {
            inputs = {points = 3},
            outputs = {visited = 0},
            run = function(t)
                starts = starts + 1
                for i = 1, t.inputs.points do
                    t.wait(0.5)
                    steps = steps + 1
                    t.outputs.visited = i
                end
                return true
            end,
        })
        brain = ai.agent()
        h = brain.start("Patrol", {points = 2})
    )"));
    Frames(se, 6);   // 0.6 с — одна точка пройдена
    CHECK_EQ(Get<int>(se, "starts"), 1);
    CHECK_EQ(Get<int>(se, "steps"), 1);
    CHECK_TRUE(Get<std::string>(se, "h.status") == "running");
    Frames(se, 6);
    CHECK_EQ(Get<int>(se, "starts"), 1);   // не начиналась заново
    CHECK_TRUE(Get<bool>(se, "h.ok"));
    CHECK_EQ(Get<int>(se, "h.outputs.visited"), 2);
}

TEST(AILua_nested_parallel_cancel_and_failure) {
    ScriptEngine se;
    CHECK_TRUE(Lua(se, R"(
        log = {}
        ai.task("Sleep", {
            inputs = {s = 1},
            run = function(t) t.wait(t.inputs.s); return true end,
            cancel = function(t) log[#log + 1] = "cancel " .. t.inputs.s end,
        })
        ai.task("Reach", {
            run = function(t) t.wait(0.1); return false, "нет пути" end,
        })
        ai.task("Root", {
            run = function(t)
                local ok, r = t.run("Reach")
                reach_ok, reach_err = ok, r.error
                local a = t.start("Sleep", {s = 0.2})
                local b = t.start("Sleep", {s = 9})
                local first = t.wait_any(a, b)
                first_is_a = first == a
                return true     -- b ещё идёт: мир отменит его вместе с концом Root
            end,
        })
        root = ai.agent().start("Root")
    )"));
    Frames(se, 6);
    CHECK_TRUE(Get<bool>(se, "root.ok"));
    CHECK_FALSE(Get<bool>(se, "reach_ok"));
    CHECK_TRUE(Get<std::string>(se, "reach_err") == "нет пути");
    CHECK_TRUE(Get<bool>(se, "first_is_a"));
    CHECK_TRUE(Get<std::string>(se, "log[1]") == "cancel 9");
    CHECK_EQ((int)se.AI().ActiveCount(), 0);
}

TEST(AILua_scopes_events_and_errors) {
    ScriptEngine se;
    CHECK_TRUE(Lua(se, R"(
        ai.task("Guard", {
            run = function(t)
                t.locals.n = 1
                t.instance.alert = (t.instance.alert or 0) + 1
                local heard, where = t.wait_event("noise")
                t.shared.last = where
                local again = t.wait_event("noise", 0.3)
                timed_out = not again
                t.fail("устал")
            end,
        })
        g1 = ai.agent(nil, {shared = "guards"})
        g2 = ai.agent(nil, {shared = "guards"})
        r1 = g1.start("Guard")
        r2 = g2.start("Guard")
    )"));
    Frames(se, 1);
    CHECK_EQ(Get<int>(se, "g1.memory.alert"), 1);
    CHECK_EQ(Get<int>(se, "r1.locals.n"), 1);
    CHECK_EQ(Get<int>(se, "ai.emit('noise', Vec3(1, 2, 3), g1)"), 1);
    Frames(se, 1);
    CHECK_NEAR(Get<float>(se, "ai.shared('guards').last.y"), 2.0f, 1e-5f);
    CHECK_TRUE(Get<bool>(se, "ai.shared('world').last == nil"));
    Frames(se, 4);
    CHECK_TRUE(Get<bool>(se, "timed_out"));
    CHECK_TRUE(Get<std::string>(se, "r1.status") == "failed");
    CHECK_TRUE(Get<std::string>(se, "r1.error") == "устал");
    CHECK_TRUE(Get<std::string>(se, "r2.status") == "running");   // g2 не слышал

    // Ошибки называют, что не так.
    CHECK_FALSE(Lua(se, "g1.start('Guardd')"));
    CHECK_FALSE(Lua(se, "ai.task('X', {run = function() end, outptus = {}})"));
    CHECK_FALSE(Lua(se, "ai.task('Y', {inputs = {a = 1}, run = function() end}); g1.start('Y', {b = 2})"));
    CHECK_FALSE(Lua(se, "g1.memory.x = function() end"));
    // Таблица — список или структура и возвращается тем же.
    CHECK_TRUE(Lua(se, "g1.memory.path = {Vec3(1, 0, 0), Vec3(2, 0, 0)}; g1.memory.t = {_type = 'Target', seen = true}"));
    CHECK_NEAR(Get<float>(se, "g1.memory.path[2].x"), 2.0f, 1e-5f);
    CHECK_TRUE(Get<bool>(se, "g1.memory.t.seen and g1.memory.t._type == 'Target'"));
    CHECK_FALSE(Lua(se, "local _ = r1.stauts"));
}

TEST(AILua_lua_and_cpp_tasks_call_each_other) {
    ScriptEngine se;
    World& w = se.AI();
    // Задача на C++ зовёт задачу на Lua и забирает её выход.
    TaskSignature sig;
    sig.Outputs = {{"got", Value(0)}};
    RunPtr child;
    int got = 0;
    w.RegisterTask(MakeTask("NativeAsk", sig, [&](TaskContext& t) {
        if (t.Phase()++ == 0) {
            Blackboard in;
            in.Set("x", Value(20));
            child = t.Start("LuaDouble", in);
            t.WaitRuns({child});
            return TaskStatus::Running;
        }
        got = child->Outputs->Get("y").AsInt();
        t.Outputs().Set("got", Value(got));
        return child->Status == TaskStatus::Succeeded ? TaskStatus::Succeeded : TaskStatus::Failed;
    }));
    CHECK_TRUE(Lua(se, R"(
        ai.task("LuaDouble", {
            inputs = {x = 0}, outputs = {y = 0},
            run = function(t) t.outputs.y = t.inputs.x * 2; return true end,
        })
        ai.task("LuaAsk", {
            run = function(t)
                local ok, h = t.run("NativeAsk")
                native_ok = ok
                return ok
            end,
        })
        ai.operation("twice", function(v) return v * 2 end, {inputs = {{"v", 0}}, outputs = {{"r", 0}}})
        op = ai.call("twice", 21)
        h = ai.agent().start("LuaAsk")
    )"));
    Frames(se, 3);
    CHECK_TRUE(Get<bool>(se, "native_ok"));
    CHECK_EQ(got, 40);
    CHECK_TRUE(Get<bool>(se, "h.ok"));
    CHECK_EQ(Get<int>(se, "op"), 42);

    // Операция на Lua — из C++ тем же путём.
    Values out;
    std::string err;
    CHECK_TRUE(w.Call("twice", {Value(5)}, out, err));
    CHECK_EQ(out.empty() ? 0 : out[0].AsInt(), 10);
}

TEST(AILua_agent_dies_with_owner_and_ticks_with_scripts) {
    Scene scene{"ai"};
    ScriptEngine se;
    se.BindScene(scene);
    GameObject guard = scene.CreateObject("Guard");
    se.Lua()["guard"] = guard;
    CHECK_TRUE(Lua(se, R"(
        cleaned = false
        ai.task("Watch", {
            outputs = {who = guard},
            run = function(t) t.outputs.who = t.self; t.wait_event("never") end,
            cancel = function(t) cleaned = true end,
        })
        brain = ai.agent(guard)
        h = brain.start("Watch")
    )"));
    se.UpdateAll(0.016f);   // задачи ИИ тикают вместе со скриптами кадра
    CHECK_TRUE(Get<std::string>(se, "h.status") == "running");
    CHECK_TRUE(Get<bool>(se, "h.outputs.who.Id == guard.Id"));
    scene.RemoveObject(guard.Id());
    se.UpdateAll(0.016f);
    CHECK_TRUE(Get<std::string>(se, "h.status") == "cancelled");
    CHECK_TRUE(Get<bool>(se, "cleaned"));
    CHECK_EQ((int)se.AI().AgentCount(), 0);
}
