// AI GRAPH: жизненный цикл задачи и управление поведением — по их
// определениям. Каждая проверка — то, что узел ОБЕЩАЕТ: Sequence падает на
// первом неудачном шаге, Selector берёт первый удачный вариант, Repeat не
// перезапускает в одном кадре, неудача задачи без провода валит ветку, Abort
// отменяет всё. Плюс задачи поведения в мире (цель, следование,
// взаимодействие) и события Script Graph (постоянный шаг, сигналы объекта).
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/GraphRuntime.h"
#include "sage/ai/World.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/scene/VisualScriptComponent.h"
#include "sage/scripting/ScriptEngine.h"

using namespace sage::ai;
using sage::vars::EntityRef;
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

GraphDoc Behavior(const char* name) {
    GraphDoc d;
    d.Kind = GraphKind::Behavior;
    d.Name = name;
    return d;
}

// Задача-счётчик: пишет своё имя в память «log» и кончается успехом или
// неудачей (вход ok).
void RegisterStep(World& w, const std::string& name, bool ok, int* starts = nullptr) {
    TaskSignature sig;
    w.RegisterTask(MakeTask(name, sig, [name, ok, starts](TaskContext& t) {
        if (starts) ++*starts;
        const std::string prev = t.Instance().Has("log") ? t.Instance().Get("log").AsString() : std::string();
        t.Instance().Set("log", Value(prev + name + ";"));
        if (!ok) return t.Fail(name + " не смогла");
        return TaskStatus::Succeeded;
    }));
}

RunPtr Launch(World& w, const GraphDoc& d, int& agent, int ticks) {
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    agent = w.CreateAgent();
    RunPtr r = w.Start(agent, d.Name);
    for (int i = 0; i < ticks; ++i) w.Tick(0.1f);
    return r;
}

std::string Log(World& w, int agent) {
    const Blackboard& m = *w.FindAgent(agent)->Memory;
    return m.Has("log") ? m.Get("log").AsString() : std::string();
}

} // namespace

TEST(AIBehavior_sequence_stops_at_the_first_failure_and_fails_the_task) {
    World w;
    RegisterStep(w, "A", true);
    RegisterStep(w, "B", false);
    RegisterStep(w, "C", true);
    GraphDoc d = Behavior("Seq");
    const int start = Add(d, "ai.task_start");
    const int seq = Add(d, "ai.sequence", {{"steps", Value(3)}});
    Exec(d, start, "then", seq);
    Exec(d, seq, "step 0", Add(d, "task.run", {{"task", Str("A")}}));
    Exec(d, seq, "step 1", Add(d, "task.run", {{"task", Str("B")}}));
    Exec(d, seq, "step 2", Add(d, "task.run", {{"task", Str("C")}}));
    Exec(d, seq, "success", Add(d, "ai.succeed"));
    int agent = 0;
    RunPtr r = Launch(w, d, agent, 3);
    CHECK_TRUE(Log(w, agent) == "A;B;");   // C не запускалась
    // Выход failure не соединён — неудача поднимается до задачи с причиной.
    CHECK_TRUE(r->Status == TaskStatus::Failed);
    CHECK_TRUE(r->Error.find("B не смогла") != std::string::npos);
    CHECK_TRUE(r->Error.find("шаг 1") != std::string::npos);
}

