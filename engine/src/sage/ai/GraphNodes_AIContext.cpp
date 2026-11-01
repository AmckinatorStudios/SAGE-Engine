#include <algorithm>
#include <cmath>
#include <string>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// AI GRAPH: УСЛОВИЯ, КОНТЕКСТ И ОЖИДАНИЯ.
//
// УСЛОВИЯ — чистые узлы с логическим результатом (Compare Parameter, In
// State, Can Run Task, All/Any/Not) и узел потока Check Condition, который
// превращает условие в успех или неудачу ветки. На результат условий
// опираются If Condition, Decide и Selector.
//
// КОНТЕКСТ — данные поведения по области и времени жизни:
//   instance — память ЭТОГО персонажа: живёт, пока жив его ИИ, и переживает
//              конец и перезапуск задач (цель, состояние, улики);
//   shared   — общая область (стража, торговцы);
//   local    — данные одного выполнения поведения;
//   input / output — входы и выходы самой задачи.
// Last Result — итог предыдущей задачи этого выполнения (что, удалось ли,
// почему нет): так следующий шаг опирается на результат прошлого.
//
// ОЖИДАНИЯ — задачи поведения с результатом: Wait For Event кончается
// неудачей по таймауту, Wait Until — когда условие так и не стало истинным.
// Узел запускает ожидание один раз и ждёт; кадр за кадром он не стартует.
// ---------------------------------------------------------------------------
namespace sage::ai::nodes {

using vars::Kind;
using vars::Value;

namespace {

std::string Num(int i) { return std::to_string(i); }

NodeType AiNode(const std::string& id, const std::string& title, const std::string& category,
                const std::string& description) {
    NodeType t = Node(id, title, category, description);
    t.Domains = kDomainAI;
    return t;
}

const std::vector<std::string> kAiScopes = {"instance", "shared", "local", "input", "output"};

bool IsNumber(const Value& v) { return v.Type() == Kind::Int || v.Type() == Kind::Float || v.Type() == Kind::Bool; }

bool CompareValues(const Value& a, const Value& b, const std::string& op) {
    if (IsNumber(a) && IsNumber(b)) {
        const float x = a.AsFloat(), y = b.AsFloat();
        if (op == "==") return std::fabs(x - y) < 1e-6f;
        if (op == "!=") return std::fabs(x - y) >= 1e-6f;
        if (op == "<") return x < y;
        if (op == "<=") return x <= y;
        if (op == ">") return x > y;
        if (op == ">=") return x >= y;
        return false;
    }
    if (op == "==") return a == b || a.AsString() == b.AsString();
    if (op == "!=") return !(a == b || a.AsString() == b.AsString());
    const std::string x = a.AsString(), y = b.AsString();
    if (op == "<") return x < y;
    if (op == "<=") return x <= y;
    if (op == ">") return x > y;
    if (op == ">=") return x >= y;
    return false;
}

Blackboard& NeedScope(GraphExec& ex) {
    Blackboard* b = ex.Scope(ex.SettingText("scope"));
    if (!b) ex.Error("неизвестная область данных '" + ex.SettingText("scope") + "'");
    return *b;
}

void RegisterConditions(NodeRegistry& r) {
    auto pure = [&r](NodeType t) {
        t.Pure = true;
        r.Register(std::move(t));
    };
    {
        NodeType t = AiNode("ai.check", "Check Condition", "Conditions",
                            "Succeeds if the condition holds, fails (with the reason) if not.");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(true))};
        t.Outputs = {X("success"), X("failure")};
        t.Settings = {S("reason", "string", Value(std::string()), "Reason when false")};
        t.Run = [](GraphExec& ex) {
            if (ex.In("condition").AsBool()) return NodeResult::Go("success");
            const std::string why = ex.SettingText("reason");
            ex.FailReason(why.empty() ? "условие Check Condition #" + Num(ex.Node().Id) + " не выполнено" : why);
            return NodeResult::Go("failure");
        };
        t.Keywords = {"if", "test", "assert", "require"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.compare", "Compare Parameter", "Conditions",
                            "Compares a behavior parameter with a value (or checks that it is set).");
        t.Inputs = {P("value", type::Any)};
        t.Outputs = {P("result", type::Bool, Value(false)), P("current", type::Any)};
        t.Settings = {S("scope", "choice", Value(std::string("instance")), "Scope", kAiScopes),
                      S("name", "var", Value(std::string()), "Parameter"),
                      S("op", "choice", Value(std::string("==")), "Comparison",
                        {"==", "!=", "<", "<=", ">", ">=", "is set"})};
        t.Run = [](GraphExec& ex) {
            Blackboard& b = NeedScope(ex);
            const std::string name = ex.SettingText("name");
            const std::string op = ex.SettingText("op");
            const Value cur = b.Get(name);
            ex.Out("current", cur);
            ex.Out("result", Value(op == "is set" ? b.Has(name) && b.Get(name).AsString() != ""
                                                  : b.Has(name) && CompareValues(cur, ex.In("value"), op)));
            return NodeResult::Go("");
        };
        t.Keywords = {"memory", "blackboard", "equal", "greater", "less", "parameter"};
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.in_state", "In State", "Conditions",
                            "Whether the character is in this behavior state (see Set State).");
        t.Inputs = {P("state", type::String, Value(std::string()))};
        t.Outputs = {P("result", type::Bool, Value(false)), P("current", type::String)};
        t.Run = [](GraphExec& ex) {
            const std::string cur = ex.Task().Instance().Get("state").AsString();
            ex.Out("current", Value(cur));
            ex.Out("result", Value(cur == ex.In("state").AsString()));
            return NodeResult::Go("");
        };
        t.Keywords = {"state", "mode", "phase"};
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.can_run", "Can Run Task", "Conditions",
                            "Whether the task could start now: it exists and its channels are not held by "
                            "more important or uninterruptible tasks of this character.");
        t.Outputs = {P("result", type::Bool, Value(false)), P("reason", type::String)};
        t.Settings = {S("task", "task", Value(std::string()), "Task"), S("priority", "int", Value(0), "Priority")};
        t.Run = [](GraphExec& ex) {
            std::string why;
            const bool ok = ex.GetWorld().CanStart(ex.Task().Agent(), ex.SettingText("task"), ex.SettingInt("priority"),
                                                   ex.Task().GetRun().Id, why);
            ex.Out("result", Value(ok));
            ex.Out("reason", Value(why));
            return NodeResult::Go("");
        };
        t.Keywords = {"possible", "available", "channel", "busy"};
        pure(std::move(t));
    }
    // И / ИЛИ / НЕ над условиями — своими узлами: логика Script Graph в AI
    // Graph не заходит, а сложить «вижу цель И она близко» поведению нужно.
    for (const bool all : {true, false}) {
        NodeType t = AiNode(all ? "ai.all" : "ai.any", all ? "All Of" : "Any Of", "Conditions",
                            all ? "True when every condition is true." : "True when at least one condition is true.");
        t.Settings = {S("count", "int", Value(2), "Conditions")};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "count", 2), 2, 8);
            for (int i = 0; i < count; ++i) p.Inputs.push_back(P("condition " + Num(i), type::Bool, Value(false)));
            p.Outputs.push_back(P("result", type::Bool, Value(false)));
        };
        t.Run = [all](GraphExec& ex) {
            const int count = std::clamp(ex.SettingInt("count"), 2, 8);
            bool res = all;
            for (int i = 0; i < count; ++i) {
                const bool c = ex.In("condition " + Num(i)).AsBool();
                if (all && !c) res = false;
                if (!all && c) res = true;
            }
            ex.Out("result", Value(res));
            return NodeResult::Go("");
        };
        t.Keywords = all ? std::vector<std::string>{"and", "both"} : std::vector<std::string>{"or", "either"};
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.not", "Not", "Conditions", "The opposite of the condition.");
        t.Inputs = {P("condition", type::Bool, Value(false))};
        t.Outputs = {P("result", type::Bool, Value(true))};
        t.Run = [](GraphExec& ex) {
            ex.Out("result", Value(!ex.In("condition").AsBool()));
            return NodeResult::Go("");
        };
        t.Keywords = {"invert", "negate"};
        pure(std::move(t));
    }
}

