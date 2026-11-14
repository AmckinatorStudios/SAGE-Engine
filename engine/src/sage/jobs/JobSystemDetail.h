#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "sage/jobs/JobSystem.h"
#include "sage/jobs/WorkStealingDeque.h"

// Внутреннее устройство планировщика, общее для его файлов: запуск и
// остановка (JobSystem.cpp), постановка/исполнение/ожидание
// (JobScheduling.cpp), параллельные проходы (JobParallel.cpp), диагностика
// (JobDiagnostics.cpp). Снаружи движка не включается.
namespace sage::jobs::detail {

int64_t NowNs();

// Очередь полосы: три приоритета под одним замком. Секция — несколько
// инструкций (push/pop дека), а размер дублируется атомиком, чтобы пустую
// очередь проверять без замка — это самый частый случай у ищущего работу.
struct LaneQueue {
    explicit LaneQueue(const char* lockName) : M(lockName) {}
    sync::Mutex M;
    std::deque<JobRecord*> Q[(int)Priority::Count];
    std::atomic<int> Size{0};
    std::atomic<int> SizeByPrio[(int)Priority::Count] = {};

    void Push(JobRecord* r) {
        const int p = (int)r->Prio;
        {
            std::lock_guard<sync::Mutex> lk(M);
            Q[p].push_back(r);
            SizeByPrio[p].fetch_add(1, std::memory_order_relaxed);
        }
        Size.fetch_add(1, std::memory_order_seq_cst);
    }
    // Первая по приоритету (с maxPrio включительно).
    JobRecord* Pop(Priority maxPrio = Priority::Low) {
        if (Size.load(std::memory_order_seq_cst) == 0) return nullptr;
        std::lock_guard<sync::Mutex> lk(M);
        for (int p = 0; p <= (int)maxPrio; ++p) {
            if (Q[p].empty()) continue;
            JobRecord* r = Q[p].front();
            Q[p].pop_front();
            SizeByPrio[p].fetch_sub(1, std::memory_order_relaxed);
            Size.fetch_sub(1, std::memory_order_relaxed);
            return r;
        }
        return nullptr;
    }
    JobRecord* PopExactly(Priority prio) {
        const int p = (int)prio;
        if (SizeByPrio[p].load(std::memory_order_relaxed) == 0) return nullptr;
        std::lock_guard<sync::Mutex> lk(M);
        if (Q[p].empty()) return nullptr;
        JobRecord* r = Q[p].front();
        Q[p].pop_front();
        SizeByPrio[p].fetch_sub(1, std::memory_order_relaxed);
        Size.fetch_sub(1, std::memory_order_relaxed);
        return r;
    }
    std::vector<JobRecord*> DrainAll() {
        std::vector<JobRecord*> out;
        std::lock_guard<sync::Mutex> lk(M);
        for (int p = 0; p < (int)Priority::Count; ++p) {
            for (JobRecord* r : Q[p]) out.push_back(r);
            Q[p].clear();
            SizeByPrio[p].store(0, std::memory_order_relaxed);
        }
        Size.store(0, std::memory_order_relaxed);
        return out;
    }
};

// «Счётчик событий» (eventcount) для засыпания без потерянных пробуждений.
// Ждущий: Prepare → ещё раз проверить очереди → Commit (уснуть, если эпоха не
// сменилась). Будящий: положить работу → Notify. Атомик ждущих позволяет
// будящему вовсе не трогать замок, пока все потоки бодрствуют — а под
// нагрузкой так почти всегда.
struct Parker {
    std::mutex M;
    std::condition_variable CV;
    std::atomic<uint64_t> Epoch{0};
    std::atomic<int> Waiters{0};