TEST(AIBehavior_selector_takes_the_first_option_that_succeeds) {
    World w;
    RegisterStep(w, "A", false);
    RegisterStep(w, "B", true);
    RegisterStep(w, "C", true);
    GraphDoc d = Behavior("Sel");
    const int start = Add(d, "ai.task_start");
    const int sel = Add(d, "ai.selector", {{"options", Value(3)}});
    Exec(d, start, "then", sel);
    Exec(d, sel, "option 0", Add(d, "task.run", {{"task", Str("A")}}));
    Exec(d, sel, "option 1", Add(d, "task.run", {{"task", Str("B")}}));
    Exec(d, sel, "option 2", Add(d, "task.run", {{"task", Str("C")}}));
    const int ok = Add(d, "ai.succeed");
    Exec(d, sel, "success", ok);
    int agent = 0;
    RunPtr r = Launch(w, d, agent, 3);
    CHECK_TRUE(Log(w, agent) == "A;B;");
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);

    // Все варианты неудачны — неудача с причиной последнего.
    World w2;
    RegisterStep(w2, "A", false);
    GraphDoc all = Behavior("SelFail");
    const int s2 = Add(all, "ai.task_start");
    const int sel2 = Add(all, "ai.selector", {{"options", Value(1)}});
    Exec(all, s2, "then", sel2);
    Exec(all, sel2, "option 0", Add(all, "task.run", {{"task", Str("A")}}));
    RunPtr r2 = Launch(w2, all, agent, 3);
    CHECK_TRUE(r2->Status == TaskStatus::Failed);
    CHECK_TRUE(r2->Error.find("ни один вариант") != std::string::npos);
}

TEST(AIBehavior_repeat_runs_each_round_on_its_own_frame_and_stops_by_rule) {
    World w;
    int starts = 0;
    RegisterStep(w, "Tick", true, &starts);
    GraphDoc d = Behavior("Rep");
    const int start = Add(d, "ai.task_start");
    const int rep = Add(d, "ai.repeat", {{"count", Value(3)}});
    Exec(d, start, "then", rep);
    Exec(d, rep, "body", Add(d, "task.run", {{"task", Str("Tick")}}));
    Exec(d, rep, "success", Add(d, "ai.succeed"));
    int agent = 0;
    RunPtr r = Launch(w, d, agent, 1);
    CHECK_EQ(starts, 1);   // круг — не раньше следующего кадра, а не три в одном
    for (int i = 0; i < 8; ++i) w.Tick(0.1f);
    CHECK_EQ(starts, 3);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);

    // «До неудачи»: третий круг неудачен — Repeat кончается неудачей.
    World w2;
    int n = 0;
    w2.RegisterTask(MakeTask("Flaky", {}, [&n](TaskContext& t) {
        return ++n >= 3 ? t.Fail("сломалось на круге 3") : TaskStatus::Succeeded;
    }));
    GraphDoc u = Behavior("Until");
    const int s2 = Add(u, "ai.task_start");
    const int rep2 = Add(u, "ai.repeat", {{"count", Value(0)}, {"stop_on", Str("failure")}});
    Exec(u, s2, "then", rep2);
    Exec(u, rep2, "body", Add(u, "task.run", {{"task", Str("Flaky")}}));
    RunPtr r2 = Launch(w2, u, agent, 6);
    CHECK_EQ(n, 3);
    CHECK_TRUE(r2->Status == TaskStatus::Failed);
    CHECK_TRUE(r2->Error.find("круг 3") != std::string::npos);
}