void RegisterContext(NodeRegistry& r) {
    {
        NodeType t = AiNode("ai.get_param", "Get Parameter", "AI Context",
                            "Reads a behavior parameter: the character's memory, the shared area, this run.");
        t.Pure = true;
        t.Outputs = {P("value", type::Any), P("set", type::Bool, Value(false))};
        t.Settings = {S("scope", "choice", Value(std::string("instance")), "Scope", kAiScopes),
                      S("name", "var", Value(std::string()), "Parameter")};
        t.Run = [](GraphExec& ex) {
            Blackboard& b = NeedScope(ex);
            const std::string name = ex.SettingText("name");
            ex.Out("value", b.Get(name));
            ex.Out("set", Value(b.Has(name)));
            return NodeResult::Go("");
        };
        t.Keywords = {"memory", "blackboard", "read", "context"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.set_param", "Set Parameter", "AI Context",
                            "Writes a behavior parameter. The character's memory keeps it after the task ends.");
        t.Inputs = {X("in"), P("value", type::Any)};
        t.Outputs = {X("then")};
        t.Settings = {S("scope", "choice", Value(std::string("instance")), "Scope", kAiScopes),
                      S("name", "var", Value(std::string()), "Parameter")};
        t.Run = [](GraphExec& ex) {
            Blackboard& b = NeedScope(ex);
            std::string err;
            if (!b.Set(ex.SettingText("name"), ex.In("value"), &err)) ex.Error(err);
            return NodeResult::Go("then");
        };
        t.Keywords = {"memory", "blackboard", "write", "remember", "context"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.set_state", "Set State", "AI Context",
                            "Switches the behavior state of the character (its memory \"state\").");
        t.Inputs = {X("in"), P("state", type::String, Value(std::string()))};
        t.Outputs = {X("then")};
        t.Run = [](GraphExec& ex) {
            const std::string next = ex.In("state").AsString();
            const std::string prev = ex.Task().Instance().Get("state").AsString();
            ex.Task().Instance().Set("state", Value(next));
            if (prev != next) ex.Note("decision", "состояние: " + (prev.empty() ? "—" : prev) + " → " + next);
            return NodeResult::Go("then");
        };
        t.Keywords = {"mode", "phase", "switch", "state machine"};
        r.Register(std::move(t));
    }
    {
        // ВОСПРИЯТИЕ СОБЫТИЙ В ПАМЯТЬ. AI Graph не держит обработчиков потока
        // (On Event — узел Script Graph); услышанное поведение ЗАПОМИНАЕТ, а
        // решает по памяти: Decide перепроверяет условия на каждое событие.
        NodeType t = AiNode("ai.remember_event", "Remember Event", "AI Context",
                            "Whenever the AI event arrives, writes its data into a parameter of the character's "
                            "memory (and sets a flag), whatever the behavior is doing.");
        t.Entry = true;
        t.Outputs = {X("then"), P("data", type::Any)};
        t.Settings = {S("event", "event", Value(std::string()), "Event"),
                      S("name", "var", Value(std::string()), "Store data in"),
                      S("flag", "var", Value(std::string()), "Set flag (optional)")};
        t.ListensTo = [](const GraphNode& n) { return SettingOf(n, "event"); };
        t.EventRunsSelf = true;
        t.Run = [](GraphExec& ex) {
            const Value data = ex.State().Outputs["data"];
            Blackboard& mem = ex.Task().Instance();
            if (!ex.SettingText("name").empty()) mem.Set(ex.SettingText("name"), data);
            if (!ex.SettingText("flag").empty()) mem.Set(ex.SettingText("flag"), Value(true));
            ex.Note("event", "запомнено событие '" + ex.SettingText("event") + "'");
            return NodeResult::Go("then");
        };
        t.Keywords = {"hear", "perceive", "listen", "memory", "event"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.last_result", "Last Result", "AI Context",
                            "The result of the previous behavior task of this run: whether it succeeded, which "
                            "task, and why it failed.");
        t.Pure = true;
        t.Outputs = {P("ok", type::Bool, Value(true)), P("task", type::String), P("reason", type::String)};
        t.Run = [](GraphExec& ex) {
            bool ok = true;
            std::string what, reason;
            ex.LastResult(ok, what, reason);
            ex.Out("ok", Value(ok));
            ex.Out("task", Value(what));
            ex.Out("reason", Value(reason));
            return NodeResult::Go("");
        };
        t.Keywords = {"previous", "outcome", "status"};
        r.Register(std::move(t));
    }
}

