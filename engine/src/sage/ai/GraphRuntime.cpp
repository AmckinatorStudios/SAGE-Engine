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

} // namespace

// ---------------------------------------------------------------------------
// Сборка графа для выполнения.
// ---------------------------------------------------------------------------
std::shared_ptr<CompiledGraph> CompileGraph(std::shared_ptr<const GraphDoc> doc, const World& world) {
    auto g = std::make_shared<CompiledGraph>();
    g->Doc = std::move(doc);
    const GraphDoc& d = *g->Doc;
    const GraphEnv env{&world, &world.Types(), &d};
    std::string unknown;
    for (const GraphNode& n : d.Nodes) {
        CompiledNode c;
        c.Node = &n;
        c.Type = world.Nodes().Find(n.Type);
        if (c.Type) c.Pins = ResolvePins(*c.Type, n, env);
        else if (unknown.empty()) unknown = "'" + n.Type + "' (#" + std::to_string(n.Id) + ")";
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
    for (size_t i = 0; i < g->Nodes.size(); ++i) {
        const std::string& t = g->Nodes[i].Node->Type;
        if (t == "flow.start" && !g->StartNode) g->StartNode = g->Nodes[i].Node->Id;
        if (t == "event.on") g->EventNodes.push_back(i);
    }
    // Узла нет в реестре — чаще всего не загружена библиотека, где он
    // объявлен (свой узел на Lua). Граф с дыркой не запускается вовсе: молча
    // пропустить узел значило бы выполнить другую логику, чем нарисовано.
    if (!unknown.empty()) g->Problem = "в графе '" + d.Name + "' неизвестный узел " + unknown;
    else if (!g->StartNode && g->EventNodes.empty())
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

void GraphExec::Kill(int fiber) { m_g.KillFiber(fiber); }

bool GraphExec::FiberDone(int fiber) const {
    Fiber* f = m_g.FindFiber(fiber);
    return !f || f->Done;
}

RunPtr GraphExec::StartTask(const std::string& name, const Blackboard& inputs) {
    RunPtr run = m_t.Start(name, inputs);
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

void GraphBehavior::KillFiber(int id) {
    Fiber* f = FindFiber(id);
    if (!f || f->Done) return;
    f->Done = true;
    // Ветка, ждавшая задачу, уносит её с собой: «Parallel: любая» отменил
    // проигравшую ветку — её перемещение не должно идти дальше в пустоту.
    if (f->Wait == Fiber::WaitKind::Run && f->Run && f->Run->Status == TaskStatus::Running) {
        // Отмену делает мир: он знает про детей и про очистку.
        // Не сразу: убивают ветку изнутри шага другого узла, и отмена с
        // вызовом чужого cancel посреди него — лишняя вложенность.
        m_cancelQueue.push_back(f->Run);
        f->Run.reset();
    }
    const std::vector<int> waiting = f->Waiting;
    for (int w : waiting) KillFiber(w);
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

void GraphBehavior::Fail(const std::string& reason) {
    m_finished = true;
    m_ok = false;
    m_reason = reason;
}

void GraphBehavior::DeliverEvents(TaskContext& ctx) {
    while (!m_events.empty()) {
        auto [name, payload] = std::move(m_events.front());
        m_events.pop_front();
        for (auto& f : m_fibers) {
            if (f->Done || f->Wait != Fiber::WaitKind::Event || f->Event != name) continue;
            f->Wait = Fiber::WaitKind::None;
            f->TimedOut = false;
            f->Payload = payload;
        }
        for (size_t i : m_g->EventNodes) {
            const CompiledNode& n = m_g->Nodes[i];
            auto it = n.Node->Values.find("event");
            if (it == n.Node->Values.end() || it->second.AsString() != name) continue;
            m_state[n.Node->Id].Outputs["data"] = payload;
            if (const int target = Target(n, "fired")) NewFiber(target);
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
            case Flow::Next: f.Node = Target(*n, r.Pin); break;
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
        if (m_g->StartNode) NewFiber(m_g->StartNode);
        // События слышны всегда, а не только когда ветка ждёт: обработчик On
        // Event — это «когда бы ни случилось», а не «если сейчас стою и жду».
        run.Listener = [this](const std::string& name, const vars::Value& payload) {
            bool want = false;
            for (auto& f : m_fibers)
                if (!f->Done && f->Wait == Fiber::WaitKind::Event && f->Event == name) want = true;
            for (size_t i : m_g->EventNodes) {
                auto it = m_g->Nodes[i].Node->Values.find("event");
                if (it != m_g->Nodes[i].Node->Values.end() && it->second.AsString() == name) want = true;
            }
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
    m_budget = kMaxNodesPerStep;
    for (size_t i = 0; i < m_fibers.size() && !m_finished && !m_paused; ++i) {
        Fiber& f = *m_fibers[i];
        if (f.Done || !Ready(f)) continue;
        RunFiber(ctx, f);
        if (m_budget < 0) {
            Fail("граф '" + GraphName() + "' выполнил больше " + std::to_string(kMaxNodesPerStep) +
                 " узлов за один кадр без ожидания — похоже на бесконечный цикл");
            break;
        }
    }
    while (!m_cancelQueue.empty()) {
        RunPtr r = m_cancelQueue.front();
        m_cancelQueue.pop_front();
        ctx.GetWorld().Cancel(r->Id);
    }
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
    if (m_fibers.empty() && m_g->EventNodes.empty() && !m_paused) return TaskStatus::Succeeded;
    ctx.WaitUntil([this] { return AnyReady(); });
    return TaskStatus::Running;
}

void GraphBehavior::Cancel(TaskContext&) {
    // Дочерние задачи отменяет мир (они дети этого выполнения); здесь
    // остаётся только забыть ветки.
    m_fibers.clear();
    m_cancelQueue.clear();
}

} // namespace sage::ai
