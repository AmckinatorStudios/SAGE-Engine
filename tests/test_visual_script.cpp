// Visual Scripting и SAGE AI Graph — одна основа с разными наборами узлов:
// назначение узлов (домены), проверка графа, входы On Start / On Update,
// общие графы-функции и их вызов из Lua, свои узлы для каждого редактора,
// компонент Visual Script на объекте сцены, общая шина событий.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "sage/ai/GraphValidate.h"
#include "sage/ai/World.h"
#include "sage/scene/Scene.h"
#include "sage/scene/VisualScriptComponent.h"
#include "sage/scripting/ScriptEngine.h"

using namespace sage::ai;
using sage::vars::Value;

namespace {

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

bool HasIssue(const std::vector<GraphIssue>& v, int node, bool error, const std::string& part) {
    for (const GraphIssue& i : v)
        if (i.Node == node && i.Error == error && i.Text.find(part) != std::string::npos) return true;
    return false;
}

} // namespace

TEST(VisualScript_node_domains_split_editors) {
    World w;
    auto allowed = [&](const char* id, GraphKind k) {
        const NodeType* t = w.Nodes().Find(id);
        CHECK_TRUE(t != nullptr);
        return t && NodeAllowed(t->Domains, k);
    };
    // AI Graph — только узлы ИИ: никакого Branch, Sequence потока, циклов,
    // переменных Script Graph и событий объекта.
    for (const char* id : {"ai.task_start", "ai.sequence", "ai.selector", "ai.guard", "ai.repeat", "ai.decide",
                           "ai.succeed", "ai.fail", "ai.abort", "ai.check", "ai.compare", "ai.get_param",
                           "ai.set_param", "ai.wait_event", "task.run", "task.call", "ai.can_run"}) {
        CHECK_TRUE(allowed(id, GraphKind::Behavior));
        CHECK_FALSE(allowed(id, GraphKind::Script));
        CHECK_FALSE(allowed(id, GraphKind::Function));
    }
    for (const char* id : {"flow.start", "flow.branch", "flow.sequence", "flow.while", "flow.parallel", "flow.wait",
                           "var.get", "var.set", "math.add", "graph.call", "event.on", "script.on_start",
                           "script.on_update", "debug.log"}) {
        CHECK_FALSE(allowed(id, GraphKind::Behavior));
        CHECK_TRUE(allowed(id, GraphKind::Script));
    }
    // События объекта — не в функции: у функции нет своего объекта.
    CHECK_FALSE(allowed("script.on_update", GraphKind::Function));
    CHECK_TRUE(allowed("flow.branch", GraphKind::Function));
    // Излом провода нужен обоим.
    CHECK_TRUE(allowed("flow.reroute", GraphKind::Behavior));
    CHECK_TRUE(allowed("flow.reroute", GraphKind::Script));

    // Узел ИИ в Script Graph: проверка называет узел, запуск не начинается.
    GraphDoc d;
    d.Kind = GraphKind::Script;
    d.Name = "Wrong";
    const int start = Add(d, "script.on_start");
    const int task = Add(d, "task.call", {{"task", Str("X")}});
    Exec(d, start, "then", task);
    CHECK_TRUE(HasIssue(ValidateGraph(d, w), task, true, "не для Script Graph"));
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    RunPtr r = w.Start(w.CreateAgent(), "Wrong");
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Failed);
    CHECK_TRUE(r->Error.find("не для этого вида графа") != std::string::npos);

    // Узел игровой логики в AI Graph — то же самое.
    GraphDoc ai;
    ai.Kind = GraphKind::Behavior;
    ai.Name = "WrongAi";
    const int as = Add(ai, "ai.task_start");
    const int br = Add(ai, "flow.branch");
    Exec(ai, as, "then", br);
    CHECK_TRUE(HasIssue(ValidateGraph(ai, w), br, true, "не для AI Graph"));
    CHECK_TRUE(w.RegisterGraph(ai, err));
    RunPtr r2 = w.Start(w.CreateAgent(), "WrongAi");
    w.Tick(0.016f);
    CHECK_TRUE(r2->Status == TaskStatus::Failed);
    CHECK_TRUE(r2->Error.find("Branch") != std::string::npos);

    // Без Task Start AI Graph не запускается; два начала — тоже ошибка.
    GraphDoc none;
    none.Kind = GraphKind::Behavior;
    none.Name = "NoStart";
    Add(none, "ai.succeed");
    CHECK_TRUE(HasIssue(ValidateGraph(none, w), 0, true, "Task Start"));
    CHECK_TRUE(w.RegisterGraph(none, err));
    RunPtr r3 = w.Start(w.CreateAgent(), "NoStart");
    w.Tick(0.016f);
    CHECK_TRUE(r3->Status == TaskStatus::Failed && r3->Error.find("Task Start") != std::string::npos);
}