TEST(AIBehavior_succeed_fail_abort_and_last_result) {
    World w;
    RegisterStep(w, "Nope", false);
    // Fail с причиной: задача кончается неудачей ровно с ней.
    GraphDoc f = Behavior("Fails");
    Exec(f, Add(f, "ai.task_start"), "then", Add(f, "ai.fail", {{"reason", Str("дверь заперта")}}));
    int agent = 0;
    RunPtr r = Launch(w, f, agent, 1);
    CHECK_TRUE(r->Status == TaskStatus::Failed && r->Error == "дверь заперта");

    // Неудача задачи, обработанная проводом: Last Result видит, что и почему.
    GraphDoc h = Behavior("Handled");
    const int s = Add(h, "ai.task_start");
    const int nope = Add(h, "task.run", {{"task", Str("Nope")}});
    const int note = Add(h, "ai.set_param", {{"scope", Str("instance")}, {"name", Str("why")}});
    const int last = Add(h, "ai.last_result");
    Exec(h, s, "then", nope);
    Exec(h, nope, "failed", note);
    Data(h, last, "reason", note, "value");
    Exec(h, note, "then", Add(h, "ai.succeed"));
    RunPtr r2 = Launch(w, h, agent, 2);
    CHECK_TRUE(r2->Status == TaskStatus::Succeeded);
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("why").AsString() == "Nope не смогла");

    // Abort: поведение отменяется целиком, идущая задача — тоже.
    int cancelled = 0;
    w.RegisterTask(MakeTask("Long", {}, [](TaskContext& t) {
        t.WaitSeconds(100.0f);
        return TaskStatus::Running;
    }, [&](TaskContext&) { ++cancelled; }));
    GraphDoc a = Behavior("Aborts");
    const int as = Add(a, "ai.task_start");
    const int call = Add(a, "task.call", {{"task", Str("Long")}});
    Exec(a, as, "then", call);
    Exec(a, call, "then", Add(a, "ai.abort", {{"reason", Str("тревога")}}));
    RunPtr r3 = Launch(w, a, agent, 2);
    CHECK_TRUE(r3->Status == TaskStatus::Cancelled);
    CHECK_TRUE(r3->Error == "тревога");
    CHECK_EQ(cancelled, 1);
}

TEST(AIBehavior_conditions_state_and_waits) {
    World w;
    TaskSignature legs;
    legs.Channels = {"legs"};
    legs.Interruptible = false;
    w.RegisterTask(MakeTask("Hold", legs, [](TaskContext& t) {
        t.WaitSeconds(100.0f);
        return TaskStatus::Running;
    }));
    w.RegisterTask(MakeTask("Walk", legs, [](TaskContext&) { return TaskStatus::Succeeded; }));

    // Can Run Task: канал занят непрерываемой задачей — нельзя, с причиной.
    GraphDoc d = Behavior("Checks");
    const int s = Add(d, "ai.task_start");
    const int set = Add(d, "ai.set_state", {{"state", Str("alert")}});
    const int can = Add(d, "ai.can_run", {{"task", Str("Walk")}, {"priority", Value(50)}});
    const int keep = Add(d, "ai.set_param", {{"scope", Str("instance")}, {"name", Str("can")}});
    const int inState = Add(d, "ai.in_state", {{"state", Str("alert")}});
    const int check = Add(d, "ai.check");
    const int waitEv = Add(d, "ai.wait_event", {{"event", Str("go")}, {"timeout", Value(0.25f)}});
    Exec(d, s, "then", set);
    Exec(d, set, "then", keep);
    Data(d, can, "result", keep, "value");
    Exec(d, keep, "then", check);
    Data(d, inState, "result", check, "condition");
    Exec(d, check, "success", waitEv);
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    const int agent = w.CreateAgent();
    w.Start(agent, "Hold");
    RunPtr r = w.Start(agent, "Checks");
    w.Tick(0.1f);
    CHECK_FALSE(w.FindAgent(agent)->Memory->Get("can").AsBool());
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("state").AsString() == "alert");
    CHECK_TRUE(r->Status == TaskStatus::Running);   // ждёт событие
    for (int i = 0; i < 4; ++i) w.Tick(0.1f);
    // Событие не пришло — Wait For Event кончается неудачей, задача тоже.
    CHECK_TRUE(r->Status == TaskStatus::Failed);
    CHECK_TRUE(r->Error.find("'go' не пришло") != std::string::npos);

    // Compare Parameter и All Of.
    GraphDoc c = Behavior("Cmp");
    const int cs = Add(c, "ai.task_start");
    const int hp = Add(c, "ai.compare", {{"name", Str("hp")}, {"op", Str("<")}, {"value", Value(30)}});
    const int has = Add(c, "ai.compare", {{"name", Str("ammo")}, {"op", Str("is set")}});
    const int both = Add(c, "ai.all", {{"count", Value(2)}});
    const int chk = Add(c, "ai.check", {{"reason", Str("рано отступать")}});
    Data(c, hp, "result", both, "condition 0");
    Data(c, has, "result", both, "condition 1");
    Data(c, both, "result", chk, "condition");
    Exec(c, cs, "then", chk);
    Exec(c, chk, "success", Add(c, "ai.succeed"));
    CHECK_TRUE(w.RegisterGraph(c, err));
    const int a2 = w.CreateAgent();
    w.FindAgent(a2)->Memory->Set("hp", Value(20));
    RunPtr r2 = w.Start(a2, "Cmp");
    w.Tick(0.1f);
    CHECK_TRUE(r2->Status == TaskStatus::Failed && r2->Error == "рано отступать");   // патронов нет
    w.FindAgent(a2)->Memory->Set("ammo", Value(5));
    RunPtr r3 = w.Start(a2, "Cmp");
    w.Tick(0.1f);
    CHECK_TRUE(r3->Status == TaskStatus::Succeeded);
}

