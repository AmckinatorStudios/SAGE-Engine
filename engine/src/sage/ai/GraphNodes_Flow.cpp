#include <algorithm>
#include <string>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// Встроенные узлы: поток выполнения, задачи, события, переменные, вызовы.
// Данные и математика — в GraphNodes_Data.cpp.
//
// Ни один узел здесь не знает «поведения»: Wait ждёт, Call Task запускает то,
// что ему назвали. Что персонаж делает — решает рисунок автора.
// ---------------------------------------------------------------------------
namespace sage::ai {

using vars::Value;
using namespace nodes;

void NodeRegistry::Register(NodeType type) {
    if (type.Id.empty()) return;
    if (type.Title.empty()) type.Title = type.Id;
    m_types[type.Id] = std::move(type);
    ++m_version;
}

const NodeType* NodeRegistry::Find(const std::string& id) const {
    auto it = m_types.find(id);
    return it == m_types.end() ? nullptr : &it->second;
}

std::vector<const NodeType*> NodeRegistry::All() const {
    std::vector<const NodeType*> out;
    for (const auto& [_, t] : m_types) out.push_back(&t);
    return out;
}

const PinSpec* FindPin(const std::vector<PinSpec>& pins, const std::string& name) {
    for (const PinSpec& p : pins)
        if (p.Name == name) return &p;
    return nullptr;
}

NodePins ResolvePins(const NodeType& type, const GraphNode& node, const GraphEnv& env) {
    NodePins p;
    p.Inputs = type.Inputs;
    p.Outputs = type.Outputs;
    p.Pure = type.Pure;
    if (type.Pins) type.Pins(node, env, p);
    return p;
}

namespace nodes {

PinSpec P(const std::string& name, const std::string& type, Value def, const std::string& title) {
    PinSpec p;
    p.Name = name;
    p.Type = type;
    p.Default = std::move(def);
    p.Title = title;
    return p;
}

PinSpec X(const std::string& name, const std::string& title) { return P(name, type::Exec, Value(), title); }

SettingSpec S(const std::string& name, const std::string& kind, Value def, const std::string& title,
              std::vector<std::string> choices) {
    SettingSpec s;
    s.Name = name;
    s.Kind = kind;
    s.Default = std::move(def);
    s.Title = title;
    s.Choices = std::move(choices);
    return s;
}

NodeType Node(const std::string& id, const std::string& title, const std::string& category,
              const std::string& description) {
    NodeType t;
    t.Id = id;
    t.Title = title;
    t.Category = category;
    t.Description = description;
    return t;
}

std::string SettingOf(const GraphNode& n, const std::string& name, const std::string& fallback) {
    auto it = n.Values.find(name);
    return it == n.Values.end() ? fallback : it->second.AsString();
}

int SettingIntOf(const GraphNode& n, const std::string& name, int fallback) {
    auto it = n.Values.find(name);
    return it == n.Values.end() ? fallback : it->second.AsInt();
}

std::string TypeOfValue(const GraphEnv& env, const Value& v) {
    return env.Types ? env.Types->Of(v) : std::string(type::Any);
}

} // namespace nodes

namespace {

// Пины по подписи задачи: входы — входами узла, выходы — выходами.
void TaskPins(const GraphNode& n, const GraphEnv& env, NodePins& p, bool inputs, bool outputs,
              const char* key = "task") {
    if (!env.W) return;
    TaskDefinitionPtr def = env.W->FindTask(SettingOf(n, key));
    if (!def) return;
    if (inputs)
        for (const Param& in : def->Signature.Inputs)
            p.Inputs.push_back(P(in.Name, TypeOfValue(env, in.Default), in.Default));
    if (outputs)
        for (const Param& out : def->Signature.Outputs)
            p.Outputs.push_back(P(out.Name, TypeOfValue(env, out.Default), out.Default));
}

Blackboard TaskInputs(GraphExec& ex) {
    Blackboard in;
    for (const PinSpec& pin : ex.Pins().Inputs)
        if (pin.Type != type::Exec) in.Set(pin.Name, ex.In(pin.Name));
    return in;
}

void TaskResult(GraphExec& ex, const RunPtr& r) {
    ex.Out("ok", Value(r->Status == TaskStatus::Succeeded));
    ex.Out("status", Value(std::string(StatusName(r->Status))));
    ex.Out("error", Value(r->Error));
    for (const auto& [name, v] : r->Outputs->All()) ex.Out(name, v);
}

std::string VarType(const GraphDoc* doc, const std::string& scope, const std::string& name) {
    if (!doc) return type::Any;
    const std::vector<GraphVar>* list = scope == "local"    ? &doc->Locals
                                        : scope == "input"  ? &doc->Inputs
                                        : scope == "output" ? &doc->Outputs
                                                            : nullptr;
    if (!list) return type::Any;
    const GraphVar* v = doc->FindVar(*list, name);
    return v ? v->Type : std::string(type::Any);
}

const std::vector<std::string> kScopes = {"local", "instance", "shared", "input", "output"};

void RegisterFlow(NodeRegistry& r) {
    {
        NodeType t = Node("flow.start", "Start", "Flow", "Where the graph begins.");
        t.Entry = true;
        t.Outputs = {X("then")};
        t.Run = [](GraphExec&) { return NodeResult::Go("then"); };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.sequence", "Sequence", "Flow", "Runs its outputs one after another.");
        t.Inputs = {X("in")};
        t.Settings = {S("count", "int", Value(2), "Outputs")};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "count", 2), 1, 16);
            for (int i = 0; i < count; ++i) p.Outputs.push_back(X("then " + std::to_string(i)));
        };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const int count = std::clamp(ex.SettingInt("count"), 1, 16);
            if (!ex.Reentered()) s.Counter = 0;
            if (s.Counter < count - 1) return NodeResult::Push("then " + std::to_string(s.Counter++));
            s.Counter = 0;
            return NodeResult::Go("then " + std::to_string(count - 1));
        };
        t.Keywords = {"then", "order", "steps"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.branch", "Branch", "Flow", "Goes one way or the other by a condition.");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(false))};
        t.Outputs = {X("true"), X("false")};
        t.Run = [](GraphExec& ex) { return NodeResult::Go(ex.In("condition").AsBool() ? "true" : "false"); };
        t.Keywords = {"if", "condition"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.while", "Loop While", "Flow", "Repeats the body while the condition holds.");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(true))};
        t.Outputs = {X("body"), X("completed")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Reentered()) s.Broken = false;
            if (s.Broken) {
                s.Broken = false;
                return NodeResult::Go("completed");
            }
            return ex.In("condition").AsBool() ? NodeResult::Push("body") : NodeResult::Go("completed");
        };
        t.Keywords = {"repeat", "loop"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.for", "For Loop", "Flow", "Repeats the body for each index from first to last.");
        t.Inputs = {X("in"), P("first", type::Int, Value(0)), P("last", type::Int, Value(9))};
        t.Outputs = {X("body"), P("index", type::Int, Value(0)), X("completed")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Reentered()) {
                s.Counter = ex.In("first").AsInt();
                s.Limit = ex.In("last").AsInt();
                s.Broken = false;
            }
            if (s.Broken || s.Counter > s.Limit) {
                s.Broken = false;
                return NodeResult::Go("completed");
            }
            ex.Out("index", Value(s.Counter++));
            return NodeResult::Push("body");
        };
        t.Keywords = {"repeat", "loop", "count"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.for_each", "For Each", "Flow", "Runs the body for every item of an array.");
        t.Inputs = {X("in"), P("array", type::Array, Value::Array())};
        t.Outputs = {X("body"), P("item", type::Any), P("index", type::Int, Value(0)), X("completed")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Reentered()) {
                s.Data = ex.In("array");
                s.Counter = 0;
                s.Broken = false;
            }
            const std::vector<Value>& items = s.Data.Items();
            if (s.Broken || s.Counter >= (int)items.size()) {
                s.Broken = false;
                return NodeResult::Go("completed");
            }
            ex.Out("item", items[s.Counter]);
            ex.Out("index", Value(s.Counter++));
            return NodeResult::Push("body");
        };
        t.Keywords = {"array", "loop", "list"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.break", "Break", "Flow", "Leaves the nearest loop.");
        t.Inputs = {X("in")};
        t.Run = [](GraphExec&) { return NodeResult::Break(); };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.parallel", "Parallel", "Flow",
                          "Runs its branches at the same time and waits for all (or the first) of them.");
        t.Inputs = {X("in")};
        t.Settings = {S("branches", "int", Value(2), "Branches"),
                      S("mode", "choice", Value(std::string("all")), "Wait for", {"all", "any"})};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "branches", 2), 1, 16);
            for (int i = 0; i < count; ++i) p.Outputs.push_back(X("branch " + std::to_string(i)));
            p.Outputs.push_back(X("completed"));
        };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const bool all = ex.SettingText("mode") != "any";
            if (!ex.Resumed()) {
                s.Fibers.clear();
                const int count = std::clamp(ex.SettingInt("branches"), 1, 16);
                for (int i = 0; i < count; ++i)
                    if (const int f = ex.Spawn("branch " + std::to_string(i))) s.Fibers.push_back(f);
                if (s.Fibers.empty()) return NodeResult::Go("completed");
                ex.WaitFibers(s.Fibers, all);
                return NodeResult::Wait();
            }
            // «Любая»: первая кончилась — остальные снимаются вместе с тем, что
            // они запустили и ждут.
            if (!all)
                for (int f : s.Fibers) ex.Kill(f, "Parallel: первой кончилась другая ветка");
            s.Fibers.clear();
            return NodeResult::Go("completed");
        };
        t.Keywords = {"concurrent", "together", "race"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.wait", "Wait", "Flow", "Waits for a number of seconds.");
        t.Inputs = {X("in"), P("seconds", type::Float, Value(1.0f))};
        t.Outputs = {X("then")};
        t.Run = [](GraphExec& ex) {
            if (ex.Resumed()) return NodeResult::Go("then");
            ex.WaitSeconds(ex.In("seconds").AsFloat());
            return NodeResult::Wait();
        };
        t.Keywords = {"delay", "sleep", "time"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.wait_until", "Wait Until", "Flow",
                          "Waits until the condition holds (checked every frame), or until the timeout.");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(false)), P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("then"), X("timeout")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Resumed()) {
                const float timeout = ex.In("timeout").AsFloat();
                s.Until = timeout > 0.0f ? ex.GetWorld().Time() + timeout : 0.0;
            }
            if (ex.In("condition").AsBool()) return NodeResult::Go("then");
            if (s.Until > 0.0 && ex.GetWorld().Time() >= s.Until) return NodeResult::Go("timeout");
            ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"condition", "until"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.return", "Return", "Flow",
                          "Finishes the graph: success or failure, and the values of its outputs.");
        t.Inputs = {X("in"), P("ok", type::Bool, Value(true)), P("reason", type::String, Value(std::string()))};
        t.Pins = [](const GraphNode&, const GraphEnv& env, NodePins& p) {
            if (!env.Doc) return;
            for (const GraphVar& v : env.Doc->Outputs) p.Inputs.push_back(P(v.Name, v.Type, v.Default));
        };
        t.Run = [](GraphExec& ex) {
            for (const GraphVar& v : ex.Doc().Outputs) ex.Task().Outputs().Set(v.Name, ex.In(v.Name));
            ex.Finish(ex.In("ok").AsBool(), ex.In("reason").AsString());
            return NodeResult::Return();
        };
        t.Keywords = {"finish", "result", "succeed", "fail"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.end", "End Branch", "Flow", "Stops this branch of execution.");
        t.Inputs = {X("in")};
        t.Run = [](GraphExec&) { return NodeResult::End(); };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.select", "Select", "Flow", "Picks one of two values by a condition.");
        t.Pure = true;
        t.Inputs = {P("condition", type::Bool, Value(false)), P("a", type::Any), P("b", type::Any)};
        t.Outputs = {P("result", type::Any)};
        t.Run = [](GraphExec& ex) {
            ex.Out("result", ex.In("condition").AsBool() ? ex.In("a") : ex.In("b"));
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("flow.reroute", "Reroute", "Flow", "A bend in a wire: passes the value as is.");
        t.Pure = true;
        t.Inputs = {P("value", type::Any)};
        t.Outputs = {P("value", type::Any)};
        t.Run = [](GraphExec& ex) {
            ex.Out("value", ex.In("value"));
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
}

void RegisterTasks(NodeRegistry& r) {
    {
        NodeType t = Node("task.call", "Call Task", "Tasks",
                          "Starts a task (graph, Lua or C++) and goes on at once. The task output is its handle.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Inputs = {X("in")};
        t.Outputs = {X("then"), P("task", type::Task, Value(0))};
        // Приоритет запуска решает спор за каналы персонажа (TaskSignature::Channels).
        t.Settings = {S("task", "task", Value(std::string()), "Task"), S("priority", "int", Value(0), "Priority")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) { TaskPins(n, env, p, true, false); };
        t.Run = [](GraphExec& ex) {
            RunPtr run = ex.StartTask(ex.SettingText("task"), TaskInputs(ex), ex.SettingInt("priority"));
            ex.Out("task", Value(run->Id));
            return NodeResult::Go("then");
        };
        t.Keywords = {"start", "launch"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("task.run", "Run Task", "Tasks",
                          "Starts a task and waits for it: goes on by success or failure with its outputs.");
        t.Inputs = {X("in")};
        t.Outputs = {X("succeeded"), X("failed"), P("ok", type::Bool, Value(false)),
                     P("error", type::String, Value(std::string()))};
        // Приоритет запуска решает спор за каналы персонажа (TaskSignature::Channels).
        t.Settings = {S("task", "task", Value(std::string()), "Task"), S("priority", "int", Value(0), "Priority")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) { TaskPins(n, env, p, true, true); };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Resumed()) {
                s.Task = ex.StartTask(ex.SettingText("task"), TaskInputs(ex), ex.SettingInt("priority"));
                ex.Out("task", Value(s.Task->Id));
                if (!IsFinished(s.Task->Status)) {
                    ex.WaitRun(s.Task);
                    return NodeResult::Wait();
                }
            }
            RunPtr run = s.Task;
            s.Task.reset();
            if (!run) return NodeResult::Go("failed");
            TaskResult(ex, run);
            return NodeResult::Go(run->Status == TaskStatus::Succeeded ? "succeeded" : "failed");
        };
        t.Keywords = {"call", "await", "subgraph", "function"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("task.wait", "Wait For Task", "Tasks", "Waits until a started task finishes.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Inputs = {X("in"), P("task", type::Task, Value(0))};
        t.Outputs = {X("succeeded"), X("failed"), P("ok", type::Bool, Value(false)),
                     P("status", type::String, Value(std::string())), P("error", type::String, Value(std::string()))};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Resumed()) {
                s.Task = ex.TaskById(ex.In("task").AsInt());
                if (!s.Task) ex.Error("нет задачи #" + std::to_string(ex.In("task").AsInt()));
                if (!IsFinished(s.Task->Status)) {
                    ex.WaitRun(s.Task);
                    return NodeResult::Wait();
                }
            }
            RunPtr run = s.Task;
            s.Task.reset();
            if (!run) return NodeResult::Go("failed");
            TaskResult(ex, run);
            return NodeResult::Go(run->Status == TaskStatus::Succeeded ? "succeeded" : "failed");
        };
        t.Keywords = {"await", "join"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("task.cancel", "Cancel Task", "Tasks", "Cancels a running task and its children.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Inputs = {X("in"), P("task", type::Task, Value(0))};
        t.Outputs = {X("then")};
        t.Run = [](GraphExec& ex) {
            if (RunPtr run = ex.TaskById(ex.In("task").AsInt())) ex.GetWorld().Cancel(run->Id);
            return NodeResult::Go("then");
        };
        t.Keywords = {"stop", "abort"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("task.status", "Task Status", "Tasks", "State of a task: running, done, its error.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Pure = true;
        t.Inputs = {P("task", type::Task, Value(0))};
        t.Outputs = {P("status", type::String, Value(std::string())), P("done", type::Bool, Value(false)),
                     P("ok", type::Bool, Value(false)), P("error", type::String, Value(std::string()))};
        t.Run = [](GraphExec& ex) {
            RunPtr run = ex.TaskById(ex.In("task").AsInt());
            ex.Out("status", Value(std::string(run ? StatusName(run->Status) : "none")));
            ex.Out("done", Value(run && IsFinished(run->Status)));
            ex.Out("ok", Value(run && run->Status == TaskStatus::Succeeded));
            ex.Out("error", Value(run ? run->Error : std::string()));
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("task.output", "Task Output", "Tasks", "One output value of a task.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Pure = true;
        t.Inputs = {P("task", type::Task, Value(0))};
        t.Outputs = {P("value", type::Any)};
        t.Settings = {S("output", "string", Value(std::string()), "Output")};
        t.Run = [](GraphExec& ex) {
            RunPtr run = ex.TaskById(ex.In("task").AsInt());
            ex.Out("value", run ? run->Outputs->Get(ex.SettingText("output")) : Value());
            return NodeResult::Go("");
        };
        r.Register(std::move(t));
    }
}

void RegisterEvents(NodeRegistry& r) {
    {
        NodeType t = Node("event.on", "On Event", "Events",
                          "Runs every time the event arrives, whatever the graph is doing.");
        t.Entry = true;
        t.Outputs = {X("fired"), P("data", type::Any)};
        t.Settings = {S("event", "event", Value(std::string()), "Event")};
        t.Run = [](GraphExec&) { return NodeResult::Go("fired"); };
        t.Keywords = {"listen", "subscribe", "handler"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("event.wait", "Wait For Event", "Events", "Waits for an event, or until the timeout.");
        t.Inputs = {X("in"), P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("received"), X("timeout"), P("data", type::Any)};
        t.Settings = {S("event", "event", Value(std::string()), "Event")};
        t.Run = [](GraphExec& ex) {
            if (!ex.Resumed()) {
                ex.WaitEvent(ex.SettingText("event"), ex.In("timeout").AsFloat());
                return NodeResult::Wait();
            }
            if (ex.TimedOut()) return NodeResult::Go("timeout");
            ex.Out("data", ex.EventPayload());
            return NodeResult::Go("received");
        };
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("event.emit", "Emit Event", "Events",
                          "Sends an event to every AI, to the AI of one object, or to itself.");
        t.Domains = kDomainAI;   // управление задачами — только в AI Graph
        t.Inputs = {X("in"), P("data", type::Any), P("target", type::Object, Value(vars::EntityRef{}))};
        t.Outputs = {X("then")};
        t.Settings = {S("event", "event", Value(std::string()), "Event"),
                      S("self_only", "bool", Value(false), "Only to itself")};
        t.Run = [](GraphExec& ex) {
            World& w = ex.GetWorld();
            int agent = 0;
            if (ex.Setting("self_only").AsBool()) agent = ex.Task().Agent();
            else if (const int owner = ex.In("target").AsEntity().Id; owner > 0) {
                agent = w.AgentOf(owner);
                if (!agent) return NodeResult::Go("then");   // у объекта нет ИИ — слушать некому
            }
            w.Emit(ex.SettingText("event"), ex.In("data"), agent);
            return NodeResult::Go("then");
        };
        t.Keywords = {"send", "broadcast", "signal"};
        r.Register(std::move(t));
    }
}

void RegisterVars(NodeRegistry& r) {
    {
        NodeType t = Node("var.get", "Get Variable", "Variables", "Reads a variable of the graph or the AI.");
        t.Pure = true;
        t.Settings = {S("scope", "choice", Value(std::string("local")), "Scope", kScopes),
                      S("name", "var", Value(std::string()), "Variable")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            p.Outputs.push_back(P("value", VarType(env.Doc, SettingOf(n, "scope", "local"), SettingOf(n, "name"))));
        };
        t.Run = [](GraphExec& ex) {
            Blackboard* b = ex.Scope(ex.SettingText("scope"));
            if (!b) ex.Error("неизвестная область переменных '" + ex.SettingText("scope") + "'");
            ex.Out("value", b->Get(ex.SettingText("name")));
            return NodeResult::Go("");
        };
        t.Keywords = {"read", "memory", "blackboard"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("var.set", "Set Variable", "Variables", "Writes a variable of the graph or the AI.");
        t.Inputs = {X("in")};
        t.Outputs = {X("then")};
        t.Settings = {S("scope", "choice", Value(std::string("local")), "Scope", kScopes),
                      S("name", "var", Value(std::string()), "Variable")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            const std::string type = VarType(env.Doc, SettingOf(n, "scope", "local"), SettingOf(n, "name"));
            p.Inputs.push_back(P("value", type));
            p.Outputs.push_back(P("value", type));
        };
        t.Run = [](GraphExec& ex) {
            Blackboard* b = ex.Scope(ex.SettingText("scope"));
            if (!b) ex.Error("неизвестная область переменных '" + ex.SettingText("scope") + "'");
            const Value v = ex.In("value");
            std::string err;
            if (!b->Set(ex.SettingText("name"), v, &err)) ex.Error(err);
            ex.Out("value", v);
            return NodeResult::Go("then");
        };
        t.Keywords = {"write", "assign", "memory", "blackboard"};
        r.Register(std::move(t));
    }
}

void RegisterCalls(NodeRegistry& r) {
    {
        // ОБЩИЙ вызов переиспользуемого графа: граф-функция — библиотека для
        // обоих редакторов, и звать её одинаково должны и ИИ, и Visual
        // Scripting. Под капотом — то же выполнение задачи, но только графа
        // вида function: «Run Task» с задачами ИИ остаётся в SAGE AI Graph.
        NodeType t = Node("graph.call", "Call Function", "Functions",
                          "Runs a function graph with its inputs and goes on when it returns, with its outputs.");
        t.Inputs = {X("in")};
        t.Outputs = {X("then"), X("failed"), P("error", type::String, Value(std::string()))};
        t.Settings = {S("function", "function", Value(std::string()), "Function")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            TaskPins(n, env, p, true, true, "function");
        };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            if (!ex.Resumed()) {
                const std::string name = ex.SettingText("function");
                std::shared_ptr<const GraphDoc> g = ex.GetWorld().FindGraph(name);
                if (!g || g->Kind != GraphKind::Function) ex.Error("«" + name + "» — не граф-функция");
                s.Task = ex.StartTask(name, TaskInputs(ex));
                if (!IsFinished(s.Task->Status)) {
                    ex.WaitRun(s.Task);
                    return NodeResult::Wait();
                }
            }
            RunPtr run = s.Task;
            s.Task.reset();
            if (!run) return NodeResult::Go("failed");
            TaskResult(ex, run);
            return NodeResult::Go(run->Status == TaskStatus::Succeeded ? "then" : "failed");
        };
        t.Keywords = {"subgraph", "macro", "reuse", "library"};
        r.Register(std::move(t));
    }
    {
        // Входы Visual Scripting: как Start/Update скрипта объекта.
        NodeType t = Node("script.on_start", "On Start", "Events", "Runs once when the object starts in Play.");
        t.Domains = kDomainObject;   // вход скрипта объекта
        t.Entry = true;
        t.Outputs = {X("then")};
        t.Run = [](GraphExec&) { return NodeResult::Go("then"); };
        t.Keywords = {"begin", "start", "init"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("script.on_update", "On Update", "Events",
                          "Runs every frame; dt — seconds since the previous frame. A frame does not start "
                          "while the previous one still waits.");
        t.Domains = kDomainObject;   // вход скрипта объекта
        t.Entry = true;
        t.Outputs = {X("then"), P("dt", type::Float, Value(0.0f))};
        t.Run = [](GraphExec&) { return NodeResult::Go("then"); };
        t.Keywords = {"tick", "frame", "update"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("call.op", "Call Operation", "Functions",
                          "Calls an operation by name — C++, Lua or engine API — and gives its results.");
        t.Settings = {S("operation", "operation", Value(std::string()), "Operation"),
                      S("pure", "bool", Value(false), "Without flow pins")};
        t.Pins = [](const GraphNode& n, const GraphEnv& env, NodePins& p) {
            const bool pure = n.Values.count("pure") && n.Values.at("pure").AsBool();
            p.Pure = pure;
            if (!pure) {
                p.Inputs.push_back(X("in"));
                p.Outputs.push_back(X("then"));
            }
            const Operation* op = env.W ? env.W->FindOperation(SettingOf(n, "operation")) : nullptr;
            if (op && !op->Inputs.empty()) {
                for (const Param& in : op->Inputs) p.Inputs.push_back(P(in.Name, TypeOfValue(env, in.Default), in.Default));
            } else {
                p.Inputs.push_back(P("args", type::Array, Value::Array()));
            }
            if (op && !op->Outputs.empty()) {
                for (const Param& out : op->Outputs)
                    p.Outputs.push_back(P(out.Name, TypeOfValue(env, out.Default), out.Default));
            } else {
                p.Outputs.push_back(P("result", type::Any));
            }
        };
        t.Run = [](GraphExec& ex) {
            const std::string name = ex.SettingText("operation");
            const Operation* op = ex.GetWorld().FindOperation(name);
            Values in, out;
            if (op && !op->Inputs.empty()) {
                for (const Param& p : op->Inputs) in.push_back(ex.In(p.Name));
            } else {
                in = ex.In("args").Items();
            }
            std::string err;
            if (!ex.GetWorld().Call(name, in, out, err)) ex.Error(err);
            if (op && !op->Outputs.empty()) {
                for (size_t i = 0; i < op->Outputs.size() && i < out.size(); ++i) ex.Out(op->Outputs[i].Name, out[i]);
            } else {
                ex.Out("result", out.empty() ? Value() : out[0]);
            }
            return NodeResult::Go(ex.Pins().Pure ? "" : "then");
        };
        t.Keywords = {"function", "invoke", "lua", "api"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("debug.log", "Log", "Debug", "Writes a message to the log and the AI debugger.");
        t.Inputs = {X("in"), P("message", type::String, Value(std::string()))};
        t.Outputs = {X("then")};
        t.Run = [](GraphExec& ex) {
            ex.Log(ex.In("message").AsString());
            return NodeResult::Go("then");
        };
        t.Keywords = {"print", "trace"};
        r.Register(std::move(t));
    }
}

} // namespace

void RegisterBuiltinNodes(NodeRegistry& nodes) {
    RegisterFlow(nodes);
    RegisterTasks(nodes);
    RegisterEvents(nodes);
    RegisterVars(nodes);
    RegisterCalls(nodes);
    nodes::RegisterDataNodes(nodes);
    nodes::RegisterDecisionNodes(nodes);
}

} // namespace sage::ai
