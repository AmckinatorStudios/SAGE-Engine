#include "sage/ai/World.h"

#include <algorithm>
#include <exception>

#include "sage/core/Log.h"

namespace sage::ai {

const char* StatusName(TaskStatus status) {
    switch (status) {
        case TaskStatus::Running: return "running";
        case TaskStatus::Succeeded: return "succeeded";
        case TaskStatus::Failed: return "failed";
        case TaskStatus::Cancelled: return "cancelled";
    }
    return "running";
}

// ---------------------------------------------------------------------------
// Задача на C++ из функций шага и отмены.
// ---------------------------------------------------------------------------
namespace {

class NativeBehavior : public TaskBehavior {
public:
    NativeBehavior(const StepFn* step, const CancelFn* cancel) : m_step(step), m_cancel(cancel) {}
    TaskStatus Step(TaskContext& ctx) override { return (*m_step)(ctx); }
    void Cancel(TaskContext& ctx) override {
        if (*m_cancel) (*m_cancel)(ctx);
    }

private:
    // Указатели на функции ОПИСАНИЯ: исполнитель держит описание живым через
    // Run::Definition, копировать функции в каждый запуск незачем.
    const StepFn* m_step;
    const CancelFn* m_cancel;
};

class NativeTask : public TaskDefinition {
public:
    NativeTask(StepFn step, CancelFn cancel) : m_step(std::move(step)), m_cancel(std::move(cancel)) {}
    std::unique_ptr<TaskBehavior> Create() override {
        return std::make_unique<NativeBehavior>(&m_step, &m_cancel);
    }

private:
    StepFn m_step;
    CancelFn m_cancel;
};

} // namespace

TaskDefinitionPtr MakeTask(const std::string& name, TaskSignature signature, StepFn step,
                           CancelFn cancel) {
    auto def = std::make_shared<NativeTask>(std::move(step), std::move(cancel));
    def->Name = name;
    def->Signature = std::move(signature);
    def->Origin = "C++";
    return def;
}

// ---------------------------------------------------------------------------
// Контекст шага.
// ---------------------------------------------------------------------------
Blackboard& TaskContext::Instance() {
    if (sage::ai::Agent* a = m_world.FindAgent(m_run.Agent)) return *a->Memory;
    // Экземпляр удалили посреди шага его же задачи (agent.destroy() изнутри):
    // шаг доживает на пустой памяти, а не на висячей ссылке.
    static Blackboard orphan;
    orphan.Clear();
    return orphan;
}

Blackboard& TaskContext::Shared() {
    sage::ai::Agent* a = m_world.FindAgent(m_run.Agent);
    return *m_world.Shared(a ? a->SharedSpace : "world");
}

int TaskContext::Owner() const {
    sage::ai::Agent* a = m_world.FindAgent(m_run.Agent);
    return a ? a->Owner : 0;
}

void TaskContext::WaitFrame() {
    m_run.Waiting = Wait{};
    m_run.Waiting.Kind = WaitKind::Frame;
    m_run.Waiting.FrameMark = m_world.m_frame;
}

void TaskContext::WaitSeconds(float seconds) {
    m_run.Waiting = Wait{};
    m_run.Waiting.Kind = WaitKind::Time;
    m_run.Waiting.Until = m_world.m_time + (seconds > 0.0f ? seconds : 0.0f);
    // Ноль секунд — это «следующий кадр», а не «сейчас»: иначе wait(0) в цикле
    // крутился бы в одном кадре до предела шагов.
    m_run.Waiting.FrameMark = m_world.m_frame;
}

void TaskContext::WaitEvent(const std::string& name, float timeout) {
    m_run.Waiting = Wait{};
    m_run.Waiting.Kind = WaitKind::Event;
    m_run.Waiting.Event = name;
    m_run.Waiting.Until = timeout > 0.0f ? m_world.m_time + timeout : 0.0;
    m_run.TimedOut = false;
    m_run.EventName.clear();
    m_run.EventPayload = vars::Value();
}

void TaskContext::WaitRuns(std::vector<RunPtr> runs, bool all) {
    m_run.Waiting = Wait{};
    m_run.Waiting.Kind = WaitKind::Runs;
    m_run.Waiting.Runs = std::move(runs);
    m_run.Waiting.All = all;
}

void TaskContext::WaitUntil(std::function<bool()> condition) {
    m_run.Waiting = Wait{};
    m_run.Waiting.Kind = WaitKind::Condition;
    m_run.Waiting.Condition = std::move(condition);
}

RunPtr TaskContext::Start(const std::string& task, const Blackboard& inputs) {
    return m_world.Start(m_run.Agent, task, inputs, m_run.Id);
}

TaskStatus TaskContext::Fail(const std::string& reason) {
    m_run.FailRequested = true;
    m_run.Error = reason;
    return TaskStatus::Failed;
}

// ---------------------------------------------------------------------------
// Библиотека.
// ---------------------------------------------------------------------------
World::World() = default;

World::~World() { Clear(); }

void World::RegisterTask(TaskDefinitionPtr definition) {
    if (!definition || definition->Name.empty()) return;
    m_tasks[definition->Name] = std::move(definition);
}

TaskDefinitionPtr World::FindTask(const std::string& name) const {
    auto it = m_tasks.find(name);
    return it == m_tasks.end() ? nullptr : it->second;
}

std::vector<std::string> World::TaskNames() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : m_tasks) out.push_back(name);
    return out;
}