TEST(AIBehavior_debugger_sees_task_outcomes_on_nodes) {
    GraphDebugger& dbg = GraphDebugger::Get();
    dbg.Reset();
    dbg.Enabled = true;
    World w;
    w.RegisterTask(MakeTask("Slow", {}, [](TaskContext& t) {
        if (t.Phase()++ == 0) {
            t.WaitSeconds(0.3f);
            return TaskStatus::Running;
        }
        return TaskStatus::Succeeded;
    }));
    GraphDoc d = Behavior("Watched");
    const int s = Add(d, "ai.task_start");
    const int run = Add(d, "task.run", {{"task", Str("Slow")}});
    Exec(d, s, "then", run);
    Exec(d, run, "succeeded", Add(d, "ai.succeed"));
    int agent = 0;
    RunPtr r = Launch(w, d, agent, 1);
    RunDebug* rd = dbg.FindRun(r->Id);
    CHECK_TRUE(rd && rd->Outcomes[run] == "running");
    for (int i = 0; i < 5; ++i) w.Tick(0.1f);
    CHECK_TRUE(rd && rd->Outcomes[run] == "succeeded");
    dbg.Enabled = false;
    dbg.Reset();
}

TEST(AIBehavior_world_tasks_target_follow_and_interact) {
    Scene scene{"behavior"};
    ScriptEngine se;
    se.BindScene(scene);
    GameObject npc = scene.CreateObject("Npc");
    GameObject enemy = scene.CreateObject("Enemy Orc");
    enemy.GetTransform().Position = glm::vec3(10, 0, 0);
    GameObject far = scene.CreateObject("Enemy Far");
    far.GetTransform().Position = glm::vec3(80, 0, 0);
    World& w = se.AI();
    // Find Target (ближайший «Enemy») -> Follow Target 0.5 с -> Can See / Distance -> Face Target.
    GraphDoc d = Behavior("Hunt");
    const int s = Add(d, "ai.task_start");
    const int find = Add(d, "ai.find_target", {{"name_contains", Str("Enemy")}, {"radius", Value(30.0f)}});
    const int follow = Add(d, "ai.follow", {{"keep_distance", Value(2.0f)}, {"speed", Value(20.0f)},
                                            {"duration", Value(0.5f)}});
    const int near = Add(d, "ai.target_near", {{"distance", Value(2.5f)}});
    const int check = Add(d, "ai.check", {{"reason", Str("не догнал")}});
    const int face = Add(d, "ai.face_target");
    Exec(d, s, "then", find);
    Exec(d, find, "success", follow);
    Exec(d, follow, "success", check);
    Data(d, near, "result", check, "condition");
    Exec(d, check, "success", face);
    Exec(d, face, "success", Add(d, "ai.succeed"));
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    const int agent = w.CreateAgent(npc.Id());
    RunPtr r = w.Start(agent, "Hunt");
    for (int i = 0; i < 12; ++i) se.UpdateAll(0.1f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("target").AsEntity().Id == enemy.Id());   // ближний, не дальний
    const float x = npc.GetTransform().Position.x;
    CHECK_TRUE(x > 7.0f && x < 8.5f);   // держит дистанцию, а не лезет в цель

    // Цель пропала посреди следования — Follow Target кончается неудачей.
    GraphDoc lost = Behavior("Lost");
    const int ls = Add(lost, "ai.task_start");
    Exec(lost, ls, "then", Add(lost, "ai.follow", {{"duration", Value(0.0f)}}));
    CHECK_TRUE(w.RegisterGraph(lost, err));
    RunPtr r2 = w.Start(agent, "Lost");
    se.UpdateAll(0.1f);
    CHECK_TRUE(r2->Status == TaskStatus::Running);
    scene.RemoveObject(enemy.Id());
    se.UpdateAll(0.1f);
    CHECK_TRUE(r2->Status == TaskStatus::Failed);
    CHECK_TRUE(r2->Error.find("цель пропала") != std::string::npos);

    // Interact без цели — неудача с причиной, а не тихий успех.
    GraphDoc use = Behavior("Use");
    const int us = Add(use, "ai.task_start");
    const int lua = Add(use, "ai.interact", {{"function", Str("missing_fn_for_sure")}});
    Exec(use, us, "then", lua);
    Exec(use, lua, "success", Add(use, "ai.succeed"));
    CHECK_TRUE(w.RegisterGraph(use, err));
    w.FindAgent(agent)->Memory->Set("target", Value(EntityRef{}));
    RunPtr r3 = w.Start(agent, "Use");
    se.UpdateAll(0.1f);
    CHECK_TRUE(r3->Status == TaskStatus::Failed);
    CHECK_TRUE(r3->Error.find("объекта нет") != std::string::npos);
}

