// SAGE AI Graph, ядро без языка: документ и его файл, типы и совместимость,
// выполнение графа как задачи (поток, ожидания, Parallel, циклы, события,
// задачи из графа), отладчик. Граф и Lua вместе — в test_ai_graph_lua.cpp.
#include "TestFramework.h"

#include <string>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/GraphRuntime.h"
#include "sage/ai/World.h"

using namespace sage::ai;
using sage::vars::Value;

namespace {

// Узел с настройками и значениями входов — одной строкой в тесте.
// Номер, а не ссылка: следующий AddNode растит вектор узлов, и ссылка на
// прежний узел протухла бы.
int Add(GraphDoc& d, const std::string& type, std::map<std::string, Value> values = {}) {
    GraphNode& n = d.AddNode(type, 0, 0);
    n.Values = std::move(values);
    return n.Id;
}

void Exec(GraphDoc& d, int from, const std::string& pin, int to) { d.Link({from, pin}, {to, "in"}, true); }
void Data(GraphDoc& d, int from, const std::string& pin, int to, const std::string& in) {
    d.Link({from, pin}, {to, in}, false);
}

RunPtr StartGraph(World& w, const GraphDoc& d, Blackboard in = {}) {
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    return w.Start(w.CreateAgent(), d.Name, in);
}

} // namespace

TEST(AIGraph_file_roundtrip_keeps_everything) {
    GraphDoc d;
    d.Name = "Patrol";
    d.Kind = GraphKind::Behavior;
    d.Inputs = {{"speed", "float", Value(2.5f), "м/с"}};
    d.Locals = {{"path", "array", Value::Array({Value(glm::vec3(1, 2, 3))}), ""}};
    const int s = Add(d, "flow.start");
    const int w = Add(d, "flow.wait", {{"seconds", Value(0.5f)}});
    d.FindNode(w)->Comment = "передышка";
    Exec(d, s, "then", w);
    d.AddGroup("Патруль", 10, 20, 300, 200);
    GraphDoc back;
    std::string err;
    CHECK_TRUE(LoadGraph(SaveGraph(d), back, err));
    CHECK_TRUE(back.Name == "Patrol" && back.Kind == GraphKind::Behavior);
    CHECK_EQ((int)back.Nodes.size(), 2);
    CHECK_EQ((int)back.Links.size(), 1);
    CHECK_EQ((int)back.Groups.size(), 1);
    CHECK_TRUE(back.FindNode(w) && back.FindNode(w)->Comment == "передышка");
    CHECK_NEAR(back.FindNode(w)->Values["seconds"].AsFloat(), 0.5f, 1e-6f);
    CHECK_TRUE(back.Locals.size() == 1 && back.Locals[0].Default.Items().size() == 1);
    CHECK_NEAR(back.Locals[0].Default.Items()[0].AsVec3().z, 3.0f, 1e-6f);
    CHECK_TRUE(back.NextId > w);
    CHECK_FALSE(LoadGraph("{не json", back, err));
}