void World::RegisterOperation(Operation operation) {
    if (operation.Name.empty() || !operation.Fn) return;
    std::string name = operation.Name;
    m_operations[name] = std::move(operation);
}

const Operation* World::FindOperation(const std::string& name) const {
    auto it = m_operations.find(name);
    return it == m_operations.end() ? nullptr : &it->second;
}

std::vector<std::string> World::OperationNames() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : m_operations) out.push_back(name);
    return out;
}

bool World::Call(const std::string& name, const Values& in, Values& out, std::string& error) {
    const Operation* op = FindOperation(name);
    if (!op) {
        error = "нет операции '" + name + "'";
        return false;
    }
    // Копия функции: операция может перерегистрировать сама себя (или другую)
    // изнутри, и тогда *op переехал бы посреди вызова.
    const auto fn = op->Fn;
    const std::vector<Param> inputs = op->Inputs;
    const std::vector<Param> outputs = op->Outputs;
    Values args = in;
    if (!inputs.empty()) {
        if (args.size() > inputs.size()) {
            error = "операции '" + name + "' дали " + std::to_string(args.size()) +
                    " входов, а она берёт " + std::to_string(inputs.size());
            return false;
        }
        for (size_t i = 0; i < inputs.size(); ++i) {
            const vars::Value& def = inputs[i].Default;
            if (i >= args.size()) args.push_back(def);
            else if (args[i].Type() != def.Type()) args[i] = vars::Value::Convert(args[i], def.Type());
        }
    }
    out.clear();
    bool ok = false;
    try {
        ok = fn(args, out, error);
    } catch (const std::exception& e) {
        error = e.what();
        ok = false;
    }
    if (!ok) {
        if (error.empty()) error = "операция '" + name + "' не удалась";
        return false;
    }
    if (!outputs.empty()) {
        out.resize(outputs.size());
        for (size_t i = 0; i < outputs.size(); ++i) {
            const vars::Value& def = outputs[i].Default;
            if (out[i].Type() != def.Type()) out[i] = vars::Value::Convert(out[i], def.Type());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Экземпляры.
// ---------------------------------------------------------------------------
int World::CreateAgent(int owner, const std::string& sharedSpace) {
    const int id = m_nextAgent++;
    Agent& a = m_agents[id];
    a.Id = id;
    a.Owner = owner;
    a.SharedSpace = sharedSpace.empty() ? "world" : sharedSpace;
    return id;
}

void World::StopAgent(int id) {
    Agent* a = FindAgent(id);
    if (!a) return;
    const std::vector<RunPtr> runs = a->Runs;
    for (const RunPtr& r : runs) CancelRun(*r);
}

void World::DestroyAgent(int id) {
    StopAgent(id);
    m_agents.erase(id);
}

Agent* World::FindAgent(int id) {
    auto it = m_agents.find(id);
    return it == m_agents.end() ? nullptr : &it->second;
}

BlackboardPtr World::Shared(const std::string& space) {
    BlackboardPtr& b = m_shared[space.empty() ? "world" : space];
    if (!b) b = std::make_shared<Blackboard>();
    return b;
}

// ---------------------------------------------------------------------------
// Выполнение.
// ---------------------------------------------------------------------------
RunPtr World::Start(int agentId, const std::string& task, const Blackboard& inputs, int parentRun) {
    auto run = std::make_shared<Run>();
    run->Id = m_nextRun++;
    run->Agent = agentId;
    run->Parent = parentRun;
    run->Task = task;
    auto reject = [&](const std::string& why) {
        run->Status = TaskStatus::Failed;
        run->Rejected = true;
        run->Error = why;
        return run;
    };

    Agent* agent = FindAgent(agentId);
    if (!agent) return reject("нет экземпляра ИИ #" + std::to_string(agentId));
    RunPtr parent;
    if (parentRun) {
        parent = FindRun(parentRun);
        if (!parent) return reject("задача-родитель #" + std::to_string(parentRun) + " уже завершена");
    }
    TaskDefinitionPtr def = FindTask(task);
    if (!def) return reject("нет задачи '" + task + "'");

    const TaskSignature& sig = def->Signature;
    if (!sig.Inputs.empty()) {
        for (const Param& p : sig.Inputs) run->Inputs->Declare(p.Name, p.Default);
        for (const auto& [name, value] : inputs.All()) {
            if (!run->Inputs->Set(name, value))
                return reject("у задачи '" + task + "' нет входа '" + name + "'");
        }
    } else {
        for (const auto& [name, value] : inputs.All()) run->Inputs->Set(name, value);
    }
    for (const Param& p : sig.Outputs) run->Outputs->Declare(p.Name, p.Default);

    run->Definition = def;
    run->Behavior = def->Create();
    if (!run->Behavior) return reject("задача '" + task + "' не создала исполнителя");

    if (parent) parent->Children.push_back(run);
    else agent->Runs.push_back(run);
    m_active.push_back(run);
    m_byId[run->Id] = run;
    if (m_ticking) m_queue.push_back(run);
    return run;
}

RunPtr World::FindRun(int runId) const {
    auto it = m_byId.find(runId);
    return it == m_byId.end() ? nullptr : it->second;
}

bool World::Cancel(int runId) {
    RunPtr r = FindRun(runId);
    if (!r) return false;
    CancelRun(*r);
    return true;
}

bool World::Ready(Run& r) {
    Wait& w = r.Waiting;
    switch (w.Kind) {
        case WaitKind::None: return true;
        case WaitKind::Frame: return m_frame > w.FrameMark;
        case WaitKind::Time: return m_frame > w.FrameMark && m_time >= w.Until;
        case WaitKind::Event:
            if (w.Until > 0.0 && m_time >= w.Until) {
                r.TimedOut = true;
                return true;
            }
            return false;
        case WaitKind::Runs: {
            size_t done = 0;
            for (const RunPtr& x : w.Runs) done += (x && IsFinished(x->Status)) ? 1 : 0;
            return w.All ? done == w.Runs.size() : (done > 0 || w.Runs.empty());
        }
        case WaitKind::Condition:
            try {
                return !w.Condition || w.Condition();
            } catch (const std::exception& e) {
                // Условие упало — это ошибка задачи, а не «ещё не выполнилось»:
                // иначе задача ждала бы вечно с ошибкой в каждом кадре.
                r.FailRequested = true;
                r.Error = e.what();
                return true;
            }
    }
    return true;
}

void World::StepRun(const RunPtr& run) {
    Run& r = *run;
    if (r.Status != TaskStatus::Running || !r.Behavior || r.InStep) return;
    if (r.StepFrame != m_frame) {
        r.StepFrame = m_frame;
        r.StepsThisFrame = 0;
    }
    if (!Ready(r)) return;
    if (r.FailRequested) {
        Finish(r, TaskStatus::Failed);
        return;
    }
    if (r.StepsThisFrame >= kMaxStepsPerFrame) {
        TaskContext(*this, r).WaitFrame();
        return;
    }
    ++r.StepsThisFrame;
    r.Waiting = Wait{};

    TaskContext ctx(*this, r);
    TaskStatus s = TaskStatus::Failed;
    r.InStep = true;
    try {
        s = r.Behavior->Step(ctx);
    } catch (const std::exception& e) {
        s = TaskStatus::Failed;
        r.Error = e.what();
        LOG_ERROR("AI") << "Задача '" << r.Task << "' упала: " << r.Error;
    }
    r.InStep = false;

    if (r.CancelRequested) {
        CancelRun(r);
        return;
    }
    if (r.FailRequested) s = TaskStatus::Failed;
    if (s == TaskStatus::Running) {
        if (r.Waiting.Kind == WaitKind::None) ctx.WaitFrame();
        // Ждёт уже завершённое (t.wait_for на готовую ручку) — продолжить в
        // этом же кадре: никто другой его больше не разбудит.
        if (r.Waiting.Kind == WaitKind::Runs && Ready(r)) Wake(run);
        return;
    }
    Finish(r, s);
}

void World::Detach(Run& r) {
    m_byId.erase(r.Id);
    auto drop = [&r](std::vector<RunPtr>& list) {
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&r](const RunPtr& x) { return x.get() == &r; }),
                   list.end());
    };
    if (r.Parent) {
        if (RunPtr p = FindRun(r.Parent)) drop(p->Children);
    } else if (Agent* a = FindAgent(r.Agent)) {
        drop(a->Runs);
    }
}