TEST(ScriptGraph_fixed_update_and_object_signals) {
    Scene scene{"sg"};
    ScriptEngine se;
    se.BindScene(scene);
    GameObject door = scene.CreateObject("Door");
    // Script Graph: On Fixed Update считает шаги, On Collision Enter и On
    // Signal(clicked) пишут в переменные объекта.
    GraphDoc d;
    d.Kind = GraphKind::Script;
    d.Name = "DoorLogic";
    const int fixed = Add(d, "script.on_fixed_update");
    const int get = Add(d, "var.get", {{"scope", Str("instance")}, {"name", Str("steps")}});
    const int add = Add(d, "math.add", {{"b", Value(1)}});
    const int set = Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str("steps")}});
    Exec(d, fixed, "then", set);
    Data(d, get, "value", add, "a");
    Data(d, add, "result", set, "value");
    const int hit = Add(d, "script.on_collision");
    const int who = Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str("hit_by")}});
    Exec(d, hit, "then", who);
    Data(d, hit, "other", who, "value");
    const int click = Add(d, "script.on_signal", {{"signal", Str("clicked")}});
    const int clicked = Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str("clicked")}, {"value", Value(true)}});
    Exec(d, click, "then", clicked);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "sage_door_logic.sgraph";
    std::ofstream(file, std::ios::binary) << SaveGraph(d);
    VisualScriptComponent c;
    c.Graph = file.string();
    scene.Registry().emplace<VisualScriptComponent>(door.Entity(), c);
    for (int i = 0; i < 3; ++i) {
        se.UpdateFixed(0.1f);
        se.UpdateAll(0.1f);
    }
    World& w = se.AI();
    sage::ai::Agent* a = w.FindAgent(w.AgentOf(door.Id()));
    CHECK_TRUE(a != nullptr);
    if (!a) return;
    CHECK_TRUE(a->Memory->Get("steps").AsInt() >= 3);
    scene.Events.EmitSignal(door.Id(), sage::signals::kCollision, Value(EntityRef{42}));
    scene.Events.EmitSignal(door.Id(), sage::signals::kClicked);
    se.UpdateAll(0.1f);
    CHECK_EQ(a->Memory->Get("hit_by").AsEntity().Id, 42);
    CHECK_TRUE(a->Memory->Get("clicked").AsBool());
    std::filesystem::remove(file);
}
