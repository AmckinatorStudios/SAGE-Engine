// ПРИМЕР ИЗ КОНЦЕПЦИИ SAGE AI — целиком, как его собрал бы автор игры.
//
// NPC патрулирует (задача Patrol на Lua), слышит шум — AI Graph запускает
// InvestigateNoise (задача Visual Scripting: Move To к шуму), затем
// AnalyzeEvidence (Lua) и кладёт её результат в память; замечает игрока —
// ChasePlayer (Lua) прерывает всё, что важнее не она. Задачи, двигающие
// персонажа, занимают канал «legs» — два перемещения одновременно не идут.
// Движок здесь даёт только механизмы: решение с приоритетами, задачи,
// каналы, события, память; поведение задаёт «игра» в этом тесте.
#include "TestFramework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

using namespace sage::ai;
using sage::vars::Value;
namespace fs = std::filesystem;

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
int Mem(GraphDoc& d, const char* name) {
    return Add(d, "var.get", {{"scope", Str("instance")}, {"name", Str(name)}});
}
int SetMem(GraphDoc& d, const char* name) {
    return Add(d, "var.set", {{"scope", Str("instance")}, {"name", Str(name)}});
}

template <class T>
T Get(ScriptEngine& se, const std::string& expr) {
    return se.Lua().safe_script("return " + expr).get<T>();
}

// Библиотека ИИ «игры»: задачи на Lua.
const char* kLua = R"(
ai.task("Patrol", {
    channels = {"legs"},
    run = function(t)
        while true do
            t.wait(0.2)
            t.instance.patrol_steps = (t.instance.patrol_steps or 0) + 1
        end
    end,
})
ai.task("ChasePlayer", {
    channels = {"legs"},
    run = function(t)
        while t.instance.sees_player do
            t.instance.chase_frames = (t.instance.chase_frames or 0) + 1
            t.wait()
        end
        return true
    end,
})
ai.task("AnalyzeEvidence", {
    outputs = {found = false},
    run = function(t)
        t.wait(0.1)
        t.outputs.found = t.instance.noise_pos ~= nil
        return true
    end,
})
-- Посторонняя задача, тоже «на ногах», — для проверки каналов.
ai.task("Dance", {channels = {"legs"}, interruptible = false,
    run = function(t) while true do t.wait() end end})
)";

// Задача Visual Scripting: дойти до шума (Move To), осмотреться.
GraphDoc InvestigateNoise() {
    GraphDoc d;
    d.Kind = GraphKind::Task;
    d.Name = "InvestigateNoise";
    d.Channels = {"legs"};
    d.Inputs = {{"where", "vec3", Value(glm::vec3(0.0f)), ""}};
    const int start = Add(d, "flow.start");
    const int self = Add(d, "world.self");
    const int where = Add(d, "var.get", {{"scope", Str("input")}, {"name", Str("where")}});
    const int move = Add(d, "object.move_to", {{"speed", Value(40.0f)}, {"arrive_distance", Value(0.2f)}});
    const int look = Add(d, "flow.wait", {{"seconds", Value(0.1f)}});
    const int ret = Add(d, "flow.return");
    Exec(d, start, "then", move);
    Data(d, self, "object", move, "object");
    Data(d, where, "value", move, "target");
    Exec(d, move, "arrived", look);
    Exec(d, look, "then", ret);
    return d;
}

// AI Graph: решение с приоритетами + реакция на шум.
GraphDoc GuardBrain() {
    GraphDoc d;
    d.Kind = GraphKind::Behavior;
    d.Name = "GuardBrain";
    const int start = Add(d, "flow.start");
    const int decide = Add(d, "ai.decide", {{"options", Value(3)}, {"interval", Value(0.1f)},
                                            {"priority 0", Value(30)}, {"priority 1", Value(20)},
                                            {"priority 2", Value(10)}, {"condition 2", Value(true)}});
    Exec(d, start, "then", decide);
    // 0: видит игрока — преследовать.
    Data(d, Mem(d, "sees_player"), "value", decide, "condition 0");
    const int chase = Add(d, "task.run", {{"task", Str("ChasePlayer")}, {"priority", Value(30)}});
    Exec(d, decide, "option 0", chase);
    // 1: слышал шум — исследовать, затем разобрать улики (Lua), запомнить итог.
    Data(d, Mem(d, "heard"), "value", decide, "condition 1");
    const int inv = Add(d, "task.run", {{"task", Str("InvestigateNoise")}, {"priority", Value(20)}});
    Data(d, Mem(d, "noise_pos"), "value", inv, "where");
    Exec(d, decide, "option 1", inv);
    const int analyze = Add(d, "task.run", {{"task", Str("AnalyzeEvidence")}});
    Exec(d, inv, "succeeded", analyze);
    const int remember = SetMem(d, "evidence");
    Exec(d, analyze, "succeeded", remember);
    Data(d, analyze, "found", remember, "value");
    const int calm = SetMem(d, "heard");
    d.FindNode(calm)->Values["value"] = Value(false);
    Exec(d, remember, "then", calm);
    // 2: всегда — патруль.
    const int patrol = Add(d, "task.run", {{"task", Str("Patrol")}, {"priority", Value(10)}});
    Exec(d, decide, "option 2", patrol);
    // Шум: запомнить, где, и что слышал (On Event работает параллельно решению).
    const int on = Add(d, "event.on", {{"event", Str("noise")}});
    const int pos = SetMem(d, "noise_pos");
    const int heard = SetMem(d, "heard");
    d.FindNode(heard)->Values["value"] = Value(true);
    Exec(d, on, "fired", pos);
    Data(d, on, "data", pos, "value");
    Exec(d, pos, "then", heard);
    return d;
}

} // namespace