void World::Wake(const RunPtr& run) {
    if (m_ticking && run && run->Status == TaskStatus::Running) m_queue.push_back(run);
}

void World::WakeParent(const Run& r) {
    if (!r.Parent) return;
    RunPtr p = FindRun(r.Parent);
    if (p && p->Waiting.Kind == WaitKind::Runs) Wake(p);
}

void World::Finish(Run& r, TaskStatus status) {
    if (r.Status != TaskStatus::Running) return;
    r.Status = status;
    // Дети не переживают родителя (см. «Структура выполнения» в World.h).
    const std::vector<RunPtr> kids = std::move(r.Children);
    r.Children.clear();
    for (const RunPtr& c : kids) CancelRun(*c);
    // Ожидание и исполнитель держат ссылки в скриптовый язык (условие,
    // корутину) — отпускаем их сразу, а не когда пропадёт последняя ручка.
    r.Waiting = Wait{};
    r.Behavior.reset();
    Detach(r);
    WakeParent(r);
}

void World::CancelRun(Run& r) {
    if (r.Status != TaskStatus::Running) return;
    if (r.InStep) {
        // Отмена изнутри собственного шага (agent.stop() из задачи):
        // исполнитель сейчас на стеке, разрушать его нельзя — доделаем после.
        r.CancelRequested = true;
        return;
    }
    // Сначала дети: внутреннее прибирается раньше внешнего, как при выходе из
    // вложенных блоков.
    auto cancelKids = [this, &r]() {
        const std::vector<RunPtr> kids = std::move(r.Children);
        r.Children.clear();
        for (const RunPtr& c : kids) CancelRun(*c);
    };
    cancelKids();
    if (r.Behavior) {
        TaskContext ctx(*this, r);
        r.InStep = true;
        try {
            r.Behavior->Cancel(ctx);
        } catch (const std::exception& e) {
            LOG_ERROR("AI") << "Отмена задачи '" << r.Task << "' упала: " << e.what();
        }
        r.InStep = false;
    }
    // Запущенное из самой отмены тоже не переживает задачу.
    cancelKids();
    r.Status = TaskStatus::Cancelled;
    r.Waiting = Wait{};
    r.Behavior.reset();
    Detach(r);
    WakeParent(r);
}

