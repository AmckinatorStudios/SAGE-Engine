#pragma once
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sage/ai/Nodes.h"
#include "sage/ai/Types.h"

// ---------------------------------------------------------------------------
// ВЫПОЛНЕНИЕ ГРАФА — граф SAGE AI это ЗАДАЧА мира ИИ.
//
// Граф регистрируется в библиотеке тем же TaskDefinition, что Lua и C++, и
// поэтому всё, что умеет задача, граф умеет «бесплатно»: входы и выходы,
// дети, отмена, события, ожидание из Lua (t.run("Patrol")) и из C++.
//
// ВЕТКИ ВЫПОЛНЕНИЯ (fibers). Внутри одного выполнения графа может идти
// несколько веток сразу — Parallel, обработчики событий. Ветка — это «где
// стоим» (узел) плюс стек возвратов (Sequence и циклы проходят свою ветку и
// возвращаются в себя) плюс то, чего ветка ждёт. Ждущая ветка не шагает:
// узел «Run Task: MoveTo» запускает перемещение ОДИН раз и ждёт его конца, а
// не перезапускает каждый кадр.
//
// Один шаг задачи (Step) — все готовые ветки идут, пока не упрутся в
// ожидание. Предел узлов на шаг (kMaxNodesPerStep) ловит бесконечный цикл
// без ожидания: такой граф завершается неудачей с понятной причиной, а не
// вешает игру.
//
// ГРАФ ЗАВЕРШАЕТСЯ узлом Return (успех или неудача — его вход ok) или когда
// все ветки дошли до конца. Граф с обработчиками событий (On Event) сам не
// кончается: он ждёт событий, пока его не отменят или не сработает Return.
// ---------------------------------------------------------------------------
namespace sage::ai {

// Граф, разобранный для выполнения: узлы с итоговыми пинами и проводами по
// ключу. Собирается один раз на версию библиотеки, а не на каждый запуск.
struct CompiledNode {
    const GraphNode* Node = nullptr;
    const NodeType* Type = nullptr;
    NodePins Pins;
    std::map<std::string, const GraphLink*> DataIn;   // вход данных -> провод
    std::map<std::string, int> ExecOut;               // выход потока -> узел
};

struct CompiledGraph {
    std::shared_ptr<const GraphDoc> Doc;
    std::vector<CompiledNode> Nodes;
    std::map<int, size_t> Index;
    int StartNode = 0;
    std::vector<size_t> EventNodes;   // On Event
    std::string Problem;              // почему граф нельзя выполнить
    const CompiledNode* Find(int id) const {
        auto it = Index.find(id);
        return it == Index.end() ? nullptr : &Nodes[it->second];
    }
};

std::shared_ptr<CompiledGraph> CompileGraph(std::shared_ptr<const GraphDoc> doc, const World& world);

// Ветка выполнения.
struct Fiber {
    enum class WaitKind { None, Frame, Time, Run, Event, Fibers, Condition };
    int Id = 0;
    int Node = 0;            // узел, который выполнить следующим; 0 — конец ветки
    bool Resume = false;     // повторный вызов узла после ожидания
    bool Reentered = false;  // возврат в узел со стека (Sequence/Loop)
    bool SkipBreak = false;  // точка останова на этом узле уже сработала
    bool Done = false;
    std::vector<int> Stack;
    WaitKind Wait = WaitKind::None;
    uint64_t FrameMark = 0;
    double Until = 0.0;
    RunPtr Run;
    std::string Event;
    std::vector<int> Waiting;
    bool All = true;
    std::function<bool()> Condition;
    bool TimedOut = false;
    vars::Value Payload;
};

class GraphBehavior;

// То, через что узел работает на своём вызове.
class GraphExec {
public:
    GraphExec(GraphBehavior& g, TaskContext& t, const CompiledNode& n, Fiber* f, bool resumed, bool reentered,
              int depth = 0)
        : m_g(g), m_t(t), m_n(n), m_f(f), m_resumed(resumed), m_reentered(reentered), m_depth(depth) {}

    vars::Value In(const std::string& pin);
    // Задан ли вход: соединён проводом или вписано значение. Необязательный
    // аргумент функции движка без значения передаётся как «нет», а не нулём.
    bool Has(const std::string& pin) const;
    void Out(const std::string& pin, const vars::Value& value);
    vars::Value Setting(const std::string& name) const;
    std::string SettingText(const std::string& name) const { return Setting(name).AsString(); }
    int SettingInt(const std::string& name) const { return Setting(name).AsInt(); }

