#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// ОТЛАДЧИК SAGE AI: что сейчас выполняется, что было и почему кончилось.
//
// Один на процесс: игра в Play редактора и редактор живут в одном процессе,
// и отладчику незачем ходить через сеть или файлы. Пишет он, только когда
// включён (Enabled) — в собранной игре его не включает никто, и запись не
// стоит ничего.
//
// ЧТО ПИШЕТСЯ:
//   • по каждому выполнению графа — какие узлы шагали (и в каком кадре: по
//     этому редактор подсвечивает «горячие» узлы и гасит остывшие), какие
//     значения прошли по пинам, на чём стоит сейчас каждая ветка;
//   • по КАЖДОЙ задаче мира — граф, Lua или C++ — запуск и конец с причиной:
//     так видно и то, что граф позвал Lua-задачу, и почему она не удалась;
//   • историю: узлы, события, операции, ошибки — последние kHistory записей.
//
// ТОЧКИ ОСТАНОВА ставятся по (граф, узел). Сработавшая ставит на паузу ВСЁ
// выполнение графа (все его ветки), а не одну ветку: значения на паузе
// должны стоять, иначе их нельзя рассмотреть. Continue — дальше до следующей
// точки, Step — один узел и снова пауза.
// ---------------------------------------------------------------------------
namespace sage::ai {

struct DebugEntry {
    double Time = 0.0;
    uint64_t Frame = 0;
    int Run = 0;
    std::string Graph;
    int Node = 0;
    std::string Kind;   // node, event, task, error, log, pause
    std::string Text;
};

struct RunDebug {
    int Run = 0;
    int Parent = 0;
    int Agent = 0;
    std::string Task;
    std::string Graph;     // пусто — задача не графом (Lua, C++)
    std::string Origin;
    std::string Status = "running";
    // Граф Visual Scripting, а не задача ИИ: отладчик ИИ его не показывает,
    // а редактор Visual Script — показывает.
    bool Script = false;
    // Последнее решение AI Graph этого выполнения: «почему именно эта задача».
    std::string LastDecision;
    std::string Error;
    double Started = 0.0, Ended = 0.0;
    std::map<int, uint64_t> NodeFrame;                      // узел -> кадр последнего шага
    std::map<int, int> NodeHits;                            // сколько раз шагал
    std::map<std::pair<int, std::string>, std::string> Pins;   // (узел, пин) -> значение
    std::set<int> Current;                                  // узлы, на которых стоят ветки
    bool Paused = false;
    int PausedNode = 0;
    bool StepRequested = false;
};

class GraphDebugger {
public:
    static GraphDebugger& Get();

    bool Enabled = false;
    static constexpr size_t kHistory = 2000;
    static constexpr size_t kFinishedRuns = 200;

    void ToggleBreakpoint(const std::string& graph, int node);
    bool HasBreakpoint(const std::string& graph, int node) const;
    const std::set<std::pair<std::string, int>>& Breakpoints() const { return m_breaks; }
    void ClearBreakpoints() { m_breaks.clear(); }

    void Continue(int run);
    void Step(int run);
    void ContinueAll();
    bool IsPaused(int run) const;

    // --- Запись (зовут мир ИИ и выполнение графа) --------------------------
    void TaskStarted(int run, int parent, int agent, const std::string& task, const std::string& origin,
                     const std::string& graph, double time);
    void TaskFinished(int run, const std::string& status, const std::string& error, double time);
    void NodeStepped(int run, const std::string& graph, int node, uint64_t frame, double time);
    void PinValue(int run, int node, const std::string& pin, const std::string& value);
    void SetCurrent(int run, std::set<int> nodes);
    // Сработала точка останова (или шаг): выполнение встаёт на паузу.
    void Paused(int run, const std::string& graph, int node, double time);
    // Как Log, но решение ещё и запоминается у выполнения (LastDecision).
    void Note(int run, const std::string& graph, int node, const std::string& kind, const std::string& text,
              double time);
    void Log(int run, const std::string& graph, int node, const std::string& kind, const std::string& text,
             double time);

    const std::map<int, RunDebug>& Runs() const { return m_runs; }
    RunDebug* FindRun(int run);
    const std::deque<DebugEntry>& History() const { return m_history; }
    uint64_t LastFrame() const { return m_frame; }
    void SetFrame(uint64_t frame) { m_frame = frame; }
    // Конец Play: история и выполнения больше не о чем; точки останова остаются.
    void Reset();

private:
    void Push(DebugEntry e);
    std::set<std::pair<std::string, int>> m_breaks;
    std::map<int, RunDebug> m_runs;
    std::deque<int> m_finished;
    std::deque<DebugEntry> m_history;
    uint64_t m_frame = 0;
};

} // namespace sage::ai
