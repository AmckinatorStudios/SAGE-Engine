#include "sage/ai/GraphRuntime.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/core/Log.h"

namespace sage::ai {

namespace {

bool IsLoop(const std::string& id) { return id == "flow.while" || id == "flow.for" || id == "flow.for_each"; }

// Выход неудачи: ушли по нему, а он ни с чем не соединён, — ветка неудачна.
bool IsFailurePin(const std::string& pin) { return pin == "failure" || pin == "failed"; }

bool IsStart(const std::string& t) { return t == "flow.start" || t == "script.on_start" || t == "ai.task_start"; }

} // namespace

// ---------------------------------------------------------------------------
// Сборка графа для выполнения.
// ---------------------------------------------------------------------------
std::shared_ptr<CompiledGraph> CompileGraph(std::shared_ptr<const GraphDoc> doc, const World& world) {
    auto g = std::make_shared<CompiledGraph>();
    g->Doc = std::move(doc);
    const GraphDoc& d = *g->Doc;
    const GraphEnv env{&world, &world.Types(), &d};
    std::string unknown, foreign;
    for (const GraphNode& n : d.Nodes) {
        CompiledNode c;
        c.Node = &n;
        c.Type = world.Nodes().Find(n.Type);
        if (c.Type) c.Pins = ResolvePins(*c.Type, n, env);
        else if (unknown.empty()) unknown = "'" + n.Type + "' (#" + std::to_string(n.Id) + ")";
        if (c.Type && !NodeAllowed(c.Type->Domains, d.Kind) && foreign.empty())
            foreign = "'" + c.Type->Title + "' (#" + std::to_string(n.Id) + ")";
        g->Index[n.Id] = g->Nodes.size();
        g->Nodes.push_back(std::move(c));
    }
    for (const GraphLink& l : d.Links) {
        auto fi = g->Index.find(l.From.Node), ti = g->Index.find(l.To.Node);
        if (fi == g->Index.end() || ti == g->Index.end()) continue;
        CompiledNode& from = g->Nodes[fi->second];
        CompiledNode& to = g->Nodes[ti->second];
        const PinSpec* out = FindPin(from.Pins.Outputs, l.From.Pin);
        const PinSpec* in = FindPin(to.Pins.Inputs, l.To.Pin);
        const bool exec = (out && out->Type == type::Exec) || (in && in->Type == type::Exec);
        if (exec) from.ExecOut[l.From.Pin] = l.To.Node;
        else to.DataIn[l.To.Pin] = &l;
    }
    int starts = 0;
    for (size_t i = 0; i < g->Nodes.size(); ++i) {
        const CompiledNode& c = g->Nodes[i];
        const std::string& t = c.Node->Type;
        if (IsStart(t)) {
            ++starts;
            if (!g->StartNode) g->StartNode = c.Node->Id;
        }
        if (c.Type && c.Type->ListensTo) {
            g->EventNodes.push_back(i);
            g->EventNames.push_back(c.Type->ListensTo(*c.Node));
        }
        if (t == "script.on_update") g->UpdateNodes.push_back(i);
        if (t == "ai.decide") g->ListensAll = true;
    }
    // Узла нет в реестре — чаще всего не загружена библиотека, где он
    // объявлен (свой узел на Lua). Граф с дыркой не запускается вовсе: молча
    // пропустить узел значило бы выполнить другую логику, чем нарисовано.
    if (!unknown.empty()) g->Problem = "в графе '" + d.Name + "' неизвестный узел " + unknown;
    // Узел чужого редактора (задача ИИ в Visual Scripting) — тоже не запуск:
    // правила назначения одни для палитры, проверки и выполнения.
    else if (!foreign.empty())
        g->Problem = "в графе '" + d.Name + "' узел " + foreign + " не для этого вида графа";
    // AI Graph — задача поведения: у неё ровно одно начало, Task Start.
    else if (d.Kind == GraphKind::Behavior && starts != 1)
        g->Problem = starts == 0 ? "в AI Graph '" + d.Name + "' нет узла Task Start — поведению неоткуда начаться"
                                 : "в AI Graph '" + d.Name + "' несколько узлов Task Start — начало должно быть одно";
    else if (!g->StartNode && g->EventNodes.empty() && g->UpdateNodes.empty())
        g->Problem = "в графе '" + d.Name + "' нет ни узла Start, ни обработчика событий";
    return g;
}

// ---------------------------------------------------------------------------
// Описание задачи-графа.
// ---------------------------------------------------------------------------
GraphTaskDefinition::GraphTaskDefinition(std::shared_ptr<const GraphDoc> doc, World& world)
    : m_doc(std::move(doc)), m_world(world) {
    Name = m_doc->Name;
    Origin = "Graph";
    for (const GraphVar& v : m_doc->Inputs)
        Signature.Inputs.push_back({v.Name, world.Types().Convert(v.Default, v.Type)});
    for (const GraphVar& v : m_doc->Outputs)
        Signature.Outputs.push_back({v.Name, world.Types().Convert(v.Default, v.Type)});
    Signature.Channels = m_doc->Channels;
    Signature.Interruptible = m_doc->Interruptible;
}

std::unique_ptr<TaskBehavior> GraphTaskDefinition::Create() { return std::make_unique<GraphBehavior>(*this); }

std::shared_ptr<CompiledGraph> GraphTaskDefinition::Compiled() {
    const uint64_t v = m_world.LibraryVersion() * 1000003ull + m_world.Nodes().Version() * 131ull +
                       m_world.Types().Version();
    if (!m_compiled || v != m_version) {
        m_compiled = CompileGraph(m_doc, m_world);
        m_version = v;
    }
    return m_compiled;
}

// ---------------------------------------------------------------------------
// Контекст узла.
// ---------------------------------------------------------------------------
vars::Value GraphExec::In(const std::string& pin) { return m_g.InputOf(m_t, m_n, pin, m_depth + 1); }

bool GraphExec::Has(const std::string& pin) const {
    return m_n.DataIn.count(pin) != 0 || m_n.Node->Values.count(pin) != 0;
}

void GraphExec::Out(const std::string& pin, const vars::Value& value) {
    State().Outputs[pin] = value;
    GraphDebugger& dbg = GraphDebugger::Get();
    if (dbg.Enabled) dbg.PinValue(m_g.m_runId, m_n.Node->Id, pin, value.AsString());
}

vars::Value GraphExec::Setting(const std::string& name) const {
    auto it = m_n.Node->Values.find(name);
    if (it != m_n.Node->Values.end()) return it->second;
    if (m_n.Type)
        for (const SettingSpec& s : m_n.Type->Settings)
            if (s.Name == name) return s.Default;
    return vars::Value();
}

NodeState& GraphExec::State() { return m_g.m_state[m_n.Node->Id]; }
const TypeRegistry& GraphExec::Types() const { return m_t.GetWorld().Types(); }
const GraphDoc& GraphExec::Doc() const { return *m_g.m_g->Doc; }

Fiber& GraphExec::Need(const char* what) {
    if (!m_f) throw std::runtime_error(std::string("чистый узел не может ") + what);
    return *m_f;
}

void GraphExec::WaitFrame() {
    Fiber& f = Need("ждать");
    f.Wait = Fiber::WaitKind::Frame;
    f.FrameMark = GetWorld().Frame();
}

void GraphExec::WaitSeconds(float seconds) {
    Fiber& f = Need("ждать");
    f.Wait = Fiber::WaitKind::Time;
    f.Until = GetWorld().Time() + std::max(0.0f, seconds);
    f.FrameMark = GetWorld().Frame();
}

void GraphExec::WaitRun(RunPtr run) {
    Fiber& f = Need("ждать задачу");
    f.Wait = Fiber::WaitKind::Run;
    f.Run = std::move(run);
}

void GraphExec::WaitEvent(const std::string& name, float timeout) {
    Fiber& f = Need("ждать событие");
    f.Wait = Fiber::WaitKind::Event;
    f.Event = name;
    f.Until = timeout > 0.0f ? GetWorld().Time() + timeout : 0.0;
    f.TimedOut = false;
    f.Payload = vars::Value();
}

void GraphExec::WaitFibers(std::vector<int> fibers, bool all) {
    Fiber& f = Need("ждать ветки");
    f.Wait = Fiber::WaitKind::Fibers;
    f.Waiting = std::move(fibers);
    f.All = all;
}

void GraphExec::WaitUntil(std::function<bool()> condition) {
    Fiber& f = Need("ждать условие");
    f.Wait = Fiber::WaitKind::Condition;
    f.Condition = std::move(condition);
}

int GraphExec::Spawn(const std::string& pin) {
    const int target = m_g.Target(m_n, pin);
    if (!target) return 0;
    return m_g.NewFiber(target).Id;
}

void GraphExec::Kill(int fiber, const std::string& reason) { m_g.KillFiber(fiber, reason); }

uint64_t GraphExec::EventSerial() const { return m_g.m_eventSerial; }

void GraphExec::WaitDecision(double until, int fiber, bool onEvents) {
    GraphBehavior* g = &m_g;
    const uint64_t mark = m_g.m_eventSerial;
    WaitUntil([g, until, fiber, onEvents, mark]() {
        if (g->m_world && g->m_world->Time() >= until) return true;
        if (fiber) {
            Fiber* f = g->FindFiber(fiber);
            if (!f || f->Done) return true;
        }
        return onEvents && g->m_eventSerial != mark;
    });
}

void GraphExec::Note(const std::string& kind, const std::string& text) {
    GraphDebugger& dbg = GraphDebugger::Get();
    if (dbg.Enabled) dbg.Note(m_g.m_runId, m_g.GraphName(), m_n.Node->Id, kind, text, GetWorld().Time());
}

bool GraphExec::FiberDone(int fiber) const {
    Fiber* f = m_g.FindFiber(fiber);
    return !f || f->Done;
}

bool GraphExec::FiberResult(int fiber, bool& ok, std::string& reason) {
    if (Fiber* f = m_g.FindFiber(fiber)) {
        if (!f->Done) return false;
        ok = !f->Failed;
        reason = f->Reason;
        f->Failed = false;   // прочитан — при уборке не запоминается
        f->Reason.clear();
        return true;
    }
    auto it = m_g.m_results.find(fiber);
    if (it == m_g.m_results.end()) {
        ok = true;   // ветки не было (выход ни с чем не соединён) — пустая ветка успешна
        reason.clear();
        return true;
    }
    ok = it->second.first;
    reason = it->second.second;
    m_g.m_results.erase(it);
    return true;
}

NodeResult GraphExec::EndBranch(bool ok, const std::string& reason) {
    Fiber& f = Need("закончить ветку");
    f.Failed = !ok;
    f.Reason = ok ? std::string() : reason;
    return NodeResult::End();
}

void GraphExec::FailReason(const std::string& reason) {
    if (m_f) m_f->Reason = reason;
}

void GraphExec::SetLastResult(bool ok, const std::string& what, const std::string& reason) {
    m_g.m_lastOk = ok;
    m_g.m_lastWhat = what;
    m_g.m_lastReason = reason;
}

void GraphExec::LastResult(bool& ok, std::string& what, std::string& reason) const {
    ok = m_g.m_lastOk;
    what = m_g.m_lastWhat;
    reason = m_g.m_lastReason;
}

void GraphExec::Abort(const std::string& reason) {
    m_t.GetRun().Error = reason.empty() ? "поведение прервано графом" : reason;
    m_g.m_finished = true;
    m_g.m_ok = false;
    m_g.m_reason = m_t.GetRun().Error;
    // Отмену делает мир (дети, cancel-обработчики); из шага — отложенно.
    m_t.GetWorld().Cancel(m_t.GetRun().Id);
}

void GraphExec::Report(const std::string& state) {
    GraphDebugger& dbg = GraphDebugger::Get();
    if (dbg.Enabled) dbg.NodeOutcome(m_g.m_runId, m_n.Node->Id, state);
}

RunPtr GraphExec::StartTask(const std::string& name, const Blackboard& inputs, int priority) {
    RunPtr run = m_t.Start(name, inputs, priority);
    if (run->Rejected) Error(run->Error);
    m_g.m_tasks[run->Id] = run;
    return run;
}

RunPtr GraphExec::TaskById(int id) const {
    auto it = m_g.m_tasks.find(id);
    if (it != m_g.m_tasks.end()) return it->second;
    return m_t.GetWorld().FindRun(id);
}

Blackboard* GraphExec::Scope(const std::string& scope) {
    if (scope == "local") return &m_t.Locals();
    if (scope == "instance") return &m_t.Instance();
    if (scope == "shared") return &m_t.Shared();
    if (scope == "input") return &m_t.Inputs();
    if (scope == "output") return &m_t.Outputs();
    return nullptr;
}

void GraphExec::Finish(bool ok, const std::string& reason) {
    m_g.m_finished = true;
    m_g.m_ok = ok;
    m_g.m_reason = reason;
}

void GraphExec::Log(const std::string& text) {
    LOG_INFO("AI") << m_g.GraphName() << ": " << text;
    GraphDebugger& dbg = GraphDebugger::Get();
    if (dbg.Enabled) dbg.Log(m_g.m_runId, m_g.GraphName(), m_n.Node->Id, "log", text, GetWorld().Time());
}

void GraphExec::Error(const std::string& message) const { throw std::runtime_error(message); }

// ---------------------------------------------------------------------------
// Выполнение.
// ---------------------------------------------------------------------------
Fiber& GraphBehavior::NewFiber(int node) {
    auto f = std::make_unique<Fiber>();
    f->Id = m_nextFiber++;
    f->Node = node;
    m_fibers.push_back(std::move(f));
    return *m_fibers.back();
}

Fiber* GraphBehavior::FindFiber(int id) {
    for (auto& f : m_fibers)
        if (f->Id == id) return f.get();
    return nullptr;
}

void GraphBehavior::KillFiber(int id, const std::string& reason) {
    Fiber* f = FindFiber(id);
    if (!f || f->Done) return;
    f->Done = true;
    f->Failed = true;
    f->Reason = reason.empty() ? "ветка снята" : reason;
    // Ветка, ждавшая задачу, уносит её с собой: «Parallel: любая» отменил
    // проигравшую ветку — её перемещение не должно идти дальше в пустоту.
    if (f->Wait == Fiber::WaitKind::Run && f->Run && f->Run->Status == TaskStatus::Running) {
        // Отмену делает мир: он знает про детей и про очистку. СРАЗУ, а не в
        // конце шага: решение, снявшее ветку, тут же запускает новую задачу,
        // и та не должна застать старую на тех же каналах — иначе причиной
        // отмены в отладчике стал бы «спор за канал», а не само решение.
        f->Run->Error = reason.empty() ? "ветка графа '" + GraphName() + "' снята" : reason;
        if (m_world) m_world->Cancel(f->Run->Id);
        else m_cancelQueue.push_back(f->Run);
        f->Run.reset();
    }
    const std::vector<int> waiting = f->Waiting;
    for (int w : waiting) KillFiber(w, reason);
}

int GraphBehavior::Target(const CompiledNode& n, const std::string& pin) const {
    if (pin.empty()) return 0;
    auto it = n.ExecOut.find(pin);
    return it == n.ExecOut.end() ? 0 : it->second;
}

bool GraphBehavior::Ready(Fiber& f) {
    const World& w = *m_world;
    switch (f.Wait) {
        case Fiber::WaitKind::None: return true;
        case Fiber::WaitKind::Frame: return w.Frame() > f.FrameMark;
        case Fiber::WaitKind::Time: return w.Frame() > f.FrameMark && w.Time() >= f.Until;
        case Fiber::WaitKind::Run: return !f.Run || IsFinished(f.Run->Status);
        case Fiber::WaitKind::Event:
            if (f.Until > 0.0 && w.Time() >= f.Until) {
                f.TimedOut = true;
                return true;
            }
            return false;
        case Fiber::WaitKind::Fibers: {
            size_t done = 0;
            for (int id : f.Waiting) {
                Fiber* x = FindFiber(id);
                done += (!x || x->Done) ? 1 : 0;
            }
            return f.All ? done == f.Waiting.size() : (done > 0 || f.Waiting.empty());
        }
        case Fiber::WaitKind::Condition:
            try {
                return !f.Condition || f.Condition();
            } catch (const std::exception&) {
                return true;   // узел сам повторит условие и получит ошибку
            }
    }
    return true;
}

bool GraphBehavior::AnyReady() {
    if (m_paused) return !GraphDebugger::Get().IsPaused(m_runId);
    if (!m_events.empty() || !m_cancelQueue.empty()) return true;
    // On Update — каждый новый кадр.
    if (m_g && !m_g->UpdateNodes.empty() && m_world && m_world->Frame() != m_updateFrame) return true;
    for (auto& f : m_fibers)
        if (!f->Done && Ready(*f)) return true;
    return false;
}

vars::Value GraphBehavior::InputOf(TaskContext& ctx, const CompiledNode& n, const std::string& pin, int depth) {
    const TypeRegistry& types = ctx.GetWorld().Types();
    const PinSpec* spec = FindPin(n.Pins.Inputs, pin);
    const std::string type = spec ? spec->Type : std::string(type::Any);
    auto link = n.DataIn.find(pin);
    if (link == n.DataIn.end()) {
        auto v = n.Node->Values.find(pin);
        if (v != n.Node->Values.end()) return types.Convert(v->second, type);
        return spec ? types.Convert(spec->Default, type) : vars::Value();
    }
    const GraphLink& l = *link->second;
    const CompiledNode* src = m_g->Find(l.From.Node);
    if (!src || !src->Type) return spec ? spec->Default : vars::Value();
    vars::Value v;
    if (src->Pins.Pure) {
        v = Eval(ctx, *src, l.From.Pin, depth);
    } else {
        NodeState& st = m_state[src->Node->Id];
        auto o = st.Outputs.find(l.From.Pin);
        if (o != st.Outputs.end()) v = o->second;
        else if (const PinSpec* op = FindPin(src->Pins.Outputs, l.From.Pin)) v = types.Convert(op->Default, op->Type);
    }
    return types.Convert(v, type);
}

vars::Value GraphBehavior::Eval(TaskContext& ctx, const CompiledNode& n, const std::string& pin, int depth) {
    // Чистые узлы, замкнутые сами на себя, — ошибка рисунка, а не повод для
    // переполнения стека.
    if (depth > 64) throw std::runtime_error("чистые узлы замкнуты в кольцо у узла #" + std::to_string(n.Node->Id));
    if (n.Type->Run) {
        GraphExec ex(*this, ctx, n, nullptr, false, false, depth);
        n.Type->Run(ex);
    }
    NodeState& st = m_state[n.Node->Id];
    auto o = st.Outputs.find(pin);
    if (o != st.Outputs.end()) return o->second;
    const PinSpec* op = FindPin(n.Pins.Outputs, pin);
    return op ? op->Default : vars::Value();
}

void GraphBehavior::Remember(const Fiber& f) {
    // Результат ждёт узел, запустивший ветку; бесхозные (ветки On Event) не
    // копятся без конца.
    if (m_results.size() > 256) m_results.erase(m_results.begin());
    m_results[f.Id] = {!f.Failed, f.Reason};
}

void GraphBehavior::Fail(const std::string& reason) {
    m_finished = true;
    m_ok = false;
    m_reason = reason;
}

void GraphBehavior::DeliverEvents(TaskContext& ctx) {
    while (!m_events.empty()) {
        // Обработчик без наложения, чья ветка прошлого срабатывания ещё не
        // начала шагать: событие ждёт следующего прохода этого же кадра —
        // три шага физики дают три срабатывания On Fixed Update, а не одно.
        {
            const std::string& next = m_events.front().first;
            for (size_t k = 0; k < m_g->EventNodes.size(); ++k) {
                if (m_g->EventNames[k] != next) continue;
                const CompiledNode& n = m_g->Nodes[m_g->EventNodes[k]];
                if (!n.Type->NoOverlap) continue;
                auto prev = m_updateFiber.find(n.Node->Id);
                if (prev == m_updateFiber.end()) continue;
                if (Fiber* pf = FindFiber(prev->second); pf && !pf->Done && !pf->Ran) return;
            }
        }
        auto [name, payload] = std::move(m_events.front());
        m_events.pop_front();
        ++m_eventSerial;
        for (auto& f : m_fibers) {
            if (f->Done || f->Wait != Fiber::WaitKind::Event || f->Event != name) continue;
            f->Wait = Fiber::WaitKind::None;
            f->TimedOut = false;
            f->Payload = payload;
        }
        for (size_t k = 0; k < m_g->EventNodes.size(); ++k) {
            if (m_g->EventNames[k] != name) continue;
            const CompiledNode& n = m_g->Nodes[m_g->EventNodes[k]];
            const int id = n.Node->Id;
            // Обработчик без наложения (On Fixed Update): прошлое срабатывание
            // ещё ждёт — новое не заводится, как и у On Update.
            if (n.Type->NoOverlap) {
                auto prev = m_updateFiber.find(id);
                if (prev != m_updateFiber.end())
                    if (Fiber* pf = FindFiber(prev->second); pf && !pf->Done) continue;
            }
            m_state[id].Outputs[n.Type->EventData] = payload;
            if (n.Type->EventRunsSelf) {
                NewFiber(id);
            } else if (const int target = Target(n, n.Type->EventExec)) {
                const int fid = NewFiber(target).Id;
                if (n.Type->NoOverlap) m_updateFiber[id] = fid;
            }
            GraphDebugger& dbg = GraphDebugger::Get();
            if (dbg.Enabled) {
                dbg.NodeStepped(m_runId, GraphName(), n.Node->Id, ctx.GetWorld().Frame(), ctx.GetWorld().Time());
                dbg.Log(m_runId, GraphName(), n.Node->Id, "event", "событие " + name + ": " + payload.AsString(),
                        ctx.GetWorld().Time());
            }
        }
    }
}

void GraphBehavior::RunFiber(TaskContext& ctx, Fiber& f) {
    GraphDebugger& dbg = GraphDebugger::Get();
    while (!f.Done && !m_finished && !m_paused) {
        if (f.Node == 0) {
            if (f.Stack.empty()) {
                f.Done = true;
                break;
            }
            f.Node = f.Stack.back();
            f.Stack.pop_back();
            f.Reentered = true;
            f.Resume = false;
        }
        const CompiledNode* n = m_g->Find(f.Node);
        if (!n || !n->Type) {
            Fail("в графе '" + GraphName() + "' нет узла #" + std::to_string(f.Node));
            break;
        }
        if (dbg.Enabled && !f.SkipBreak) {
            const bool fresh = !f.Resume && !f.Reentered;
            if (m_stepPause || (fresh && dbg.HasBreakpoint(GraphName(), f.Node))) {
                m_stepPause = false;
                f.SkipBreak = true;
                m_paused = true;
                dbg.Paused(m_runId, GraphName(), f.Node, ctx.GetWorld().Time());
                std::set<int> cur;
                for (auto& x : m_fibers)
                    if (!x->Done && x->Node) cur.insert(x->Node);
                dbg.SetCurrent(m_runId, std::move(cur));
                return;
            }
        }
        f.SkipBreak = false;
        if (--m_budget < 0) return;
        f.Ran = true;

        const bool resumed = f.Resume, reentered = f.Reentered;
        f.Resume = false;
        f.Reentered = false;
        f.Wait = Fiber::WaitKind::None;
        f.Condition = nullptr;
        GraphExec ex(*this, ctx, *n, &f, resumed, reentered);
        NodeResult r;
        try {
            r = n->Type->Run ? n->Type->Run(ex) : NodeResult::Go("then");
        } catch (const std::exception& e) {
            Fail(n->Type->Title + " (#" + std::to_string(n->Node->Id) + "): " + e.what());
            if (dbg.Enabled)
                dbg.Log(m_runId, GraphName(), n->Node->Id, "error", m_reason, ctx.GetWorld().Time());
            break;
        }
        if (dbg.Enabled) {
            dbg.NodeStepped(m_runId, GraphName(), n->Node->Id, ctx.GetWorld().Frame(), ctx.GetWorld().Time());
            if (RunDebug* rd = dbg.FindRun(m_runId); rd && rd->StepRequested) {
                rd->StepRequested = false;
                m_stepPause = true;
            }
        }
        if (r.Kind != Flow::Wait) {
            f.TimedOut = false;
            f.Payload = vars::Value();
        }
        switch (r.Kind) {
            case Flow::Next:
                f.Node = Target(*n, r.Pin);
                // Ушли по выходу неудачи в никуда — ветка неудачна, а не
                // «просто кончилась»: Sequence задач должен об этом узнать.
                if (!f.Node && IsFailurePin(r.Pin) && f.Stack.empty()) {
                    f.Failed = true;
                    if (f.Reason.empty())
                        f.Reason = n->Type->Title + " (#" + std::to_string(n->Node->Id) + "): неудача";
                    f.Done = true;
                    return;
                }
                if (f.Node) f.Reason.clear();
                break;
            case Flow::Push:
                f.Stack.push_back(n->Node->Id);
                f.Node = Target(*n, r.Pin);
                break;
            case Flow::Wait:
                f.Resume = true;
                if (f.Wait == Fiber::WaitKind::None) ex.WaitFrame();
                return;
            case Flow::End: f.Done = true; return;
            case Flow::Return: m_finished = true; return;
            case Flow::Break: {
                f.Node = 0;
                while (!f.Stack.empty()) {
                    const int id = f.Stack.back();
                    f.Stack.pop_back();
                    const CompiledNode* s = m_g->Find(id);
                    if (s && s->Type && IsLoop(s->Type->Id)) {
                        m_state[id].Broken = true;
                        f.Node = id;
                        f.Reentered = true;
                        break;
                    }
                }
                break;
            }
        }
    }
}

TaskStatus GraphBehavior::Step(TaskContext& ctx) {
    Run& run = ctx.GetRun();
    m_runId = run.Id;
    m_world = &ctx.GetWorld();
    GraphDebugger& dbg = GraphDebugger::Get();
    if (!m_started) {
        m_started = true;
        m_g = m_def.Compiled();
        if (!m_g->Problem.empty()) return ctx.Fail(m_g->Problem);
        const TypeRegistry& types = ctx.GetWorld().Types();
        for (const GraphVar& v : m_g->Doc->Locals) ctx.Locals().Declare(v.Name, types.Convert(v.Default, v.Type));
        if (m_g->StartNode) m_root = NewFiber(m_g->StartNode).Id;
        // События слышны всегда, а не только когда ветка ждёт: обработчик On
        // Event — это «когда бы ни случилось», а не «если сейчас стою и жду».
        run.Listener = [this](const std::string& name, const vars::Value& payload) {
            bool want = false;
            for (auto& f : m_fibers)
                if (!f->Done && f->Wait == Fiber::WaitKind::Event && f->Event == name) want = true;
            for (const std::string& e : m_g->EventNames)
                if (e == name) want = true;
            if (m_g->ListensAll) want = true;   // Decide перепроверяет условия на любое событие
            if (want) m_events.emplace_back(name, payload);
            return want;
        };
    }

    if (m_paused) {
        if (dbg.IsPaused(m_runId)) {
            ctx.WaitUntil([this] { return AnyReady(); });
            return TaskStatus::Running;
        }
        m_paused = false;
    }

    // Отмены, заказанные убитыми ветками, — здесь, вне шага узла.
    while (!m_cancelQueue.empty()) {
        RunPtr r = m_cancelQueue.front();
        m_cancelQueue.pop_front();
        ctx.GetWorld().Cancel(r->Id);
    }

    DeliverEvents(ctx);
    StartUpdates(ctx);
    m_budget = kMaxNodesPerStep;
    // Проходы, пока кто-то сдвинулся: ветка-родитель (Sequence, Selector)
    // стоит в списке раньше дочерней и иначе узнала бы о её конце только в
    // следующем кадре — цепочка из пяти мгновенных шагов шла бы пять кадров.
    for (int pass = 0; pass < 32 && !m_finished && !m_paused && m_budget >= 0; ++pass) {
        bool moved = false;
        if (pass > 0 && !m_events.empty()) {
            const size_t before = m_events.size();
            DeliverEvents(ctx);
            moved = m_events.size() != before;
        }
        for (size_t i = 0; i < m_fibers.size() && !m_finished && !m_paused; ++i) {
            Fiber& f = *m_fibers[i];
            if (f.Done || !Ready(f)) continue;
            moved = true;
            RunFiber(ctx, f);
            if (m_budget < 0) {
                Fail("граф '" + GraphName() + "' выполнил больше " + std::to_string(kMaxNodesPerStep) +
                     " узлов за один кадр без ожидания — похоже на бесконечный цикл");
                break;
            }
        }
        if (!moved) break;
    }
    while (!m_cancelQueue.empty()) {
        RunPtr r = m_cancelQueue.front();
        m_cancelQueue.pop_front();
        ctx.GetWorld().Cancel(r->Id);
    }
    // AI Graph — задача поведения: она кончается результатом корневой ветки.
    // Остальные ветки (их держали узлы корневой) снимаются вместе с задачами.
    if (!m_finished && m_g->Doc->Kind == GraphKind::Behavior && m_root) {
        Fiber* root = FindFiber(m_root);
        if (!root || root->Done) {
            m_finished = true;
            m_ok = root ? !root->Failed : true;
            m_reason = root ? root->Reason : std::string();
            for (auto& f : m_fibers)
                if (!f->Done) KillFiber(f->Id, "поведение '" + GraphName() + "' завершено");
        }
    }
    for (auto& f : m_fibers)
        if (f->Done) Remember(*f);
    m_fibers.erase(std::remove_if(m_fibers.begin(), m_fibers.end(), [](const auto& f) { return f->Done; }),
                   m_fibers.end());

    if (m_finished) {
        if (!m_ok) return ctx.Fail(m_reason.empty() ? "граф завершён неудачей" : m_reason);
        return TaskStatus::Succeeded;
    }
    if (dbg.Enabled && !m_paused) {
        std::set<int> cur;
        for (auto& f : m_fibers)
            if (f->Node) cur.insert(f->Node);
        dbg.SetCurrent(m_runId, std::move(cur));
    }
    if (m_fibers.empty() && m_g->EventNodes.empty() && m_g->UpdateNodes.empty() && !m_paused)
        return TaskStatus::Succeeded;
    ctx.WaitUntil([this] { return AnyReady(); });
    return TaskStatus::Running;
}

void GraphBehavior::StartUpdates(TaskContext& ctx) {
    if (m_g->UpdateNodes.empty()) return;
    const World& w = ctx.GetWorld();
    if (m_updateFrame == w.Frame()) return;   // кадр уже заведён
    m_updateFrame = w.Frame();
    const float dt = m_updateTime < 0.0 ? 0.0f : (float)(w.Time() - m_updateTime);
    m_updateTime = w.Time();
    for (size_t i : m_g->UpdateNodes) {
        const CompiledNode& n = m_g->Nodes[i];
        const int id = n.Node->Id;
        auto prev = m_updateFiber.find(id);
        if (prev != m_updateFiber.end()) {
            Fiber* f = FindFiber(prev->second);
            if (f && !f->Done) continue;   // прошлый кадр ещё ждёт — не догоняем сами себя
        }
        m_state[id].Outputs["dt"] = vars::Value(dt);
        if (const int target = Target(n, "then")) m_updateFiber[id] = NewFiber(target).Id;
        GraphDebugger& dbg = GraphDebugger::Get();
        if (dbg.Enabled) dbg.NodeStepped(m_runId, GraphName(), id, w.Frame(), w.Time());
    }
}

void GraphBehavior::Cancel(TaskContext&) {
    // Дочерние задачи отменяет мир (они дети этого выполнения); здесь
    // остаётся только забыть ветки.
    m_fibers.clear();
    m_cancelQueue.clear();
}

} // namespace sage::ai