void RegisterWaits(NodeRegistry& r) {
    {
        NodeType t = AiNode("ai.wait", "Wait", "Behavior Tasks", "Waits a number of seconds, then succeeds.");
        t.Inputs = {X("in"), P("seconds", type::Float, Value(1.0f))};
        t.Outputs = {X("success")};
        t.Run = [](GraphExec& ex) {
            if (!ex.Resumed()) {
                ex.Report("running");
                ex.WaitSeconds(ex.In("seconds").AsFloat());
                return NodeResult::Wait();
            }
            ex.Report("succeeded");
            return NodeResult::Go("success");
        };
        t.Keywords = {"delay", "pause", "idle"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.wait_until", "Wait Until", "Behavior Tasks",
                            "Waits until the condition becomes true (success) or the timeout runs out (failure; "
                            "0 — no timeout).");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(false)), P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("success"), X("failure")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const double now = ex.GetWorld().Time();
            if (!ex.Resumed()) {
                const float timeout = ex.In("timeout").AsFloat();
                s.Until = timeout > 0.0f ? now + timeout : 0.0;
            }
            if (ex.In("condition").AsBool()) {
                ex.Report("succeeded");
                ex.SetLastResult(true, "Wait Until", {});
                return NodeResult::Go("success");
            }
            if (s.Until > 0.0 && now >= s.Until) {
                ex.Report("failed");
                ex.SetLastResult(false, "Wait Until", "время ожидания вышло");
                ex.FailReason("Wait Until #" + Num(ex.Node().Id) + ": условие не наступило за отведённое время");
                return NodeResult::Go("failure");
            }
            ex.Report("running");
            ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"until", "condition", "await"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.wait_event", "Wait For Event", "Behavior Tasks",
                            "Waits for an AI event (success, with its data) or until the timeout (failure; "
                            "0 — no timeout).");
        t.Inputs = {X("in"), P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("success"), X("failure"), P("data", type::Any)};
        t.Settings = {S("event", "event", Value(std::string()), "Event")};
        t.Run = [](GraphExec& ex) {
            const std::string name = ex.SettingText("event");
            if (!ex.Resumed()) {
                if (name.empty()) ex.Error("не задано событие");
                ex.Report("running");
                ex.WaitEvent(name, ex.In("timeout").AsFloat());
                return NodeResult::Wait();
            }
            if (ex.TimedOut()) {
                ex.Report("failed");
                ex.SetLastResult(false, "Wait For Event", "событие '" + name + "' не пришло");
                ex.FailReason("событие '" + name + "' не пришло за отведённое время");
                return NodeResult::Go("failure");
            }
            ex.Out("data", ex.EventPayload());
            ex.Report("succeeded");
            ex.SetLastResult(true, "Wait For Event", {});
            return NodeResult::Go("success");
        };
        t.Keywords = {"listen", "signal", "await", "hear"};
        r.Register(std::move(t));
    }
}

} // namespace

void RegisterAiContextNodes(NodeRegistry& r) {
    RegisterConditions(r);
    RegisterContext(r);
    RegisterWaits(r);
}

} // namespace sage::ai::nodes
