#include "sage/ai/GraphDebug.h"

namespace sage::ai {

GraphDebugger& GraphDebugger::Get() {
    static GraphDebugger d;
    return d;
}

void GraphDebugger::ToggleBreakpoint(const std::string& graph, int node) {
    const auto key = std::make_pair(graph, node);
    if (!m_breaks.erase(key)) m_breaks.insert(key);
}

bool GraphDebugger::HasBreakpoint(const std::string& graph, int node) const {
    return m_breaks.count({graph, node}) != 0;
}

void GraphDebugger::Continue(int run) {
    if (RunDebug* r = FindRun(run)) {
        r->Paused = false;
        r->StepRequested = false;
    }
}

void GraphDebugger::Step(int run) {
    if (RunDebug* r = FindRun(run)) {
        r->Paused = false;
        r->StepRequested = true;
    }
}

void GraphDebugger::ContinueAll() {
    for (auto& [_, r] : m_runs) {
        r.Paused = false;
        r.StepRequested = false;
    }
}

bool GraphDebugger::IsPaused(int run) const {
    auto it = m_runs.find(run);
    return it != m_runs.end() && it->second.Paused;
}

RunDebug* GraphDebugger::FindRun(int run) {
    auto it = m_runs.find(run);
    return it == m_runs.end() ? nullptr : &it->second;
}

void GraphDebugger::Push(DebugEntry e) {
    e.Frame = m_frame;
    m_history.push_back(std::move(e));
    while (m_history.size() > kHistory) m_history.pop_front();
}

void GraphDebugger::TaskStarted(int run, int parent, int agent, const std::string& task,
                                const std::string& origin, const std::string& graph, double time) {
    RunDebug& r = m_runs[run];
    r.Run = run;
    r.Parent = parent;
    r.Agent = agent;
    r.Task = task;
    r.Origin = origin;
    r.Graph = graph;
    r.Started = time;
    Push({time, 0, run, graph, 0, "task", "запуск " + task + " (" + origin + ")"});
}

void GraphDebugger::TaskFinished(int run, const std::string& status, const std::string& error, double time) {
    RunDebug* r = FindRun(run);
    if (!r) return;
    r->Status = status;
    r->Error = error;
    r->Ended = time;
    r->Current.clear();
    r->Paused = false;
    std::string text = r->Task + ": " + status;
    if (!error.empty()) text += " — " + error;
    Push({time, 0, run, r->Graph, 0, status == "failed" ? "error" : "task", text});
    // Завершённые живут ограниченно: за долгую игру их были бы тысячи.
    m_finished.push_back(run);
    while (m_finished.size() > kFinishedRuns) {
        m_runs.erase(m_finished.front());
        m_finished.pop_front();
    }
}

void GraphDebugger::NodeStepped(int run, const std::string& graph, int node, uint64_t frame, double time) {
    RunDebug& r = m_runs[run];
    r.NodeFrame[node] = frame;
    ++r.NodeHits[node];
    Push({time, 0, run, graph, node, "node", {}});
}

void GraphDebugger::PinValue(int run, int node, const std::string& pin, const std::string& value) {
    m_runs[run].Pins[{node, pin}] = value;
}

void GraphDebugger::SetCurrent(int run, std::set<int> nodes) {
    if (RunDebug* r = FindRun(run)) r->Current = std::move(nodes);
}

void GraphDebugger::Paused(int run, const std::string& graph, int node, double time) {
    RunDebug& r = m_runs[run];
    r.Paused = true;
    r.PausedNode = node;
    r.StepRequested = false;
    Push({time, 0, run, graph, node, "pause", "остановка на узле #" + std::to_string(node)});
}

void GraphDebugger::Log(int run, const std::string& graph, int node, const std::string& kind,
                        const std::string& text, double time) {
    Push({time, 0, run, graph, node, kind, text});
}

void GraphDebugger::Reset() {
    m_runs.clear();
    m_finished.clear();
    m_history.clear();
}

} // namespace sage::ai
