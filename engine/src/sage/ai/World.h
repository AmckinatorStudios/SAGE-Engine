#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/ai/Graph.h"
#include "sage/ai/Nodes.h"
#include "sage/ai/Task.h"
#include "sage/ai/Types.h"

// ---------------------------------------------------------------------------
// МИР ИИ: библиотека задач и операций, экземпляры ИИ и планировщик.
//
// БИБЛИОТЕКА ПО ИМЕНИ — ТО, ЧТО ДЕЛАЕТ GRAPH И LUA ОДНИМ ЦЕЛЫМ. Задача
// вызывает другую по имени и не знает, на чём та написана: Lua-задача зовёт
// граф, граф — Lua, C++ — обе. Перерегистрация того же имени заменяет
// описание (горячая перезагрузка скрипта): уже идущие выполнения доживают со
// старым, новые берут новое.
//
// ЭКЗЕМПЛЯР ИИ (Agent) — один «мозг»: память персонажа (Instance), имя общей
// области (Shared) и верхние задачи. Обычно привязан к объекту сцены; объект
// уничтожили — экземпляр уходит вместе с задачами (см. SetOwnerAlive).
//
// ОБЩИЕ ДАННЫЕ (Shared) — именованные области. Пределы задаёт автор игры: у
// стражей своя «guards», у торговцев своя, а «world» — по умолчанию у всех.
//
// СТРУКТУРА ВЫПОЛНЕНИЯ. Дочерняя задача не переживает родителя: родитель
// завершился или отменён — живые дети отменяются. Это то, что делает
// параллельные задачи управляемыми: отменил «Бой» — остановились и «Стрелять»,
// и «Искать укрытие», и никто не продолжает стрелять в пустоту.
//
// СОБЫТИЯ будят тех, кто ждёт их СЕЙЧАС. Очереди нет намеренно: шум, который
// прозвучал, пока страж был занят другим, он и не услышал — а «услышать
// задним числом» автор выражает памятью экземпляра, если ему это нужно.
// ---------------------------------------------------------------------------
namespace sage::ai {

struct Agent {
    int Id = 0;
    int Owner = 0; // Id объекта сцены; 0 — экземпляр без объекта
    std::string SharedSpace = "world";
    BlackboardPtr Memory = std::make_shared<Blackboard>();
    std::vector<RunPtr> Runs; // живые верхние задачи
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // --- Библиотека -------------------------------------------------------
    void RegisterTask(TaskDefinitionPtr definition);
    TaskDefinitionPtr FindTask(const std::string& name) const;
    std::vector<std::string> TaskNames() const;

    void RegisterOperation(Operation operation);
    const Operation* FindOperation(const std::string& name) const;
    std::vector<std::string> OperationNames() const;
    // Вызов операции: входы сверяются с объявлением (недостающие —
    // умолчанием), выходы — сколько объявлено.
    bool Call(const std::string& operation, const Values& in, Values& out, std::string& error);

    // --- Граф (SAGE AI Graph) ----------------------------------------------
    TypeRegistry& Types() { return m_types; }
    const TypeRegistry& Types() const { return m_types; }
    NodeRegistry& Nodes() { return m_nodes; }
    const NodeRegistry& Nodes() const { return m_nodes; }
    // Зарегистрировать граф задачей с именем doc.Name. Повторно — замена.
    bool RegisterGraph(const GraphDoc& doc, std::string& error);
    // Описание графа задачи (nullptr — задача не графом).
    std::shared_ptr<const GraphDoc> FindGraph(const std::string& task) const;
    // Меняется при каждой регистрации задачи, операции, типа или узла: по
    // ней граф узнаёт, что его пины надо собрать заново.
    uint64_t LibraryVersion() const { return m_version; }
    void TouchLibrary() { ++m_version; }

    // --- Экземпляры -------------------------------------------------------
    int CreateAgent(int owner = 0, const std::string& sharedSpace = "world");
    // Отменяет все задачи экземпляра и удаляет его.
    void DestroyAgent(int id);
    // Отменяет все задачи, экземпляр остаётся.
    void StopAgent(int id);
    Agent* FindAgent(int id);
    // Экземпляр, чей хозяин — объект с этим Id (0 — нет такого).
    int AgentOf(int owner) const;
    size_t AgentCount() const { return m_agents.size(); }

    BlackboardPtr Shared(const std::string& space);

    // --- Выполнение -------------------------------------------------------
    // Всегда возвращает выполнение. Не запустилось (нет экземпляра, задачи,
    // неизвестный вход) — оно сразу Failed, Rejected и с причиной в Error.
    // Первый шаг — в ближайшем Tick (в том же, если он уже идёт).
    RunPtr Start(int agent, const std::string& task, const Blackboard& inputs = {},
                 int parentRun = 0);
    bool Cancel(int runId);
    // Запустить и сразу шагать, пока выполнение не начнёт ждать или не
    // кончится: функция графа без ожиданий отдаёт результат в тот же вызов
    // (visual.call из Lua). Внутри идущего Tick — обычный Start: шаг будет в
    // этом же проходе.
    RunPtr RunNow(int agent, const std::string& task, const Blackboard& inputs = {});
    RunPtr FindRun(int runId) const;
    size_t ActiveCount() const { return m_byId.size(); }

    // Разбудить ждущих события. agent = 0 — всех; иначе — только его задачи.
    // Возвращает, скольких разбудило.
    int Emit(const std::string& event, const vars::Value& payload = {}, int agent = 0);

    void Tick(float dt);
    double Time() const { return m_time; }
    uint64_t Frame() const { return m_frame; }

    // Жив ли объект сцены с этим Id. Мир ИИ сцену не знает (он ниже её по
    // слоям), поэтому спрашивает тот, кто знает, — движок скриптов.
    void SetOwnerAlive(std::function<bool(int)> alive) { m_ownerAlive = std::move(alive); }

    // Снять всё без вызова отмены у задач: это не отмена, а конец мира
    // (остановка игры), и задачам уже некуда «прибирать».
    void Clear();

    // Сколько шагов одного выполнения за кадр самое большее. Родитель, чья
    // дочерняя задача кончилась мгновенно, продолжает в том же кадре — но
    // «мгновенная задача в бесконечном цикле» упирается сюда, а не вешает игру.
    static constexpr int kMaxStepsPerFrame = 16;

private:
    friend class TaskContext;

    bool Ready(Run& run);
    void StepRun(const RunPtr& run);
    void Finish(Run& run, TaskStatus status);
    void CancelRun(Run& run);
    void Detach(Run& run);
    void Wake(const RunPtr& run);
    void WakeParent(const Run& run);

    TypeRegistry m_types;
    NodeRegistry m_nodes;
    uint64_t m_version = 1;
    std::map<std::string, TaskDefinitionPtr> m_tasks;
    std::map<std::string, Operation> m_operations;

    std::map<int, Agent> m_agents;
    std::map<std::string, BlackboardPtr> m_shared;

    // Живые выполнения в порядке запуска (порядок шагов детерминирован) и
    // индекс по номеру. Завершённые уходят из обоих; ручки на них держат
    // свой указатель и видят итог.
    std::vector<RunPtr> m_active;
    std::unordered_map<int, RunPtr> m_byId;
    // Очередь шагов текущего прохода Tick: разбуженные посреди прохода
    // попадают сюда и шагают в том же кадре.
    std::deque<RunPtr> m_queue;
    bool m_ticking = false;

    std::function<bool(int)> m_ownerAlive;
    double m_time = 0.0;
    uint64_t m_frame = 0;
    int m_nextRun = 1;
    int m_nextAgent = 1;
};

} // namespace sage::ai