TEST(VisualScript_file_extension_decides_the_graph_kind) {
    std::string err;
    // Расширение и вид совпадают.
    CHECK_TRUE(CheckGraphFile("brain.aigraph", GraphKind::Behavior, err));
    CHECK_TRUE(CheckGraphFile("door.sgraph", GraphKind::Script, err));
    CHECK_TRUE(CheckGraphFile("dmg.sgraph", GraphKind::Function, err));
    CHECK_TRUE(CheckGraphFile("old.vgraph", GraphKind::Task, err));   // устаревший файл — как был
    // Ручная правка вида в файле не обходит правила.
    CHECK_FALSE(CheckGraphFile("brain.aigraph", GraphKind::Script, err));
    CHECK_TRUE(err.find("не совпадают") != std::string::npos);
    CHECK_FALSE(CheckGraphFile("door.sgraph", GraphKind::Behavior, err));
    CHECK_FALSE(CheckGraphFile("door.sgraph", GraphKind::Task, err));
    CHECK_FALSE(CheckGraphFile("x.vgraph", GraphKind::Behavior, err));
    CHECK_FALSE(CheckGraphFile("notes.txt", GraphKind::Script, err));
    CHECK_TRUE(std::string(GraphFileExt(GraphKind::Behavior)) == ".aigraph");
    CHECK_TRUE(std::string(GraphFileExt(GraphKind::Script)) == ".sgraph");
    CHECK_TRUE(IsGraphFile("A.SGRAPH") && IsGraphFile("b.aigraph") && !IsGraphFile("c.slua"));
}

TEST(VisualScript_validation_finds_problems_before_play) {
    World w;
    GraphDoc d;
    d.Kind = GraphKind::Script;
    d.Name = "Checks";
    const int set = Add(d, "var.set", {{"scope", Str("local")}, {"name", Str("missing")}});
    const int run = Add(d, "graph.call");
    const int vec = Add(d, "vec.make3");
    const int br = Add(d, "flow.branch");
    d.Link({vec, "vector"}, {br, "condition"}, false);   // вектор в условие — нельзя
    const std::vector<GraphIssue> issues = ValidateGraph(d, w);
    CHECK_TRUE(HasIssue(issues, 0, true, "нет события"));
    CHECK_TRUE(HasIssue(issues, set, true, "нет переменной 'missing'"));
    CHECK_TRUE(HasIssue(issues, run, true, "не выбрана функция"));
    CHECK_TRUE(HasIssue(issues, br, true, "не подходит"));
    CHECK_TRUE(HasIssue(issues, set, false, "не ведёт поток"));
}

TEST(VisualScript_on_start_and_on_update_without_overlap) {
    World w;
    GraphDoc d;
    d.Kind = GraphKind::Script;
    d.Name = "Ticker";
    d.Locals = {{"frames", "int", Value(0), ""}, {"time", "float", Value(0.0f), ""}};
    const int start = Add(d, "script.on_start");
    const int init = Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str("started")}, {"value", Value(true)}});
    Exec(d, start, "then", init);
    const int upd = Add(d, "script.on_update");
    const int get = Add(d, "var.get", {{"scope", Str("local")}, {"name", Str("frames")}});
    const int add = Add(d, "math.add", {{"b", Value(1)}});
    const int set = Add(d, "var.set", {{"scope", Str("local")}, {"name", Str("frames")}});
    const int wait = Add(d, "flow.wait", {{"seconds", Value(0.25f)}});
    Exec(d, upd, "then", set);
    Data(d, get, "value", add, "a");
    Data(d, add, "result", set, "value");
    Exec(d, set, "then", wait);
    std::string err;
    CHECK_TRUE(ValidateGraph(d, w).empty() || !ValidateGraph(d, w)[0].Error);
    CHECK_TRUE(w.RegisterGraph(d, err));
    const int agent = w.CreateAgent();
    RunPtr r = w.Start(agent, "Ticker");
    for (int i = 0; i < 10; ++i) w.Tick(0.1f);   // 1 секунда, кадр ждёт 0.25 с
    CHECK_TRUE(r->Status == TaskStatus::Running);   // скрипт объекта сам не кончается
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("started").AsBool());
    // Кадр не заводится, пока прошлый ждёт: не 10 срабатываний, а 3–4.
    const int frames = r->Locals->Get("frames").AsInt();
    CHECK_TRUE(frames >= 3 && frames <= 4);
}

