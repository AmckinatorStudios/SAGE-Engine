#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/ai/Blackboard.h"

// ---------------------------------------------------------------------------
// SAGE AI: ЗАДАЧА — единица поведения, которую целиком определяет автор игры.
//
// Движок НЕ знает ни «патруля», ни «преследования»: ему всё равно, что задача
// делает. Он знает только, как её запустить, разбудить, дождаться, отменить и
// отдать результат — одинаково для задачи на C++, на Lua и на графе. Поэтому
// здесь три понятия, и ни одно из них не про игру:
//
//   TaskDefinition — ОПИСАНИЕ задачи: имя, входы и выходы, и из чего делать
//                    исполнителя. Одно на все запуски и на всех персонажей.
//   TaskBehavior   — ИСПОЛНИТЕЛЬ одного запуска: собственное состояние
//                    (корутина Lua, текущий узел графа, номер шага C++).
//   Run            — ВЫПОЛНЕНИЕ: состояние, входы, выходы, локальные
//                    переменные, дети. То, что видит вызывающий.
//
// Исполнитель создаётся на каждый запуск, поэтому одна задача «MoveTo» у
// десяти стражей — десять независимых состояний, а не одно общее.
//
// ДОЛГАЯ ЗАДАЧА НЕ НАЧИНАЕТСЯ ЗАНОВО КАЖДЫЙ КАДР. Исполнитель зовут, только
// когда он готов: кончилось ожидание времени, пришло событие, завершились
// дети, выполнилось условие. Между вызовами он хранит, где остановился. Это и
// дешевле (ждущая задача стоит одной проверки), и честнее: «иду к двери» не
// пересчитывает путь шестьдесят раз в секунду только потому, что так устроен
// цикл.
// ---------------------------------------------------------------------------
namespace sage::ai {

class World;

enum class TaskStatus {
    Running = 0,
    Succeeded,
    Failed,
    Cancelled,
};

// Имя для Lua, графа и журнала: "running", "succeeded", "failed", "cancelled".
const char* StatusName(TaskStatus status);
inline bool IsFinished(TaskStatus status) { return status != TaskStatus::Running; }

// Вход или выход задачи. Умолчание задаёт и тип: граф по нему красит провод, а
// запись чужого вида приводится к нему (см. Blackboard).
struct Param {
    std::string Name;
    vars::Value Default;
};

// Подпись задачи. Пустой список входов — задача берёт любые входы как есть
// (быстрый набросок на Lua); объявленный — строгий (см. Blackboard).
struct TaskSignature {
    std::vector<Param> Inputs;
    std::vector<Param> Outputs;
};

struct Run;
using RunPtr = std::shared_ptr<Run>;

// Чего ждёт выполнение. Одно ожидание за раз: задача, которой нужно «событие
// ИЛИ таймаут», выражает это одним ожиданием события со сроком, а не двумя.
enum class WaitKind {
    None,      // готово к шагу
    Frame,     // следующий кадр
    Time,      // до момента Until по часам мира
    Event,     // событие с именем Event (и, если Until > 0, не дольше)
    Runs,      // завершения задач из Runs (всех или любой)
    Condition, // пока Condition() не станет true
};

struct Wait {
    WaitKind Kind = WaitKind::None;
    double Until = 0.0;
    uint64_t FrameMark = 0;
    std::string Event;
    std::vector<RunPtr> Runs;
    bool All = true;
    std::function<bool()> Condition;
};

class TaskContext;

// Исполнитель одного запуска. Step зовётся при каждом пробуждении и
// возвращает Running (жду — тогда до возврата назначают ожидание через
// TaskContext), Succeeded или Failed. Running без ожидания — «продолжу в
// следующем кадре»: иначе забытое ожидание крутило бы задачу в одном кадре
// бесконечно.
class TaskBehavior {
public:
    virtual ~TaskBehavior() = default;
    virtual TaskStatus Step(TaskContext& ctx) = 0;
    // Отмена извне: прибрать за собой (остановить перемещение, снять метку).
    // После неё исполнитель больше не зовут.
    virtual void Cancel(TaskContext&) {}
};

class TaskDefinition {
public:
    virtual ~TaskDefinition() = default;
    virtual std::unique_ptr<TaskBehavior> Create() = 0;