TEST(AIConcept_guard_patrols_investigates_chases_with_priorities) {
    GraphDebugger& dbg = GraphDebugger::Get();
    dbg.Reset();
    dbg.Enabled = true;

    // Проект «игры»: assets/ai — Lua и AI Graph, assets/visual — задача VS.
    const fs::path root = fs::temp_directory_path() / "sage_ai_concept";
    fs::remove_all(root);
    fs::create_directories(root / "assets" / "ai");
    fs::create_directories(root / "assets" / "visual");
    std::ofstream(root / "assets" / "ai" / "tasks.lua") << kLua;
    std::ofstream(root / "assets" / "ai" / "GuardBrain.aigraph") << SaveGraph(GuardBrain());
    std::ofstream(root / "assets" / "visual" / "InvestigateNoise.vgraph") << SaveGraph(InvestigateNoise());

    Scene scene{"concept"};
    ScriptEngine se;
    se.BindScene(scene);
    se.SetAiRoot(root.string());
    CHECK_EQ(se.LoadAiLibrary("assets/ai") + se.LoadAiLibrary("assets/visual"), 3);
    GameObject guard = scene.CreateObject("Guard");
    AiAgentComponent c;
    c.Graph = "assets/ai/GuardBrain.aigraph";
    c.Memory.Set("sees_player", Value(false));
    c.Memory.Set("heard", Value(false));
    scene.Registry().emplace<AiAgentComponent>(guard.Entity(), c);
    se.Lua()["guard"] = guard;
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) se.UpdateAll(0.1f);
    };

    // 1. Патруль — единственное истинное условие.
    frames(10);
    World& w = se.AI();
    CHECK_TRUE(Get<int>(se, "ai.agent_of(guard).memory.patrol_steps") >= 3);
    int patrolRun = 0;
    for (const auto& [id, r] : dbg.Runs())
        if (r.Task == "GuardBrain") CHECK_TRUE(r.LastDecision.find("вариант 2") != std::string::npos);
    for (const auto& [id, r] : dbg.Runs())
        if (r.Task == "Patrol" && r.Status == "running") patrolRun = id;
    CHECK_TRUE(patrolRun > 0);

    // 2. Шум: важнее патруля — патруль прерван с причиной, NPC идёт к шуму
    //    (задача VS), потом разбирает улики (Lua), итог — в памяти.
    CHECK_TRUE(se.Lua().safe_script("sage.events.Emit('noise', Vec3(6, 0, 0))").valid());
    frames(3);
    CHECK_TRUE(dbg.FindRun(patrolRun) && dbg.FindRun(patrolRun)->Status == "cancelled");
    CHECK_TRUE(dbg.FindRun(patrolRun)->Error.find("решение AI Graph") != std::string::npos);
    frames(10);
    CHECK_NEAR(guard.GetTransform().Position.x, 6.0f, 0.3f);
    CHECK_TRUE(Get<bool>(se, "ai.agent_of(guard).memory.evidence"));
    CHECK_FALSE(Get<bool>(se, "ai.agent_of(guard).memory.heard"));
    // Шум разобран — снова патруль.
    frames(5);
    bool patrolAgain = false;
    for (const auto& [id, r] : dbg.Runs())
        if (r.Task == "Patrol" && r.Status == "running" && id != patrolRun) patrolAgain = true;
    CHECK_TRUE(patrolAgain);

    // 3. Каналы: посторонняя задача «на ногах» с меньшим приоритетом не
    //    стартует, пока патруль держит ноги, и говорит, кто занял канал.
    const int agent = w.AgentOf(guard.Id());
    RunPtr dance = w.Start(agent, "Dance", {}, 0, 5);
    CHECK_TRUE(dance->Status == TaskStatus::Failed && !dance->Rejected);
    CHECK_TRUE(dance->Error.find("канал 'legs' занят задачей 'Patrol'") != std::string::npos);

    // 4. Игрок замечен — преследование важнее всего, патруль прерван.
    CHECK_TRUE(se.Lua().safe_script("ai.agent_of(guard).memory.sees_player = true").valid());
    frames(4);
    CHECK_TRUE(Get<int>(se, "ai.agent_of(guard).memory.chase_frames") > 0);
    // 5. Непрерываемая задача с высшим приоритетом всё равно берёт ноги, но её
    //    саму уже не прервать: преследование с приоритетом 30 не стартует.
    RunPtr dance2 = w.Start(agent, "Dance", {}, 0, 100);
    CHECK_TRUE(dance2->Status == TaskStatus::Running);
    RunPtr chase = w.Start(agent, "ChasePlayer", {}, 0, 30);
    CHECK_TRUE(chase->Status == TaskStatus::Failed);
    CHECK_TRUE(chase->Error.find("непрерываемая") != std::string::npos);
    w.Cancel(dance2->Id, "тест: хватит");
    CHECK_TRUE(dance2->Error == "тест: хватит");

    // 6. Профиль: видно, что сколько стоило.
    const auto& stats = w.Stats();
    CHECK_TRUE(stats.count("Patrol") && stats.at("Patrol").Runs >= 2 && stats.at("Patrol").Cancelled >= 1);
    CHECK_TRUE(stats.count("InvestigateNoise") && stats.at("InvestigateNoise").Succeeded == 1);
    CHECK_TRUE(stats.count("GuardBrain") && stats.at("GuardBrain").Steps > 0);
    bool decisions = false;
    for (const DebugEntry& e : dbg.History())
        if (e.Kind == "decision" && e.Text.find("важнее") != std::string::npos) decisions = true;
    CHECK_TRUE(decisions);

    dbg.Enabled = false;
    dbg.Reset();
    fs::remove_all(root);
}

