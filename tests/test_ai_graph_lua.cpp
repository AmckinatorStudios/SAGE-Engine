// SAGE AI Graph вместе с Lua и сценой: свои узлы и типы из Lua, граф зовёт
// Lua-задачу и Lua зовёт граф, узлы API движка, файл графа, компонент AI на
// объекте сцены, события sage.events в графе.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "sage/ai/GraphRuntime.h"
#include "sage/ai/World.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

using namespace sage::ai;
using sage::vars::Value;

namespace {

bool Lua(ScriptEngine& se, const std::string& code) {
    auto r = se.Lua().safe_script(code, sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        std::printf("    lua: %s\n", e.what());
        return false;
    }
    return true;
}

template <class T>
T Get(ScriptEngine& se, const std::string& expr) {
    return se.Lua().safe_script("return " + expr).get<T>();
}

int Add(GraphDoc& d, const std::string& type, std::map<std::string, Value> values = {}) {
    GraphNode& n = d.AddNode(type, 0, 0);
    n.Values = std::move(values);
    return n.Id;
}
void Exec(GraphDoc& d, int from, const std::string& pin, int to) { d.Link({from, pin}, {to, "in"}, true); }
void Data(GraphDoc& d, int from, const std::string& pin, int to, const std::string& in) {
    d.Link({from, pin}, {to, in}, false);
}
Value Str(const char* s) { return Value(std::string(s)); }

} // namespace

TEST(AIGraphLua_custom_nodes_types_and_tasks_both_ways) {
    ScriptEngine se;
    CHECK_TRUE(Lua(se, R"(
        ai.type("Target", {fields = {{"pos", Vec3(0, 0, 0)}, {"seen", false}}})
        ai.node("my.double", {
            title = "Double", category = "My game", pure = true,
            inputs = {{"x", "float", 1}}, outputs = {{"y", "float"}},
            run = function(n) n.outputs.y = n.inputs.x * 2 end,
            connect = function(pin, other) if other == "string" then return false, "только числа" end end,
        })
        ai.node("my.pick", {
            inputs = {{"good", "bool", true}}, outputs = {{"yes", "exec"}, {"no", "exec"}},
            run = function(n) if n.inputs.good then return "yes" end return "no" end,
        })
        ai.task("LuaAdd", {inputs = {a = 0, b = 0}, outputs = {sum = 0},
            run = function(t) t.wait(0.1); t.outputs.sum = t.inputs.a + t.inputs.b; return true end})
        ai.task("AskGraph", {run = function(t)
            local ok, h = t.run("GraphCalc", {x = 5})
            graph_out = h.outputs.result
            return ok
        end})
    )"));
    World& w = se.AI();
    const NodeType* dbl = w.Nodes().Find("my.double");
    CHECK_TRUE(dbl && dbl->Category == "My game");
    std::string why;
    GraphNode probe;
    CHECK_TRUE(dbl && dbl->CanConnect && !dbl->CanConnect(probe, "x", "string", why));
    CHECK_TRUE(why == "только числа");
    CHECK_TRUE(w.Types().IsStruct("Target"));

    // GraphCalc: result = LuaAdd(double(x), 1) — граф зовёт Lua-узел и Lua-задачу.
    GraphDoc d;
    d.Name = "GraphCalc";
    d.Inputs = {{"x", "float", Value(0.0f), ""}};
    d.Outputs = {{"result", "float", Value(0.0f), ""}};
    const int start = Add(d, "flow.start");
    const int x = Add(d, "var.get", {{"scope", Str("input")}, {"name", Str("x")}});
    const int dbl2 = Add(d, "my.double");
    const int pick = Add(d, "my.pick");
    const int run = Add(d, "task.run", {{"task", Str("LuaAdd")}, {"b", Value(1)}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", pick);
    Exec(d, pick, "yes", run);
    Data(d, x, "value", dbl2, "x");
    Data(d, dbl2, "y", run, "a");
    Exec(d, run, "succeeded", ret);
    Data(d, run, "sum", ret, "result");
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    CHECK_TRUE(Lua(se, "h = ai.agent().start('AskGraph')"));
    for (int i = 0; i < 5; ++i) se.AI().Tick(0.1f);
    CHECK_TRUE(Get<bool>(se, "h.ok"));
    CHECK_NEAR(Get<float>(se, "graph_out"), 11.0f, 1e-5f);
}

TEST(AIGraphLua_api_nodes_file_component_and_events) {
    Scene scene{"ai"};
    ScriptEngine se;
    se.BindScene(scene);
    GameObject guard = scene.CreateObject("Guard");
    GameObject post = scene.CreateObject("Post");
    post.GetTransform().Position = glm::vec3(4, 0, 2);
    // Узел на каждую функцию API: sage.scene.Find — тоже узел.
    CHECK_TRUE(se.AI().Nodes().Find("api.scene.Find") != nullptr);
    CHECK_TRUE(se.AI().Nodes().Find("world.in_view") != nullptr);

    // Граф поведения: найти пост, встать туда, дождаться события «alarm»
    // (пришедшего через sage.events), записать в память.
    GraphDoc d;
    d.Kind = GraphKind::Behavior;
    d.Name = "GuardBrain";
    const int start = Add(d, "flow.start");
    const int find = Add(d, "api.scene.Find", {{"name", Str("Post")}});
    const int info = Add(d, "object.info");
    const int self = Add(d, "world.self");
    const int move = Add(d, "object.set_position");
    const int wait = Add(d, "event.wait", {{"event", Str("alarm")}});
    const int mem = Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str("alarm_level")}});
    Exec(d, start, "then", find);
    Exec(d, find, "then", move);
    Data(d, find, "result", info, "object");
    Data(d, self, "object", move, "object");
    Data(d, info, "position", move, "position");
    Exec(d, move, "then", wait);
    Exec(d, wait, "received", mem);
    Data(d, wait, "data", mem, "value");
    const std::string path = "sage_ai_guard.aigraph";
    {
        std::ofstream f(path, std::ios::binary);
        f << SaveGraph(d);
    }
    AiAgentComponent c;
    c.Graph = path;
    c.Shared = "guards";
    c.Memory.Set("post_name", Value(std::string("Post")));
    scene.Registry().emplace<AiAgentComponent>(guard.Entity(), c);

    se.UpdateAll(0.016f);
    se.UpdateAll(0.016f);
    CHECK_NEAR(guard.GetTransform().Position.x, 4.0f, 1e-5f);
    se.Lua()["guard_obj"] = guard;
    CHECK_TRUE(Lua(se, "brain = ai.agent_of(guard_obj)"));
    CHECK_TRUE(Get<std::string>(se, "brain.memory.post_name") == "Post");
    CHECK_TRUE(Lua(se, "sage.events.Emit('alarm', 3)"));
    se.UpdateAll(0.016f);
    CHECK_EQ(Get<int>(se, "brain.memory.alarm_level"), 3);
    CHECK_EQ(Get<int>(se, "brain.running()"), 0);   // граф дошёл до конца

    // Объект удалили — ИИ уходит с ним.
    scene.RemoveObject(guard.Id());
    se.UpdateAll(0.016f);
    CHECK_EQ((int)se.AI().AgentCount(), 0);
    std::remove(path.c_str());
}
