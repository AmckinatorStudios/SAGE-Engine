#include <algorithm>
#include <string>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// AI GRAPH: ЖИЗНЕННЫЙ ЦИКЛ ЗАДАЧИ И УПРАВЛЕНИЕ ПОВЕДЕНИЕМ.
//
// AI Graph — это задача мира ИИ. Её жизненный цикл записан узлами, а не
// подразумевается:
//
//   Task Start ─▶ … ─▶ Succeed / Fail          — начало и итог задачи;
//                       Abort Behavior          — отмена всего поведения.
//
// Между ними — узлы управления поведением. Каждый запускает свои ветки
// ДОЧЕРНИМИ ветками выполнения и решает по их РЕЗУЛЬТАТУ (см. «Результат
// ветки» в GraphRuntime.h), а не по тому, что ветка «просто дошла до конца»:
//
//   Task Sequence — шаги по порядку, первый неудачный — неудача всей цепочки;
//   Selector      — варианты по порядку, первый удачный — успех;
//   If Condition  — ветка только при истинном условии; пока она идёт,
//                   условие перепроверяется, и ложное снимает ветку
//                   (прерывание с причиной, задача ветки отменяется);
//   Repeat        — ветка снова и снова: N раз, до неудачи или до успеха;
//   Decide        — приоритеты и переключение (GraphNodes_Decide.cpp).
//
// Долгая задача внутри ветки (Move To Target, Run Task) запускается ОДИН раз и
// ждёт своего конца; повторяет её только Repeat, по своему правилу.
//
// Здесь нет ни одного узла Script Graph: в AI Graph нет Branch, Sequence
// потока, циклов и игровых действий — у поведения свои узлы с результатом.
// ---------------------------------------------------------------------------
namespace sage::ai::nodes {

using vars::Value;

namespace {

std::string Num(int i) { return std::to_string(i); }

// Узел ИИ: только AI Graph, итог — в отладчик.
NodeType AiNode(const std::string& id, const std::string& title, const std::string& category,
                const std::string& description) {
    NodeType t = Node(id, title, category, description);
    t.Domains = kDomainAI;
    return t;
}

// Ждать ветку-ребёнка: её конец будит узел.
NodeResult WaitChild(GraphExec& ex, int fiber) {
    ex.WaitFibers({fiber}, true);
    return NodeResult::Wait();
}

void RegisterTaskLifecycle(NodeRegistry& r) {
    {
        NodeType t = AiNode("ai.task_start", "Task Start", "Task",
                            "Where the behavior begins when the AI starts it. Its branch decides the result "
                            "of the whole behavior.");
        t.Entry = true;
        t.Outputs = {X("then")};
        t.Run = [](GraphExec&) { return NodeResult::Go("then"); };
        t.Keywords = {"begin", "entry", "root"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.succeed", "Succeed", "Task",
                            "Ends this branch successfully. On the main branch the behavior succeeds.");
        t.Inputs = {X("in")};
        t.Run = [](GraphExec& ex) {
            ex.Report("succeeded");
            return ex.EndBranch(true);
        };
        t.Keywords = {"success", "done", "finish", "return"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.fail", "Fail", "Task",
                            "Ends this branch with a failure and its reason. On the main branch the behavior fails.");
        t.Inputs = {X("in"), P("reason", type::String, Value(std::string()))};
        t.Run = [](GraphExec& ex) {
            std::string reason = ex.In("reason").AsString();
            if (reason.empty()) reason = "Fail (#" + Num(ex.Node().Id) + ")";
            ex.Report("failed");
            return ex.EndBranch(false, reason);
        };
        t.Keywords = {"failure", "error", "cannot"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.abort", "Abort Behavior", "Task",
                            "Cancels the whole behavior with a reason: running tasks are cancelled too.");
        t.Inputs = {X("in"), P("reason", type::String, Value(std::string()))};
        t.Run = [](GraphExec& ex) {
            ex.Abort(ex.In("reason").AsString());
            return NodeResult::End();
        };
        t.Keywords = {"cancel", "interrupt", "stop"};
        r.Register(std::move(t));
    }
}

void RegisterControl(NodeRegistry& r) {
    {
        NodeType t = AiNode("ai.sequence", "Task Sequence", "Behavior Control",
                            "Runs its steps one after another; the first failed step fails the whole sequence.");
        t.Inputs = {X("in")};
        t.Settings = {S("steps", "int", Value(3), "Steps")};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "steps", 3), 1, 16);
            for (int i = 0; i < count; ++i) p.Outputs.push_back(X("step " + Num(i)));
            p.Outputs.push_back(X("success"));
            p.Outputs.push_back(X("failure"));
        };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const int count = std::clamp(ex.SettingInt("steps"), 1, 16);
            if (!ex.Resumed()) {
                s.Counter = 0;
                s.ActiveFiber = 0;
            } else {
                bool ok = true;
                std::string reason;
                if (!ex.FiberResult(s.ActiveFiber, ok, reason)) return WaitChild(ex, s.ActiveFiber);
                if (!ok) {
                    ex.Report("failed");
                    ex.FailReason("шаг " + Num(s.Counter) + ": " + reason);
                    return NodeResult::Go("failure");
                }
                ++s.Counter;
            }
            for (; s.Counter < count; ++s.Counter) {
                const int f = ex.Spawn("step " + Num(s.Counter));
                if (!f) continue;   // пустой шаг — успешен
                s.ActiveFiber = f;
                ex.Report("running");
                return WaitChild(ex, f);
            }
            ex.Report("succeeded");
            return NodeResult::Go("success");
        };
        t.Keywords = {"sequence", "steps", "chain", "then"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.selector", "Selector", "Behavior Control",
                            "Tries its options one after another until one succeeds; fails if all fail.");
        t.Inputs = {X("in")};
        t.Settings = {S("options", "int", Value(3), "Options")};
        t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
            const int count = std::clamp(SettingIntOf(n, "options", 3), 1, 16);
            for (int i = 0; i < count; ++i) p.Outputs.push_back(X("option " + Num(i)));
            p.Outputs.push_back(X("success"));
            p.Outputs.push_back(X("failure"));
        };
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const int count = std::clamp(ex.SettingInt("options"), 1, 16);
            if (!ex.Resumed()) {
                s.Counter = 0;
                s.ActiveFiber = 0;
                s.Data = Value(std::string());
            } else {
                bool ok = true;
                std::string reason;
                if (!ex.FiberResult(s.ActiveFiber, ok, reason)) return WaitChild(ex, s.ActiveFiber);
                if (ok) {
                    ex.Report("succeeded");
                    ex.Note("decision", "Selector #" + Num(ex.Node().Id) + ": удался вариант " + Num(s.Counter));
                    return NodeResult::Go("success");
                }
                s.Data = Value("вариант " + Num(s.Counter) + ": " + reason);
                ++s.Counter;
            }
            for (; s.Counter < count; ++s.Counter) {
                const int f = ex.Spawn("option " + Num(s.Counter));
                if (!f) continue;   // пустой вариант не может «удаться»
                s.ActiveFiber = f;
                ex.Report("running");
                return WaitChild(ex, f);
            }
            ex.Report("failed");
            const std::string last = s.Data.AsString();
            ex.FailReason("ни один вариант не удался" + (last.empty() ? std::string() : " (последний — " + last + ")"));
            return NodeResult::Go("failure");
        };
        t.Keywords = {"fallback", "choose", "first", "alternative", "try"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.guard", "If Condition", "Behavior Control",
                            "Runs its branch only while the condition holds: checked before the start and, if "
                            "watched, while the branch runs — a false condition interrupts the branch.");
        t.Inputs = {X("in"), P("condition", type::Bool, Value(true))};
        t.Outputs = {X("do"), X("success"), X("failure")};
        t.Settings = {S("watch", "bool", Value(true), "Keep checking while running"),
                      S("interval", "float", Value(0.0f), "Re-check every (s)")};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const bool watch = ex.Setting("watch").AsBool();
            const double interval = std::max(0.0f, ex.Setting("interval").AsFloat());
            if (!ex.Resumed()) {
                if (!ex.In("condition").AsBool()) {
                    ex.Report("failed");
                    ex.FailReason("условие If Condition #" + Num(ex.Node().Id) + " не выполнено");
                    return NodeResult::Go("failure");
                }
                s.ActiveFiber = ex.Spawn("do");
                if (!s.ActiveFiber) {
                    ex.Report("succeeded");
                    return NodeResult::Go("success");
                }
                ex.Report("running");
            } else {
                bool ok = true;
                std::string reason;
                if (ex.FiberResult(s.ActiveFiber, ok, reason)) {
                    ex.Report(ok ? "succeeded" : "failed");
                    if (!ok) ex.FailReason(reason);
                    return NodeResult::Go(ok ? "success" : "failure");
                }
                if (watch && !ex.In("condition").AsBool()) {
                    const std::string why = "условие If Condition #" + Num(ex.Node().Id) + " перестало выполняться";
                    ex.Kill(s.ActiveFiber, why);
                    bool ignored = true;
                    std::string r2;
                    ex.FiberResult(s.ActiveFiber, ignored, r2);
                    ex.Note("decision", why);
                    ex.Report("failed");
                    ex.FailReason(why);
                    return NodeResult::Go("failure");
                }
            }
            if (!watch) return WaitChild(ex, s.ActiveFiber);
            ex.WaitDecision(ex.GetWorld().Time() + interval, s.ActiveFiber, false);
            if (interval <= 0.0) ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"guard", "while", "precondition", "abort", "interrupt"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.repeat", "Repeat", "Behavior Control",
                            "Runs its branch again and again: a number of times (0 — without end), or until the "
                            "branch fails or succeeds. The next run starts on the next frame.");
        t.Inputs = {X("in")};
        t.Outputs = {X("body"), X("success"), X("failure")};
        t.Settings = {S("count", "int", Value(0), "Times (0 — no limit)"),
                      S("stop_on", "choice", Value(std::string("failure")), "Stop when the branch",
                        {"failure", "success", "never"})};
        t.Run = [](GraphExec& ex) {
            NodeState& s = ex.State();
            const int count = std::max(0, ex.SettingInt("count"));
            const std::string stopOn = ex.SettingText("stop_on");
            if (!ex.Resumed()) {
                s.Counter = 0;
                s.Limit = 0;   // 0 — ждём ветку, 1 — ждём кадр перед новым кругом
            } else if (s.Limit == 0) {
                bool ok = true;
                std::string reason;
                if (!ex.FiberResult(s.ActiveFiber, ok, reason)) return WaitChild(ex, s.ActiveFiber);
                ++s.Counter;
                if (!ok && stopOn == "failure") {
                    ex.Report("failed");
                    ex.FailReason("круг " + Num(s.Counter) + ": " + reason);
                    return NodeResult::Go("failure");
                }
                if (ok && stopOn == "success") {
                    ex.Report("succeeded");
                    return NodeResult::Go("success");
                }
                if (count > 0 && s.Counter >= count) {
                    ex.Report("succeeded");
                    return NodeResult::Go("success");
                }
                // Новый круг — со следующего кадра: мгновенная ветка без конца
                // иначе крутилась бы в одном кадре до предела узлов.
                s.Limit = 1;
                ex.WaitFrame();
                return NodeResult::Wait();
            }
            s.Limit = 0;
            s.ActiveFiber = ex.Spawn("body");
            if (!s.ActiveFiber) {
                ex.Report("succeeded");
                return NodeResult::Go("success");
            }
            ex.Report("running");
            return WaitChild(ex, s.ActiveFiber);
        };
        t.Keywords = {"loop", "again", "retry", "until"};
        r.Register(std::move(t));
    }
}

} // namespace

void RegisterBehaviorNodes(NodeRegistry& r) {
    RegisterTaskLifecycle(r);
    RegisterControl(r);
    RegisterAiContextNodes(r);
}

} // namespace sage::ai::nodes