int World::Emit(const std::string& event, const vars::Value& payload, int agent) {
    int woken = 0;
    // Копия списка: шаг не идёт, но разбуженный попадает в очередь, а очередь
    // и список живых — разные контейнеры; копия — на случай, если Emit
    // позовут из отмены, которая правит список.
    const std::vector<RunPtr> active = m_active;
    for (const RunPtr& run : active) {
        Run& r = *run;
        if (r.Status != TaskStatus::Running || r.Waiting.Kind != WaitKind::Event) continue;
        if (r.Waiting.Event != event) continue;
        if (agent && r.Agent != agent) continue;
        r.EventName = event;
        r.EventPayload = payload;
        r.TimedOut = false;
        r.Waiting = Wait{};
        ++woken;
        Wake(run);
    }
    return woken;
}

void World::Tick(float dt) {
    // Повторный вход (Tick из задачи) — не второй кадр, а ошибка порядка:
    // молча пропускаем, а не шагаем задачи посреди чужого шага.
    if (m_ticking) return;
    m_ticking = true;
    m_time += dt > 0.0f ? dt : 0.0f;
    ++m_frame;

    if (m_ownerAlive) {
        std::vector<int> dead;
        for (const auto& [id, a] : m_agents)
            if (a.Owner > 0 && !m_ownerAlive(a.Owner)) dead.push_back(id);
        for (int id : dead) DestroyAgent(id);
    }

    m_queue.assign(m_active.begin(), m_active.end());
    while (!m_queue.empty()) {
        RunPtr r = std::move(m_queue.front());
        m_queue.pop_front();
        StepRun(r);
    }

    m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
                                  [](const RunPtr& r) { return r->Status != TaskStatus::Running; }),
                   m_active.end());
    m_ticking = false;
}

void World::Clear() {
    for (const RunPtr& r : m_active) {
        if (r->Status == TaskStatus::Running) r->Status = TaskStatus::Cancelled;
        r->Waiting = Wait{};
        r->Children.clear();
        r->Behavior.reset();
    }
    m_active.clear();
    m_byId.clear();
    m_queue.clear();
    m_agents.clear();
    m_shared.clear();
    m_tasks.clear();
    m_operations.clear();
}

} // namespace sage::ai
