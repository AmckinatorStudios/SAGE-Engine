#include <algorithm>
#include <string>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// РЕШЕНИЕ С ПРИОРИТЕТАМИ — главный узел AI Graph.
//
// AI Graph решает, КАКУЮ задачу выполнять и КОГДА сменить поведение; как
// задача выполняется — дело Visual Scripting или Lua. Decide и есть это «какую
// и когда»: у него варианты, у каждого — условие, приоритет и можно ли его
// прервать. Из вариантов с истинным условием выполняется ветка самого
// важного (обычно — Run Task нужной задачи).
//
// ПЕРЕКЛЮЧЕНИЕ. Условия перепроверяются раз в interval секунд, на любое
// событие графа (on_events) и когда ветка варианта кончилась. Если теперь
// важнее другой вариант и текущий прерываемый — ветка текущего снимается
// вместе с задачей, которую она ждёт (отмена с причиной), и запускается
// ветка нового. Непрерываемый вариант доигрывает до конца. Ветка кончилась —
// вариант отдыхает cooldown секунд, затем снова участвует в выборе: так
// задаётся повтор после неудачи без цикла «упал — сразу снова».
//
// Узел ничего не знает о поведении: приоритеты — числа на входах (могут
// вычисляться графом), условия — провода. Правила задаёт автор игры.
//
// Каждое переключение пишется в отладчик (вид «decision») с причиной:
// «почему NPC сейчас делает это» видно, а не угадывается.
// ---------------------------------------------------------------------------
namespace sage::ai::nodes {

using vars::Value;

namespace {

std::string Num(int i) { return std::to_string(i); }

} // namespace

void RegisterDecisionNodes(NodeRegistry& r) {
    NodeType t = Node("ai.decide", "Decide", "Decisions",
                      "Runs the branch of the most important option whose condition holds; switches when a more "
                      "important one becomes true and the current one may be interrupted.");
    t.Domains = kDomainAI;
    t.Inputs = {X("in")};
    t.Settings = {S("options", "int", Value(3), "Options"),
                  S("interval", "float", Value(0.25f), "Re-check every (s)"),
                  S("cooldown", "float", Value(0.0f), "Rest after an option ends (s)"),
                  S("on_events", "bool", Value(true), "Re-check on every event")};
    t.Pins = [](const GraphNode& n, const GraphEnv&, NodePins& p) {
        const int count = std::clamp(SettingIntOf(n, "options", 3), 1, 16);
        for (int i = 0; i < count; ++i) {
            p.Inputs.push_back(P("condition " + Num(i), type::Bool, Value(i == count - 1)));
            p.Inputs.push_back(P("priority " + Num(i), type::Int, Value((count - i) * 10)));
            p.Inputs.push_back(P("interruptible " + Num(i), type::Bool, Value(true)));
            p.Outputs.push_back(X("option " + Num(i)));
        }
        p.Outputs.push_back(P("active", type::Int, Value(-1)));
        p.Outputs.push_back(P("reason", type::String, Value(std::string())));
    };
    t.Run = [](GraphExec& ex) {
        NodeState& s = ex.State();
        const int count = std::clamp(ex.SettingInt("options"), 1, 16);
        const double interval = std::max(0.0f, ex.Setting("interval").AsFloat());
        const double cooldown = std::max(0.0f, ex.Setting("cooldown").AsFloat());
        const bool onEvents = ex.Setting("on_events").AsBool();
        const double now = ex.GetWorld().Time();
        if (!ex.Resumed()) {
            s.Active = -1;
            s.ActiveFiber = 0;
            s.NextEval = 0.0;
            s.EventMark = ex.EventSerial();
        }
        s.Cooldown.resize(count, 0.0);

        bool due = now >= s.NextEval;
        if (s.Active >= 0 && ex.FiberDone(s.ActiveFiber)) {
            ex.Note("decision", "вариант " + Num(s.Active) + " закончился");
            s.Cooldown[s.Active] = now + cooldown;
            s.Active = -1;
            s.ActiveFiber = 0;
            due = true;
        }
        if (onEvents && ex.EventSerial() != s.EventMark) due = true;

        if (due) {
            s.NextEval = now + interval;
            s.EventMark = ex.EventSerial();
            int best = -1, bestPrio = 0;
            for (int i = 0; i < count; ++i) {
                if (now < s.Cooldown[i]) continue;
                if (!ex.In("condition " + Num(i)).AsBool()) continue;
                const int p = ex.In("priority " + Num(i)).AsInt();
                if (best < 0 || p > bestPrio) {
                    best = i;
                    bestPrio = p;
                }
            }
            if (best != s.Active) {
                bool change = s.Active < 0;
                std::string why;
                if (!change) {
                    const int a = s.Active;
                    const bool interruptible = ex.In("interruptible " + Num(a)).AsBool();
                    const bool condA = ex.In("condition " + Num(a)).AsBool();
                    const int prioA = ex.In("priority " + Num(a)).AsInt();
                    if (!interruptible) {
                        // Непрерываемый доигрывает: смена — когда его ветка кончится.
                    } else if (!condA) {
                        change = true;
                        why = "условие варианта " + Num(a) + " больше не выполняется";
                    } else if (best >= 0 && bestPrio > prioA) {
                        change = true;
                        why = "вариант " + Num(best) + " важнее (приоритет " + Num(bestPrio) + " > " + Num(prioA) + ")";
                    }
                } else if (best >= 0) {
                    why = "условие истинно, приоритет " + Num(bestPrio);
                }
                if (change) {
                    if (s.Active >= 0) ex.Kill(s.ActiveFiber, "решение AI Graph: " + why);
                    s.Active = best;
                    s.ActiveFiber = best >= 0 ? ex.Spawn("option " + Num(best)) : 0;
                    const std::string text =
                        best >= 0 ? "выбран вариант " + Num(best) + ": " + why : "ни один вариант не подходит" +
                                                                                    (why.empty() ? "" : " (" + why + ")");
                    ex.Note("decision", text);
                    ex.Out("reason", Value(text));
                }
            }
        }
        ex.Out("active", Value(s.Active));
        // Событийно: узел спит до срока перепроверки, конца ветки или события.
        ex.WaitDecision(interval > 0.0 ? s.NextEval : now, s.ActiveFiber, onEvents);
        if (interval <= 0.0) ex.WaitFrame();
        return NodeResult::Wait();
    };
    t.Keywords = {"priority", "selector", "utility", "choose", "switch", "interrupt"};
    r.Register(std::move(t));
}

} // namespace sage::ai::nodes