    uint64_t Prepare() {
        Waiters.fetch_add(1, std::memory_order_seq_cst);
        return Epoch.load(std::memory_order_seq_cst);
    }
    void Cancel() { Waiters.fetch_sub(1, std::memory_order_seq_cst); }
    void Commit(uint64_t epoch, std::chrono::milliseconds timeout) {
        {
            std::unique_lock<std::mutex> lk(M);
            CV.wait_for(lk, timeout, [&] { return Epoch.load(std::memory_order_seq_cst) != epoch; });
        }
        Waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    void NotifyOne() {
        if (Waiters.load(std::memory_order_seq_cst) == 0) return;
        {
            std::lock_guard<std::mutex> lk(M);
            Epoch.fetch_add(1, std::memory_order_seq_cst);
        }
        CV.notify_one();
    }
    void NotifyAll() {
        {
            std::lock_guard<std::mutex> lk(M);
            Epoch.fetch_add(1, std::memory_order_seq_cst);
        }
        CV.notify_all();
    }
};

// Кольцо событий шкалы времени одного потока.
struct TraceRing {
    struct Item {
        char Name[40];
        Category Cat;
        int64_t StartNs, EndNs;
    };
    static constexpr int kSize = 4096;
    sync::SpinLock L;
    std::vector<Item> Items;
    int Head = 0;
    int Count = 0;
    void Push(const char* name, Category cat, int64_t s, int64_t e) {
        std::lock_guard<sync::SpinLock> lk(L);
        if (Items.empty()) Items.resize(kSize);
        Item& it = Items[(size_t)Head];
        std::strncpy(it.Name, name ? name : "", sizeof(it.Name) - 1);
        it.Name[sizeof(it.Name) - 1] = 0;
        it.Cat = cat;
        it.StartNs = s;
        it.EndNs = e;
        Head = (Head + 1) % kSize;
        if (Count < kSize) ++Count;
    }
};

struct CategoryCounters {
    std::atomic<uint64_t> Executed{0}, Failed{0}, Canceled{0};
    std::atomic<int64_t> ExecNs{0}, QueueNs{0}, MaxExecNs{0};
};

// Счётчики одного потока. Пишет их только сам поток (relaxed), а снимок
// складывает по всем потокам. Общие на систему атомики на каждую задачу —
// это линия кэша, которую все ядра перекидывают друг другу на каждом
// завершении: учёт стоил бы дороже самой мелкой задачи.
struct SlotCounters {
    std::atomic<uint64_t> Submitted{0}, Completed{0}, Failed{0}, Canceled{0};
    CategoryCounters Cats[(int)Category::Count];
    void Reset() {
        Submitted = 0;
        Completed = 0;
        Failed = 0;
        Canceled = 0;
        for (auto& c : Cats) {
            c.Executed = 0;
            c.Failed = 0;
            c.Canceled = 0;
            c.ExecNs = 0;
            c.QueueNs = 0;
            c.MaxExecNs = 0;
        }
    }
};

// Поток системы (воркер, поток ввода-вывода или «главный» — без своего потока).
struct alignas(64) ThreadSlot {
    std::thread Thread;
    std::string Name;
    Lane Kind = Lane::Worker;      // Worker / IO / Main
    int Index = 0;                  // индекс в Stats().Threads
    WorkStealingDeque<JobRecord*> Deque{256};

    // Что выполняется сейчас. Имя копируется под замком: запись задачи может
    // умереть сразу после её завершения, и читать её имя из чужого потока
    // по указателю было бы обращением к освобождённой памяти.
    sync::SpinLock InfoLock;
    char CurrentName[64] = {0};       // имя, собранное на лету (если CurrentStatic пуст)
    const char* CurrentStatic = nullptr; // имя-литерал
    Lane CurrentLane = Lane::Worker;
    Category CurrentCat = Category::General;
    std::atomic<int64_t> CurrentStartNs{0};   // 0 — свободен

    std::atomic<uint64_t> Executed{0};
    std::atomic<uint64_t> Stolen{0};
    std::atomic<uint64_t> Parks{0};
    std::atomic<int64_t> BusyNs{0};
    uint32_t Rng = 0x9E3779B9u;
    uint32_t PickCounter = 0;

    TraceRing Trace;
    SlotCounters C;

    // Предыдущий снимок для доли занятости (только под Core::StatsM).
    int64_t LastBusyNs = 0;
    int64_t LastSampleNs = 0;
    double LastUtil = 0.0;
};

struct Core {
    explicit Core(const JobSystemConfig& cfg);
    ~Core();