    std::string Name;
    TaskSignature Signature;
    // Чем написана: "C++", "Lua", "Graph" — для журнала и редактора. Правил
    // выполнения не меняет: они одни на всех.
    std::string Origin = "C++";
};
using TaskDefinitionPtr = std::shared_ptr<TaskDefinition>;

// Выполнение задачи. Поля читают все, а меняет только World: вызывающий
// узнаёт состояние, но не может «назначить» задаче успех в обход её самой.
struct Run {
    int Id = 0;
    int Agent = 0;   // экземпляр ИИ, чья это задача
    int Parent = 0;  // запустившее выполнение; 0 — верхнее (запущено экземпляру)
    std::string Task;
    TaskStatus Status = TaskStatus::Running;
    std::string Error;
    // Не запустилась вовсе (нет такой задачи, неизвестный вход): это
    // опечатка вызывающего, а не неудача задачи, и скрипт получает ошибку.
    bool Rejected = false;

    BlackboardPtr Inputs = std::make_shared<Blackboard>();
    BlackboardPtr Outputs = std::make_shared<Blackboard>();
    BlackboardPtr Locals = std::make_shared<Blackboard>();

    // Шаг исполнителя, на котором он остановился. Удобно задачам на C++,
    // которые пишутся машиной состояний: case 0 — запустить, case 1 — ждать.
    int Phase = 0;

    // Последнее событие, разбудившее выполнение, и было ли это таймаутом.
    std::string EventName;
    vars::Value EventPayload;
    bool TimedOut = false;

    // --- Внутреннее хозяйство World --------------------------------------
    TaskDefinitionPtr Definition;
    std::unique_ptr<TaskBehavior> Behavior;
    std::vector<RunPtr> Children; // живые дети; завершённые выпадают сразу
    Wait Waiting;
    uint64_t StepFrame = 0;
    int StepsThisFrame = 0;
    bool InStep = false;
    bool CancelRequested = false;
    bool FailRequested = false;
    // Слушатель событий мира, независимо от ожидания: граф с обработчиками
    // On Event слышит событие, даже когда его ветки ждут другого. true —
    // событие нужно, разбудить выполнение.
    std::function<bool(const std::string& event, const vars::Value& payload)> Listener;
};

// То, через что исполнитель говорит с миром на своём шаге. Живёт ровно один
// шаг: держать его дольше незачем, а ссылки внутри после шага не обещаны.
class TaskContext {
public:
    TaskContext(World& world, Run& run) : m_world(world), m_run(run) {}

    World& GetWorld() { return m_world; }
    Run& GetRun() { return m_run; }
    int& Phase() { return m_run.Phase; }

    Blackboard& Inputs() { return *m_run.Inputs; }
    Blackboard& Outputs() { return *m_run.Outputs; }
    Blackboard& Locals() { return *m_run.Locals; }
    Blackboard& Instance();
    Blackboard& Shared();
    int Agent() const { return m_run.Agent; }
    int Owner() const;

    // Ожидания: назначить и вернуть из Step значение Running.
    void WaitFrame();
    void WaitSeconds(float seconds);
    void WaitEvent(const std::string& name, float timeout = 0.0f);
    void WaitRuns(std::vector<RunPtr> runs, bool all = true);
    void WaitUntil(std::function<bool()> condition);

    // Дочерняя задача того же экземпляра. Отменяется вместе с родителем и
    // завершается не позже него.
    RunPtr Start(const std::string& task, const Blackboard& inputs = {});

    // Завершиться неудачей с причиной: Step после этого возвращает Failed.
    TaskStatus Fail(const std::string& reason);

private:
    World& m_world;
    Run& m_run;
};

// Задача на C++ из одной функции шага (и, при желании, функции отмены).
using StepFn = std::function<TaskStatus(TaskContext&)>;
using CancelFn = std::function<void(TaskContext&)>;
TaskDefinitionPtr MakeTask(const std::string& name, TaskSignature signature, StepFn step,
                           CancelFn cancel = {});

// ОПЕРАЦИЯ — одно мгновенное действие: проверить, вычислить, спросить мир.
// В отличие от задачи она не ждёт и не имеет состояния — её просто зовут.
// Граф показывает её узлом, Lua зовёт ai.call; записана она может быть на чём
// угодно. Ошибка — false и причина в error.
using Values = std::vector<vars::Value>;
struct Operation {
    std::string Name;
    std::vector<Param> Inputs;
    std::vector<Param> Outputs;
    std::string Origin = "C++";
    std::function<bool(const Values& in, Values& out, std::string& error)> Fn;
};

} // namespace sage::ai