TEST(AIConcept_noninterruptible_option_finishes_before_switch) {
    ScriptEngine se;
    World& w = se.AI();
    CHECK_TRUE(se.Lua().safe_script(R"(
        ai.task("Eat", {run = function(t) t.wait(0.5); t.instance.ate = true; return true end})
        ai.task("Flee", {run = function(t) t.instance.fled = true; t.wait(10) end})
    )").valid());
    GraphDoc d;
    d.Kind = GraphKind::Behavior;
    d.Name = "Hungry";
    const int start = Add(d, "flow.start");
    const int decide = Add(d, "ai.decide", {{"options", Value(2)}, {"interval", Value(0.0f)},
                                            {"priority 0", Value(50)}, {"priority 1", Value(10)},
                                            {"condition 1", Value(true)}, {"interruptible 1", Value(false)}});
    Exec(d, start, "then", decide);
    Data(d, Mem(d, "danger"), "value", decide, "condition 0");
    const int flee = Add(d, "task.run", {{"task", Str("Flee")}});
    const int eat = Add(d, "task.run", {{"task", Str("Eat")}});
    Exec(d, decide, "option 0", flee);
    Exec(d, decide, "option 1", eat);
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    const int agent = w.CreateAgent();
    w.FindAgent(agent)->Memory->Set("danger", Value(false));
    w.Start(agent, "Hungry");
    w.Tick(0.1f);
    w.FindAgent(agent)->Memory->Set("danger", Value(true));   // опасность — но еду не прервать
    for (int i = 0; i < 3; ++i) w.Tick(0.1f);
    CHECK_FALSE(w.FindAgent(agent)->Memory->Get("fled").AsBool());
    for (int i = 0; i < 5; ++i) w.Tick(0.1f);
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("ate").AsBool());
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("fled").AsBool());   // доел — и бежать
}

// Узел-действие ИИ на Lua (ai.node без domain) нужен и задаче Visual
// Scripting: задачи живут там, а не в AI Graph. Раньше такой узел годился
// только графу решений, и граф задачи его отвергал.
TEST(AIConcept_lua_ai_node_usable_in_task_graph) {
    ScriptEngine se;
    World& w = se.AI();
    CHECK_TRUE(se.Lua().safe_script(R"(
        ai.node("game.bark", {run = function(n) n.instance.barked = true end})
        ai.node("game.think", {domain = "ai", run = function(n) end})
    )").valid());
    const NodeType* bark = w.Nodes().Find("game.bark");
    const NodeType* think = w.Nodes().Find("game.think");
    CHECK_TRUE(bark && think);
    if (!bark || !think) return;
    CHECK_TRUE(NodeAllowed(bark->Domains, GraphKind::Task));
    CHECK_TRUE(NodeAllowed(bark->Domains, GraphKind::Behavior));
    CHECK_FALSE(NodeAllowed(think->Domains, GraphKind::Task));
    GraphDoc d;
    d.Kind = GraphKind::Task;
    d.Name = "Bark";
    Exec(d, Add(d, "flow.start"), "then", Add(d, "game.bark"));
    std::string err;
    CHECK_TRUE(w.RegisterGraph(d, err));
    const int agent = w.CreateAgent();
    w.Start(agent, "Bark");
    w.Tick(0.1f);
    CHECK_TRUE(w.FindAgent(agent)->Memory->Get("barked").AsBool());
}