    bool Resumed() const { return m_resumed; }
    bool Reentered() const { return m_reentered; }
    NodeState& State();
    const GraphNode& Node() const { return *m_n.Node; }
    const NodePins& Pins() const { return m_n.Pins; }
    TaskContext& Task() { return m_t; }
    World& GetWorld() { return m_t.GetWorld(); }
    const TypeRegistry& Types() const;
    const GraphDoc& Doc() const;

    // Ожидания текущей ветки: назначить и вернуть NodeResult::Wait().
    void WaitFrame();
    void WaitSeconds(float seconds);
    void WaitRun(RunPtr run);
    void WaitEvent(const std::string& name, float timeout);
    void WaitFibers(std::vector<int> fibers, bool all);
    void WaitUntil(std::function<bool()> condition);
    bool TimedOut() const { return m_f && m_f->TimedOut; }
    vars::Value EventPayload() const { return m_f ? m_f->Payload : vars::Value(); }

    // Новая ветка из выхода потока pin. 0 — выход ни с чем не соединён.
    int Spawn(const std::string& pin);
    void Kill(int fiber);
    bool FiberDone(int fiber) const;

    RunPtr StartTask(const std::string& name, const Blackboard& inputs);
    RunPtr TaskById(int id) const;

    // Переменные по области: local, instance, shared, input, output.
    Blackboard* Scope(const std::string& scope);
    // Return: завершить граф.
    void Finish(bool ok, const std::string& reason);
    void Log(const std::string& text);
    // Ошибка узла: граф завершается неудачей, причина называет узел.
    [[noreturn]] void Error(const std::string& message) const;

private:
    Fiber& Need(const char* what);
    GraphBehavior& m_g;
    TaskContext& m_t;
    const CompiledNode& m_n;
    Fiber* m_f;
    bool m_resumed;
    bool m_reentered;
    int m_depth;
};

class GraphTaskDefinition : public TaskDefinition {
public:
    GraphTaskDefinition(std::shared_ptr<const GraphDoc> doc, World& world);
    std::unique_ptr<TaskBehavior> Create() override;
    const std::shared_ptr<const GraphDoc>& Doc() const { return m_doc; }
    std::shared_ptr<CompiledGraph> Compiled();

private:
    std::shared_ptr<const GraphDoc> m_doc;
    World& m_world;
    std::shared_ptr<CompiledGraph> m_compiled;
    uint64_t m_version = ~0ull;
};

class GraphBehavior : public TaskBehavior {
public:
    explicit GraphBehavior(GraphTaskDefinition& def) : m_def(def) {}
    TaskStatus Step(TaskContext& ctx) override;
    void Cancel(TaskContext& ctx) override;

    static constexpr int kMaxNodesPerStep = 10000;

private:
    friend class GraphExec;
    bool Ready(Fiber& f);
    bool AnyReady();
    void RunFiber(TaskContext& ctx, Fiber& f);
    vars::Value Eval(TaskContext& ctx, const CompiledNode& n, const std::string& pin, int depth);
    vars::Value InputOf(TaskContext& ctx, const CompiledNode& n, const std::string& pin, int depth);
    Fiber& NewFiber(int node);
    Fiber* FindFiber(int id);
    void KillFiber(int id);
    void DeliverEvents(TaskContext& ctx);
    void Fail(const std::string& reason);
    int Target(const CompiledNode& n, const std::string& pin) const;
    const std::string& GraphName() const { return m_def.Name; }

    GraphTaskDefinition& m_def;
    std::shared_ptr<CompiledGraph> m_g;
    std::vector<std::unique_ptr<Fiber>> m_fibers;
    std::map<int, NodeState> m_state;
    std::map<int, RunPtr> m_tasks;
    std::deque<std::pair<std::string, vars::Value>> m_events;
    std::deque<RunPtr> m_cancelQueue;
    World* m_world = nullptr;
    int m_nextFiber = 1;
    int m_budget = 0;
    bool m_started = false;
    bool m_finished = false;
    bool m_ok = true;
    std::string m_reason;
    int m_runId = 0;
    bool m_paused = false;
    bool m_stepPause = false;
};

} // namespace sage::ai
