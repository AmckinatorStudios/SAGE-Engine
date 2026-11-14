#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/jobs/JobTypes.h"
#include "sage/jobs/Sync.h"

// ---------------------------------------------------------------------------
// Снимок состояния системы задач — для панели «Потоки» профилировщика,
// SAGE_PROFILE_LOG и тестов. Собирается по запросу (JobSystem::Stats()):
// счётчики копятся атомиками без замков, снимок их только читает.
// ---------------------------------------------------------------------------
namespace sage::jobs {

struct ThreadStats {
    std::string Name;
    Lane Kind = Lane::Worker;        // Worker — воркер, IO — поток ввода-вывода
    uint64_t Executed = 0;           // задач выполнено
    uint64_t Stolen = 0;             // из них украдено у соседей
    uint64_t Parks = 0;              // сколько раз засыпал без работы
    double BusyMs = 0.0;             // всего времени в задачах
    double Utilization = 0.0;        // доля занятости за последнее окно (0..1)
    std::string CurrentJob;          // что выполняет сейчас ("" — свободен)
    double CurrentJobMs = 0.0;       // и сколько уже
};

struct CategoryStats {
    Category Cat = Category::General;
    uint64_t Executed = 0;
    uint64_t Failed = 0;
    uint64_t Canceled = 0;
    double ExecMs = 0.0;             // суммарно в исполнении
    double QueueMs = 0.0;            // суммарно в очереди (готова, но не начата)
    double MaxExecMs = 0.0;
};

// Задача, которая идёт дольше порога (JobSystemConfig::HungJobMs) — кандидат
// в «зависшие»: бесконечный цикл, ожидание, которое не придёт, deadlock.
struct HungJob {
    std::string Name;
    std::string Thread;
    Lane InLane = Lane::Worker;
    Category Cat = Category::General;
    double RunningMs = 0.0;
};

struct JobError {
    std::string Job;
    std::string Message;
    Category Cat = Category::General;
    double AtSeconds = 0.0;          // от запуска системы
};

// Событие шкалы времени (трассировка включается SetTracing(true)).
struct TraceEvent {
    std::string Name;
    Category Cat = Category::General;
    int Thread = 0;                  // индекс потока в Threads (-1 — главный)
    double StartMs = 0.0;            // от запуска системы
    double EndMs = 0.0;
};

struct JobSystemStats {
    bool Running = false;
    int Workers = 0;
    int IoThreads = 0;
    int BackgroundLimit = 0;
    int Queued[(int)Lane::Count] = {};
    int Running_ = 0;                // выполняется сейчас (все полосы)
    int WaitingOnDeps = 0;           // ждут зависимостей
    int BackgroundRunning = 0;
    uint64_t Submitted = 0;
    uint64_t Completed = 0;
    uint64_t Failed = 0;
    uint64_t Canceled = 0;
    uint64_t Steals = 0;
    uint64_t StealAttempts = 0;
    uint64_t HelpedWhileWaiting = 0;  // задач выполнено ждущими потоками
    uint64_t DuplicateRunsPrevented = 0;
    uint64_t MainThreadWaits = 0;     // блокирующих ожиданий в главном потоке
    double MainThreadWaitMs = 0.0;
    double UptimeSeconds = 0.0;
    std::vector<ThreadStats> Threads;
    std::vector<CategoryStats> Categories;
    std::vector<sync::LockStats> Locks;
    std::vector<HungJob> Hung;
    std::vector<JobError> Errors;     // последние (кольцо)
};

} // namespace sage::jobs