    // --- запуск/остановка (JobSystem.cpp)
    void Start();
    void Stop();
    void WorkerLoop(ThreadSlot* slot);
    void IoLoop(ThreadSlot* slot);
    void MaybeGrowIo();
    void SpawnIo();

    // --- планирование (JobScheduling.cpp)
    // false — система останавливается: задачу надо завершить как Canceled.
    bool Enqueue(JobRecord* job);
    void Execute(JobRecord* job, ThreadSlot* slot, bool stolen);
    JobRecord* FindWorkerJob(ThreadSlot* slot, bool& stolen);
    JobRecord* FindIoJob(ThreadSlot* slot);
    JobRecord* StealAny(ThreadSlot* self);
    bool TryAcquireBackgroundSlot();
    bool HelpOnce();
    int PumpLane(Lane lane, double budgetMs, int maxJobs);
    bool IsMainThread() const { return std::this_thread::get_id() == MainThread; }
    bool IsRenderThread() const { return std::this_thread::get_id() == RenderThread; }
    void RecordError(JobRecord* job);
    // Счётчики потока, на котором идёт вызов.
    SlotCounters& CountersHere();

    JobSystemConfig Cfg;
    int BackgroundLimit = 1;
    bool BackgroundOnIo = false;   // мало ядер: долгое — на потоки ввода-вывода
    int MaxIo = 8;
    int64_t StartNs = 0;

    std::vector<std::unique_ptr<ThreadSlot>> Workers;
    std::mutex IoM;                                  // рост пула ввода-вывода
    std::vector<std::unique_ptr<ThreadSlot>> Io;
    std::atomic<int> IoCount{0};
    std::atomic<int> IoIdle{0};
    ThreadSlot MainSlot;                             // главный поток (без своего std::thread)

    std::array<std::unique_ptr<LaneQueue>, (size_t)Lane::Count> Queues;
    Parker WorkerParker;
    Parker IoParker;
    // Воркеры, которые сейчас крутятся в поиске работы. Пока такой есть, будить
    // спящего незачем: ищущий найдёт задачу сам, а пробуждение — это вызов
    // ядра ОС на каждую постановку.
    std::atomic<int> Spinning{0};
    void WakeWorker() {
        if (Spinning.load(std::memory_order_seq_cst) == 0) WorkerParker.NotifyOne();
    }

    std::atomic<bool> Stopping{false};
    std::atomic<int> BackgroundRunning{0};
    std::thread::id MainThread;
    std::thread::id RenderThread;

    // Задачи, ждущие зависимостей: при остановке их отменяют, иначе зависшая
    // на чужом обещании задача осталась бы с указателем на умершую систему.
    std::mutex WaitingM;
    std::unordered_set<JobRecord*> WaitingSet;

    SlotCounters Foreign;   // постановки и завершения из чужих потоков
    std::atomic<uint64_t> Steals{0}, StealAttempts{0}, Helped{0}, DupPrevented{0};
    std::atomic<uint64_t> MainWaits{0};
    std::atomic<int64_t> MainWaitNs{0};
    std::atomic<int> RenderViolations{0};

    std::mutex ErrorsM;
    std::deque<JobError> Errors;

    std::atomic<bool> Tracing{false};
    mutable std::mutex StatsM;
};

// Поток исполнения: чей он и что выполняет.
struct ThreadCtx {
    Core* Owner = nullptr;          // воркер/поток ввода-вывода этой системы
    ThreadSlot* Slot = nullptr;
    JobRecord* Current = nullptr;
};
ThreadCtx& Ctx();

// Завершить задачу и всё, что от неё зависит (без рекурсии).
void FinishCascade(JobRecord* job, JobStatus status, std::exception_ptr error);
// Ядро, которому этот поток может помочь, ожидая job (или nullptr).
Core* HelpCoreFor(const JobRecord* job);

// Имя потока в отладчике/профилировщике ОС.
void SetCurrentThreadName(const char* name);
// Ядер, доступных процессу (с учётом маски сродства и квот контейнера).
int AvailableCores();

} // namespace sage::jobs::detail