TEST(AIGraph_types_compatibility_and_custom_rules) {
    TypeRegistry t;
    CHECK_TRUE(t.CanConnect("int", "float"));
    CHECK_TRUE(t.CanConnect("vec3", "string"));
    CHECK_TRUE(t.CanConnect("vec3", "rotation"));
    std::string why;
    CHECK_FALSE(t.CanConnect("exec", "float", &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(t.CanConnect("string", "vec3"));

    TypeInfo target;
    target.Id = "Target";
    target.Fields = {{"pos", Value(glm::vec3(0.0f))}, {"seen", Value(false)}};
    t.Register(target);
    CHECK_TRUE(t.IsStruct("Target"));
    CHECK_FALSE(t.CanConnect("Target", "vec3"));
    t.AddConnectRule([](const std::string& from, const std::string& to) {
        return from == "Target" && to == "vec3" ? 1 : -1;
    });
    t.AddConvertRule([](const Value& in, const std::string& to, Value& out) {
        if (to != "vec3" || in.StructType() != "Target") return false;
        out = *in.FindField("pos");
        return true;
    });
    CHECK_TRUE(t.CanConnect("Target", "vec3"));
    Value v = t.Convert(Value::Struct("Target", {{"pos", Value(glm::vec3(4, 5, 6))}}), "vec3");
    CHECK_NEAR(v.AsVec3().y, 5.0f, 1e-6f);
    // Неполная структура достраивается полями по умолчанию.
    Value s = t.Convert(Value::Struct("Target"), "Target");
    CHECK_TRUE(s.FindField("seen") != nullptr);
    CHECK_TRUE(t.Of(s) == "Target");
}

TEST(AIGraph_sequence_branch_vars_and_return) {
    World w;
    GraphDoc d;
    d.Name = "Count";
    d.Inputs = {{"n", "int", Value(3), ""}};
    d.Outputs = {{"result", "int", Value(0), ""}};
    d.Locals = {{"sum", "int", Value(0), ""}};
    const int start = Add(d, "flow.start");
    const int loop = Add(d, "flow.for", {{"first", Value(1)}});
    const int n = Add(d, "var.get", {{"scope", Value(std::string("input"))}, {"name", Value(std::string("n"))}});
    const int sum = Add(d, "var.get", {{"scope", Value(std::string("local"))}, {"name", Value(std::string("sum"))}});
    const int add = Add(d, "math.add");
    const int set = Add(d, "var.set", {{"scope", Value(std::string("local"))}, {"name", Value(std::string("sum"))}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", loop);
    Data(d, n, "value", loop, "last");
    Exec(d, loop, "body", set);
    Data(d, sum, "value", add, "a");
    Data(d, loop, "index", add, "b");
    Data(d, add, "result", set, "value");
    Exec(d, loop, "completed", ret);
    Data(d, sum, "value", ret, "result");
    Blackboard in;
    in.Set("n", Value(4));
    RunPtr r = StartGraph(w, d, in);
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_EQ(r->Outputs->Get("result").AsInt(), 1 + 2 + 3 + 4);
}

TEST(AIGraph_long_task_waits_without_restarting) {
    World w;
    int starts = 0;
    TaskSignature sig;
    sig.Inputs = {{"to", Value(glm::vec3(0.0f))}};
    sig.Outputs = {{"arrived", Value(false)}};
    w.RegisterTask(MakeTask("MoveTo", sig, [&](TaskContext& t) {
        if (t.Phase()++ == 0) {
            ++starts;
            t.WaitSeconds(0.5f);
            return TaskStatus::Running;
        }
        t.Outputs().Set("arrived", Value(true));
        return TaskStatus::Succeeded;
    }));
    GraphDoc d;
    d.Name = "Go";
    d.Outputs = {{"done", "bool", Value(false), ""}};
    const int start = Add(d, "flow.start");
    const int run = Add(d, "task.run", {{"task", Value(std::string("MoveTo"))}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", run);
    Exec(d, run, "succeeded", ret);
    Data(d, run, "arrived", ret, "done");
    RunPtr r = StartGraph(w, d);
    for (int i = 0; i < 4; ++i) w.Tick(0.1f);
    CHECK_EQ(starts, 1);
    CHECK_TRUE(r->Status == TaskStatus::Running);
    for (int i = 0; i < 3; ++i) w.Tick(0.1f);
    CHECK_EQ(starts, 1);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_TRUE(r->Outputs->Get("done").AsBool());
}

TEST(AIGraph_parallel_any_cancels_the_loser) {
    World w;
    int cancelled = 0;
    w.RegisterTask(MakeTask("Forever", {}, [](TaskContext& t) {
        t.WaitEvent("never");
        return TaskStatus::Running;
    }, [&](TaskContext&) { ++cancelled; }));
    GraphDoc d;
    d.Name = "Race";
    const int start = Add(d, "flow.start");
    const int par = Add(d, "flow.parallel", {{"mode", Value(std::string("any"))}});
    const int wait = Add(d, "flow.wait", {{"seconds", Value(0.2f)}});
    const int run = Add(d, "task.run", {{"task", Value(std::string("Forever"))}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", par);
    Exec(d, par, "branch 0", wait);
    Exec(d, par, "branch 1", run);
    Exec(d, par, "completed", ret);
    RunPtr r = StartGraph(w, d);
    for (int i = 0; i < 4; ++i) w.Tick(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_EQ(cancelled, 1);
    CHECK_EQ((int)w.ActiveCount(), 0);
}

TEST(AIGraph_event_handlers_and_endless_loop_guard) {
    World w;
    GraphDoc d;
    d.Name = "Listener";
    d.Outputs = {{"heard", "vec3", Value(glm::vec3(0.0f)), ""}};
    const int on = Add(d, "event.on", {{"event", Value(std::string("noise"))}});
    const int ret = Add(d, "flow.return");
    Exec(d, on, "fired", ret);
    Data(d, on, "data", ret, "heard");
    RunPtr r = StartGraph(w, d);
    for (int i = 0; i < 3; ++i) w.Tick(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Running);   // ждёт событий, сам не кончается
    CHECK_EQ(w.Emit("noise", Value(glm::vec3(1, 2, 3))), 1);
    w.Tick(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_NEAR(r->Outputs->Get("heard").AsVec3().z, 3.0f, 1e-6f);

    // Бесконечный цикл без ожидания — неудача с причиной, а не зависание.
    GraphDoc loop;
    loop.Name = "Spin";
    const int s = Add(loop, "flow.start");
    const int wh = Add(loop, "flow.while", {{"condition", Value(true)}});
    const int log = Add(loop, "math.add");
    (void)log;
    Exec(loop, s, "then", wh);
    RunPtr x = StartGraph(w, loop);
    w.Tick(0.1f);
    CHECK_TRUE(x->Status == TaskStatus::Failed);
    CHECK_TRUE(x->Error.find("бесконечный") != std::string::npos);

    // Неизвестный узел: граф не запускается, причина называет узел.
    GraphDoc bad;
    bad.Name = "Bad";
    Add(bad, "flow.start");
    Add(bad, "my.missing_node");
    RunPtr b = StartGraph(w, bad);
    w.Tick(0.1f);
    CHECK_TRUE(b->Status == TaskStatus::Failed);
    CHECK_TRUE(b->Error.find("my.missing_node") != std::string::npos);
}

TEST(AIGraph_arrays_structs_and_rotation_nodes) {
    World w;
    GraphDoc d;
    d.Name = "Data";
    d.Outputs = {{"count", "int", Value(0), ""}, {"pos", "vec3", Value(glm::vec3(0.0f)), ""}};
    const int start = Add(d, "flow.start");
    const int make = Add(d, "array.make", {{"count", Value(2)}, {"item 0", Value(5)}, {"item 1", Value(7)}});
    const int push = Add(d, "array.push", {{"item", Value(9)}});
    const int len = Add(d, "array.length");
    const int tr = Add(d, "struct.make", {{"type", Value(std::string("transform"))},
                                           {"position", Value(glm::vec3(1, 0, 0))}});
    const int pt = Add(d, "transform.point", {{"point", Value(glm::vec3(0, 0, 0))}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", ret);
    Data(d, make, "array", push, "array");
    Data(d, push, "array", len, "array");
    Data(d, len, "length", ret, "count");
    Data(d, tr, "struct", pt, "transform");
    Data(d, pt, "point", ret, "pos");
    RunPtr r = StartGraph(w, d);
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_EQ(r->Outputs->Get("count").AsInt(), 3);
    CHECK_NEAR(r->Outputs->Get("pos").AsVec3().x, 1.0f, 1e-6f);
}

TEST(AIGraph_debugger_breakpoint_step_and_history) {
    GraphDebugger& dbg = GraphDebugger::Get();
    dbg.Reset();
    dbg.ClearBreakpoints();
    dbg.Enabled = true;
    World w;
    GraphDoc d;
    d.Name = "Dbg";
    const int start = Add(d, "flow.start");
    const int log = Add(d, "debug.log", {{"message", Value(std::string("привет"))}});
    const int ret = Add(d, "flow.return", {{"ok", Value(false)}, {"reason", Value(std::string("нарочно"))}});
    Exec(d, start, "then", log);
    Exec(d, log, "then", ret);
    dbg.ToggleBreakpoint("Dbg", log);
    RunPtr r = StartGraph(w, d);
    w.Tick(0.1f);
    w.Tick(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Running);
    CHECK_TRUE(dbg.IsPaused(r->Id));
    CHECK_EQ(dbg.FindRun(r->Id) ? dbg.FindRun(r->Id)->PausedNode : 0, log);
    dbg.Step(r->Id);   // выполнить Log и встать перед Return
    w.Tick(0.1f);
    CHECK_TRUE(dbg.IsPaused(r->Id));
    CHECK_EQ(dbg.FindRun(r->Id)->PausedNode, ret);
    dbg.Continue(r->Id);
    w.Tick(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Failed);
    CHECK_TRUE(r->Error == "нарочно");
    const RunDebug* rd = dbg.FindRun(r->Id);
    CHECK_TRUE(rd && rd->Status == "failed" && rd->Error == "нарочно");
    CHECK_TRUE(rd && rd->NodeHits.count(log) == 1);
    bool logged = false;
    for (const DebugEntry& e : dbg.History())
        if (e.Kind == "log" && e.Text == "привет") logged = true;
    CHECK_TRUE(logged);
    dbg.Enabled = false;
    dbg.ClearBreakpoints();
    dbg.Reset();
}