TEST(VisualScript_lua_nodes_functions_and_component) {
    Scene scene{"vs"};
    ScriptEngine se;
    se.BindScene(scene);
    CHECK_TRUE(Lua(se, R"(
        heard = nil
        sage.events.On("door_opened", function(e) heard = e end)
        visual.node("game.double", {pure = true, inputs = {{"x", "float", 1}}, outputs = {{"y", "float"}},
            run = function(n) n.outputs.y = n.inputs.x * 2 end})
        ai.node("ai.only", {run = function(n) end})
        ai.node("shared.both", {domain = "both", run = function(n) end})
    )"));
    World& w = se.AI();
    CHECK_TRUE(NodeAllowed(w.Nodes().Find("game.double")->Domains, GraphKind::Script));
    CHECK_TRUE(NodeAllowed(w.Nodes().Find("game.double")->Domains, GraphKind::Task));
    // Действия над миром — в задачах и скриптах; AI Graph решает, а не действует.
    CHECK_FALSE(NodeAllowed(w.Nodes().Find("object.set_position")->Domains, GraphKind::Behavior));
    CHECK_TRUE(NodeAllowed(w.Nodes().Find("object.set_position")->Domains, GraphKind::Task));
    CHECK_FALSE(NodeAllowed(w.Nodes().Find("game.double")->Domains, GraphKind::Behavior));
    CHECK_FALSE(NodeAllowed(w.Nodes().Find("ai.only")->Domains, GraphKind::Script));
    CHECK_TRUE(NodeAllowed(w.Nodes().Find("shared.both")->Domains, GraphKind::Function));

    // Граф-функция — общая библиотека: Lua зовёт её и сразу получает выходы.
    GraphDoc fn;
    fn.Kind = GraphKind::Function;
    fn.Name = "Damage";
    fn.Inputs = {{"base", "float", Value(1.0f), ""}, {"armor", "float", Value(0.0f), ""}};
    fn.Outputs = {{"result", "float", Value(0.0f), ""}};
    {
        const int s = Add(fn, "flow.start");
        const int b = Add(fn, "var.get", {{"scope", Str("input")}, {"name", Str("base")}});
        const int a = Add(fn, "var.get", {{"scope", Str("input")}, {"name", Str("armor")}});
        const int sub = Add(fn, "math.sub");
        const int ret = Add(fn, "flow.return");
        Exec(fn, s, "then", ret);
        Data(fn, b, "value", sub, "a");
        Data(fn, a, "value", sub, "b");
        Data(fn, sub, "result", ret, "result");
    }
    std::string err;
    CHECK_TRUE(w.RegisterGraph(fn, err));
    CHECK_NEAR(Get<float>(se, "visual.call('Damage', {base = 10, armor = 3}).result"), 7.0f, 1e-5f);
    CHECK_FALSE(Lua(se, "visual.call('NoSuchFunction')"));

    // Скрипт объекта: On Update двигает объект на 2*dt по X через функцию и
    // Lua-узел, On Start шлёт событие игры.
    GraphDoc vs;
    vs.Kind = GraphKind::Script;
    vs.Name = "Mover";
    {
        const int st = Add(vs, "script.on_start");
        const int emit = Add(vs, "script.emit", {{"event", Str("door_opened")}, {"data", Value(5)}});
        Exec(vs, st, "then", emit);
        const int upd = Add(vs, "script.on_update");
        const int self = Add(vs, "world.self");
        const int info = Add(vs, "object.info");
        const int dbl = Add(vs, "game.double");
        const int step = Add(vs, "vec.make3");
        const int sum = Add(vs, "math.add");
        const int move = Add(vs, "object.set_position");
        Exec(vs, upd, "then", move);
        Data(vs, self, "object", info, "object");
        Data(vs, upd, "dt", dbl, "x");
        Data(vs, dbl, "y", step, "x");
        Data(vs, info, "position", sum, "a");
        Data(vs, step, "vector", sum, "b");
        Data(vs, sum, "result", move, "position");
        Data(vs, self, "object", move, "object");
    }
    const std::string path = "sage_vs_mover.sgraph";
    std::ofstream(path, std::ios::binary) << SaveGraph(vs);
    GameObject box = scene.CreateObject("Box");
    VisualScriptComponent c;
    c.Graph = path;
    c.Vars.Set("speed", Value(2.0f));
    scene.Registry().emplace<VisualScriptComponent>(box.Entity(), c);
    for (int i = 0; i < 6; ++i) se.UpdateAll(0.1f);
    CHECK_NEAR(box.GetTransform().Position.x, 1.0f, 0.05f);   // 5 кадров с dt, первый — dt = 0
    CHECK_EQ(Get<int>(se, "heard"), 5);
    se.Lua()["box"] = box;
    CHECK_NEAR(Get<float>(se, "visual.of(box).vars.speed"), 2.0f, 1e-6f);
    std::remove(path.c_str());
}
