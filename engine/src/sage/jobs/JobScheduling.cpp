#include "sage/jobs/JobSystemDetail.h"

#include <algorithm>
#include <cstdio>

#include "sage/core/Log.h"

// Постановка, исполнение, завершение и ожидание задач. Устройство — в шапке
// JobSystem.h и в docs/multithreading.md; здесь — почему каждая деталь именно
// такая.
namespace sage::jobs::detail {

int64_t NowNs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ThreadCtx& Ctx() {
    thread_local ThreadCtx ctx;
    return ctx;
}

JobRecord* CurrentJob() { return Ctx().Current; }

// --- Запись задачи -----------------------------------------------------------

JobRecord::~JobRecord() {
    // Потерянная ошибка: задача упала, а её исключение никто не прочёл (ни
    // ручкой, ни группой, ни продолжением). Без этой строки «фоновая загрузка
    // молча ничего не сделала» расследовалась бы по косвенным признакам.
    if (GetStatus() == JobStatus::Failed && Error && !Group &&
        !ErrorObserved.load(std::memory_order_relaxed)) {
        try {
            std::rethrow_exception(Error);
        } catch (const std::exception& e) {
            LOG_WARN("Jobs") << "Исключение задачи '" << DisplayName()
                             << "' никто не прочитал: " << e.what();
        } catch (...) {
            LOG_WARN("Jobs") << "Исключение задачи '" << DisplayName() << "' никто не прочитал";
        }
    }
    // Последователи без ссылок здесь не остаются: список забирает FinishCascade.
    for (JobRecord* s : Successors) s->Release();
}

bool JobRecord::ShouldCancel() const {
    if (CancelRequested.load(std::memory_order_acquire)) return true;
    if (Token && Token->Requested.load(std::memory_order_acquire)) return true;
    if (Group && Group->Cancel.IsCancelled()) return true;
    // Owner неизменен после постановки, а идущая/стоящая в очереди задача
    // держит систему живой (Stop дожидается всех) — читать его безопасно.
    if (Owner && Owner->Stopping.load(std::memory_order_acquire)) return true;
    return false;
}

void Prepare(JobRecord* job, const JobOptions& opts) {
    job->Name = opts.Name ? opts.Name : "job";
    if (!opts.DynamicName.empty()) job->OwnedName = opts.DynamicName;
    job->Cat = opts.Cat;
    job->InLane = opts.InLane;
    job->Prio = opts.Prio;
    job->RunIfDepFailed = opts.RunIfDependencyFailed;
    job->Token = opts.Token.State();
    if (opts.Group) job->Group = opts.Group->State();
    job->CreateNs = NowNs();
}

void RequestCancel(JobRecord* job) {
    job->CancelRequested.store(true, std::memory_order_release);
}

namespace {

// Ожидание с пробуждением: полосатый набор (condition_variable на адрес).
// Один на процесс, а не на систему: ручка может пережить систему, и ждать на
// её объекте было бы обращением к умершему.
struct Hub {
    std::mutex M;
    std::condition_variable CV;
};
Hub& HubFor(const void* p) {
    static Hub hubs[64];
    return hubs[((uintptr_t)p >> 6) % 64];
}

void NotifyHub(const void* p) {
    Hub& h = HubFor(p);
    { std::lock_guard<std::mutex> lk(h.M); }
    h.CV.notify_all();
}

// Зависимость не удалась: последователь не будет запущен (если он не уборщик)
// и унаследует ошибку — первую пришедшую.
void MarkDepFailed(JobRecord* succ, JobRecord* dep, JobStatus depStatus) {
    succ->DepFailed.store(true, std::memory_order_release);
    if (depStatus == JobStatus::Failed && dep->Error) {
        dep->ErrorObserved.store(true, std::memory_order_relaxed);
        std::lock_guard<sync::SpinLock> lk(succ->SuccLock);
        if (!succ->Error) succ->Error = dep->Error;
    }
}

// Переход Waiting → Queued: ровно один выигравший готовит задачу к запуску.
// Проигравший (задачу уже отменила остановка) ничего не делает.
bool ClaimReady(JobRecord* job) {
    uint8_t expected = (uint8_t)JobStatus::Waiting;
    if (!job->Status.compare_exchange_strong(expected, (uint8_t)JobStatus::Queued,
                                             std::memory_order_acq_rel))
        return false;
    if (job->InWaitingSet.load(std::memory_order_acquire) && job->Owner) {
        std::lock_guard<std::mutex> lk(job->Owner->WaitingM);
        job->Owner->WaitingSet.erase(job);
        job->InWaitingSet.store(false, std::memory_order_release);
    }
    return true;
}

struct Pending {
    JobRecord* Job;
    JobStatus St;
    std::exception_ptr Err;
};

// Готовая задача: в очередь, либо (зависимость упала / система встала) —
// в список на завершение.
void Dispatch(JobRecord* job, std::vector<Pending>& work) {
    if (job->DepFailed.load(std::memory_order_acquire) && !job->RunIfDepFailed) {
        std::exception_ptr inherited;
        {
            std::lock_guard<sync::SpinLock> lk(job->SuccLock);
            inherited = job->Error;
        }
        work.push_back({job, inherited ? JobStatus::Failed : JobStatus::Canceled, inherited});
        return;
    }
    if (!job->Owner || !job->Owner->Enqueue(job))
        work.push_back({job, JobStatus::Canceled, nullptr});
}

} // namespace

void FinishCascade(JobRecord* first, JobStatus firstStatus, std::exception_ptr firstError) {
    // Цепочку из тысяч продолжений отменяют одним вызовом — рекурсия здесь
    // съела бы стек, поэтому список работ.
    std::vector<Pending> work;
    work.push_back({first, firstStatus, std::move(firstError)});
    std::vector<JobRecord*> succs;
    while (!work.empty()) {
        Pending p = std::move(work.back());
        work.pop_back();
        JobRecord* r = p.Job;
        if (p.St == JobStatus::Failed && p.Err) {
            std::lock_guard<sync::SpinLock> lk(r->SuccLock);
            r->Error = p.Err;
        }
        Core* core = r->Owner;
        if (core) {
            CategoryCounters& c = core->Cats[(int)r->Cat];
            if (p.St == JobStatus::Failed) {
                core->Failed.fetch_add(1, std::memory_order_relaxed);
                c.Failed.fetch_add(1, std::memory_order_relaxed);
                core->RecordError(r);
            } else if (p.St == JobStatus::Canceled) {
                core->Canceled.fetch_add(1, std::memory_order_relaxed);
                c.Canceled.fetch_add(1, std::memory_order_relaxed);
            }
            core->Completed.fetch_add(1, std::memory_order_relaxed);
        }

        // Итог — до закрытия списка: тот, кто увидит список закрытым, обязан
        // увидеть и итог (иначе принял бы незавершённую за удавшуюся).
        r->Status.store((uint8_t)p.St, std::memory_order_seq_cst);
        {
            std::lock_guard<sync::SpinLock> lk(r->SuccLock);
            r->SuccClosed = true;
            succs.swap(r->Successors);
        }

        if (r->Group) {
            GroupState& g = *r->Group;
            if (p.St == JobStatus::Failed) {
                g.Failures.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(g.ErrorM);
                if (!g.FirstError) g.FirstError = r->Error;
            }
            if (g.Pending.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
                g.Waiters.load(std::memory_order_seq_cst) > 0)
                NotifyHub(&g);
        }
        if (r->Waiters.load(std::memory_order_seq_cst) > 0) NotifyHub(r);

        for (JobRecord* s : succs) {
            if (p.St != JobStatus::Succeeded) MarkDepFailed(s, r, p.St);
            if (s->Unfinished.fetch_sub(1, std::memory_order_acq_rel) == 1 && ClaimReady(s))
                Dispatch(s, work);
            s->Release();  // ссылка списка последователей
        }
        succs.clear();
        r->Release();  // ссылка планировщика
    }
}

void Submit(Core* core, JobRecord* job, const JobHandle* deps, size_t depCount) {
    job->Owner = core;
    if (job->Group) {
        job->Group->Pending.fetch_add(1, std::memory_order_seq_cst);
        if (core) job->Group->LastCore.store(core, std::memory_order_release);
    }
    if (!core || core->Stopping.load(std::memory_order_acquire)) {
        // Система не принимает работу: задача сразу отменена — Future скажет
        // «отменено», а не повиснет навсегда.
        job->Status.store((uint8_t)JobStatus::Queued, std::memory_order_relaxed);
        FinishCascade(job, JobStatus::Canceled, nullptr);
        return;
    }
    core->Submitted.fetch_add(1, std::memory_order_relaxed);
    job->Unfinished.store(1 + (int)depCount, std::memory_order_relaxed);
    if (depCount > 0) {
        std::lock_guard<std::mutex> lk(core->WaitingM);
        core->WaitingSet.insert(job);
        job->InWaitingSet.store(true, std::memory_order_release);
    }
    for (size_t i = 0; i < depCount; ++i) {
        JobRecord* d = deps[i].Record();
        if (!d) {
            job->Unfinished.fetch_sub(1, std::memory_order_acq_rel);
            continue;
        }
        bool linked = false;
        {
            std::lock_guard<sync::SpinLock> lk(d->SuccLock);
            if (!d->SuccClosed) {
                job->AddRef();
                d->Successors.push_back(job);
                linked = true;
            }
        }
        if (!linked) {
            const JobStatus st = d->GetStatus();
            if (st != JobStatus::Succeeded) MarkDepFailed(job, d, st);
            job->Unfinished.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    // Снять стража: если все зависимости уже готовы — задача готова сейчас.
    if (job->Unfinished.fetch_sub(1, std::memory_order_acq_rel) == 1 && ClaimReady(job)) {
        std::vector<Pending> work;
        Dispatch(job, work);
        for (Pending& p : work) FinishCascade(p.Job, p.St, p.Err);
    }
}

void CompleteManual(JobRecord* job, JobStatus status, std::exception_ptr error) {
    uint8_t expected = (uint8_t)JobStatus::Running;
    if (!job->Status.compare_exchange_strong(expected, (uint8_t)JobStatus::Queued,
                                             std::memory_order_acq_rel))
        return;  // уже выставлено
    job->AddRef();  // FinishCascade отпускает «ссылку планировщика»
    FinishCascade(job, status, std::move(error));
}

// --- Очереди -----------------------------------------------------------------

bool Core::Enqueue(JobRecord* job) {
    if (Stopping.load(std::memory_order_acquire)) return false;
    job->ReadyNs = NowNs();
    ThreadCtx& ctx = Ctx();
    switch (job->InLane) {
        case Lane::Worker:
            // Своя подзадача — в свой дек (LIFO, горячая в кэше); воры заберут
            // лишнее. Низкий приоритет — в общую очередь, чтобы фоновое
            // дробление не встало впереди кадровой работы соседей.
            if (ctx.Owner == this && ctx.Slot && ctx.Slot->Kind == Lane::Worker &&
                job->Prio != Priority::Low) {
                ctx.Slot->Deque.Push(job);
                std::atomic_thread_fence(std::memory_order_seq_cst);
            } else {
                Queues[(int)Lane::Worker]->Push(job);
            }
            if (Workers.empty()) IoParker.NotifyOne();
            else WorkerParker.NotifyOne();
            return true;
        case Lane::Background:
            Queues[(int)Lane::Background]->Push(job);
            if (BackgroundOnIo) {
                MaybeGrowIo();
                IoParker.NotifyOne();
            } else {
                WorkerParker.NotifyOne();
            }
            return true;
        case Lane::IO:
            Queues[(int)Lane::IO]->Push(job);
            MaybeGrowIo();
            IoParker.NotifyOne();
            return true;
        case Lane::Main:
        case Lane::Render:
        default:
            // Главный поток разбирает её в PumpMainThread/PumpRenderThread; если
            // он сейчас ждёт (Wait), то просыпается по короткому таймауту сам.
            Queues[(int)job->InLane]->Push(job);
            return true;
    }
}

bool Core::TryAcquireBackgroundSlot() {
    int cur = BackgroundRunning.load(std::memory_order_relaxed);
    while (cur < BackgroundLimit) {
        if (BackgroundRunning.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel))
            return true;
    }
    return false;
}

JobRecord* Core::StealAny(ThreadSlot* self) {
    const int n = (int)Workers.size();
    if (n == 0) return nullptr;
    uint32_t& rng = self ? self->Rng : MainSlot.Rng;
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    const int start = (int)(rng % (uint32_t)n);
    for (int i = 0; i < n; ++i) {
        ThreadSlot* victim = Workers[(size_t)((start + i) % n)].get();
        if (victim == self || victim->Deque.EmptyApprox()) continue;
        StealAttempts.fetch_add(1, std::memory_order_relaxed);
        // Проигранная гонка за последний элемент — не «пусто»: пробуем ещё.
        for (int tries = 0; tries < 2; ++tries) {
            if (JobRecord* j = victim->Deque.Steal()) {
                Steals.fetch_add(1, std::memory_order_relaxed);
                return j;
            }
            if (victim->Deque.EmptyApprox()) break;
        }
    }
    return nullptr;
}

JobRecord* Core::FindWorkerJob(ThreadSlot* slot, bool& stolen) {
    stolen = false;
    LaneQueue& wq = *Queues[(int)Lane::Worker];
    if (JobRecord* j = slot->Deque.Pop()) return j;
    // Против голодания: каждая 16-я выборка начинается с низкого приоритета —
    // иначе под постоянной кадровой нагрузкой он не шёл бы никогда.
    if ((++slot->PickCounter & 15u) == 0)
        if (JobRecord* j = wq.PopExactly(Priority::Low)) return j;
    if (JobRecord* j = wq.Pop(Priority::Normal)) return j;
    if (JobRecord* j = StealAny(slot)) { stolen = true; return j; }
    if (JobRecord* j = wq.PopExactly(Priority::Low)) return j;
    LaneQueue& bq = *Queues[(int)Lane::Background];
    if (!BackgroundOnIo && bq.Size.load(std::memory_order_relaxed) > 0 && TryAcquireBackgroundSlot()) {
        if (JobRecord* j = bq.Pop()) return j;
        BackgroundRunning.fetch_sub(1, std::memory_order_acq_rel);
    }
    return nullptr;
}

JobRecord* Core::FindIoJob(ThreadSlot*) {
    if (JobRecord* j = Queues[(int)Lane::IO]->Pop()) return j;
    if (BackgroundOnIo) {
        if (JobRecord* j = Queues[(int)Lane::Background]->Pop()) {
            BackgroundRunning.fetch_add(1, std::memory_order_acq_rel);
            return j;
        }
    }
    // Без воркеров (одно ядро) кадровую задачу, которую никто не ждёт, иначе
    // некому было бы выполнить.
    if (Workers.empty())
        if (JobRecord* j = Queues[(int)Lane::Worker]->Pop()) return j;
    return nullptr;
}

// --- Исполнение --------------------------------------------------------------

void Core::Execute(JobRecord* job, ThreadSlot* slot, bool stolen) {
    uint8_t expected = (uint8_t)JobStatus::Queued;
    if (!job->Status.compare_exchange_strong(expected, (uint8_t)JobStatus::Running,
                                             std::memory_order_acq_rel)) {
        // Повторного выполнения не бывает: задача досталась кому-то другому.
        DupPrevented.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool background = job->InLane == Lane::Background;
    if (job->ShouldCancel()) {
        if (background) BackgroundRunning.fetch_sub(1, std::memory_order_acq_rel);
        FinishCascade(job, JobStatus::Canceled, nullptr);
        return;
    }
    if (!slot) slot = &MainSlot;
    ThreadCtx& ctx = Ctx();
    JobRecord* prev = ctx.Current;
    ctx.Current = job;
    const int64_t start = NowNs();
    job->StartNs = start;
    job->ThreadIndex = (int16_t)slot->Index;
    // Вложенное исполнение (помощь во время ожидания) — внешняя задача того
    // же потока уже отмечена; показываем самую внутреннюю.
    char savedName[64];
    int64_t savedStart = slot->CurrentStartNs.load(std::memory_order_relaxed);
    Lane savedLane = slot->CurrentLane;
    Category savedCat = slot->CurrentCat;
    {
        std::lock_guard<sync::SpinLock> lk(slot->InfoLock);
        std::memcpy(savedName, slot->CurrentName, sizeof(savedName));
        std::strncpy(slot->CurrentName, job->DisplayName(), sizeof(slot->CurrentName) - 1);
        slot->CurrentName[sizeof(slot->CurrentName) - 1] = 0;
        slot->CurrentLane = job->InLane;
        slot->CurrentCat = job->Cat;
        slot->CurrentStartNs.store(start, std::memory_order_relaxed);
    }
    RunningNow.fetch_add(1, std::memory_order_relaxed);

    JobStatus status = JobStatus::Succeeded;
    std::exception_ptr error;
    try {
        job->Run();
    } catch (...) {
        error = std::current_exception();
        status = JobStatus::Failed;
    }

    const int64_t end = NowNs();
    job->EndNs = end;
    RunningNow.fetch_sub(1, std::memory_order_relaxed);
    {
        std::lock_guard<sync::SpinLock> lk(slot->InfoLock);
        std::memcpy(slot->CurrentName, savedName, sizeof(savedName));
        slot->CurrentLane = savedLane;
        slot->CurrentCat = savedCat;
        slot->CurrentStartNs.store(savedStart, std::memory_order_relaxed);
    }
    ctx.Current = prev;
    if (background) BackgroundRunning.fetch_sub(1, std::memory_order_acq_rel);

    const int64_t exec = end - start;
    // Время во вложенной задаче учтено и у внешней — занятость не удваиваем.
    if (!prev) slot->BusyNs.fetch_add(exec, std::memory_order_relaxed);
    slot->Executed.fetch_add(1, std::memory_order_relaxed);
    if (stolen) slot->Stolen.fetch_add(1, std::memory_order_relaxed);
    CategoryCounters& c = Cats[(int)job->Cat];
    c.Executed.fetch_add(1, std::memory_order_relaxed);
    c.ExecNs.fetch_add(exec, std::memory_order_relaxed);
    c.QueueNs.fetch_add(std::max<int64_t>(0, start - job->ReadyNs), std::memory_order_relaxed);
    int64_t mx = c.MaxExecNs.load(std::memory_order_relaxed);
    while (exec > mx && !c.MaxExecNs.compare_exchange_weak(mx, exec, std::memory_order_relaxed)) {
    }
    if (Tracing.load(std::memory_order_relaxed)) slot->Trace.Push(job->DisplayName(), job->Cat, start, end);

    FinishCascade(job, status, std::move(error));
}

void Core::RecordError(JobRecord* job) {
    JobError e;
    e.Job = job->DisplayName();
    e.Cat = job->Cat;
    e.AtSeconds = (double)(NowNs() - StartNs) / 1e9;
    std::exception_ptr err;
    {
        std::lock_guard<sync::SpinLock> lk(job->SuccLock);
        err = job->Error;
    }
    try {
        if (err) std::rethrow_exception(err);
        e.Message = "?";
    } catch (const std::exception& ex) {
        e.Message = ex.what();
    } catch (...) {
        e.Message = "неизвестное исключение";
    }
    std::lock_guard<std::mutex> lk(ErrorsM);
    Errors.push_back(std::move(e));
    while (Errors.size() > 64) Errors.pop_front();
}

bool Core::HelpOnce() {
    ThreadCtx& ctx = Ctx();
    ThreadSlot* slot = (ctx.Owner == this) ? ctx.Slot : nullptr;
    // Поток ввода-вывода не берёт кадровую работу на помощь: он для ожиданий.
    if (slot && slot->Kind != Lane::Worker) return false;
    if (!slot && !IsMainThread()) return false;
    JobRecord* j = nullptr;
    bool stolen = false;
    if (slot) j = slot->Deque.Pop();
    if (!j) j = Queues[(int)Lane::Worker]->Pop(Priority::Normal);
    if (!j) { j = StealAny(slot); stolen = j != nullptr; }
    if (!j) j = Queues[(int)Lane::Worker]->PopExactly(Priority::Low);
    if (!j) return false;
    Helped.fetch_add(1, std::memory_order_relaxed);
    Execute(j, slot, stolen);
    return true;
}

int Core::PumpLane(Lane lane, double budgetMs, int maxJobs) {
    LaneQueue& q = *Queues[(int)lane];
    const int64_t deadline = budgetMs > 0.0 ? NowNs() + (int64_t)(budgetMs * 1e6) : 0;
    int done = 0;
    while (maxJobs <= 0 || done < maxJobs) {
        JobRecord* j = q.Pop();
        if (!j) break;
        Execute(j, &MainSlot, false);
        ++done;
        if (deadline && NowNs() >= deadline) break;
    }
    return done;
}

// --- Ожидание ----------------------------------------------------------------

Core* HelpCoreFor(const JobRecord* job) {
    ThreadCtx& ctx = Ctx();
    if (ctx.Owner) return ctx.Owner;
    Core* owner = job ? job->Owner : nullptr;
    if (owner && owner->IsMainThread()) return owner;
    return nullptr;
}

namespace {

// Ожидание общего вида: done() — условие, hubKey — на чём будят, waiters —
// счётчик ждущих (будящий не трогает замок, пока он ноль).
template <class DoneFn>
bool WaitLoop(DoneFn done, const void* hubKey, std::atomic<int>& waiters, Core* help, Lane awaitedLane,
              int64_t deadlineNs) {
    if (done()) return true;
    ThreadCtx& ctx = Ctx();
    const bool onMain = help && !ctx.Owner && help->IsMainThread();
    const int64_t start = NowNs();
    // Долгая задача, которая ждёт, отдаёт свой фоновый слот: иначе фоновые
    // задачи, ждущие друг друга, заняли бы все слоты и встали навсегда.
    const bool releasedBackground =
        ctx.Owner && ctx.Current && ctx.Current->InLane == Lane::Background && ctx.Owner == help;
    if (releasedBackground) ctx.Owner->BackgroundRunning.fetch_sub(1, std::memory_order_acq_rel);

    int idle = 0;
    bool warned = false;
    bool ok = true;
    while (!done()) {
        if (deadlineNs && NowNs() >= deadlineNs) { ok = false; break; }
        if (help && help->HelpOnce()) { idle = 0; continue; }
        if (onMain) {
            // Ждать с главного потока задачу, которая сама ждёт главный поток,
            // — верный deadlock. Если ждём именно задачу главного потока —
            // разбираем его очередь сразу; если ожидание затянулось — тоже
            // (с предупреждением: так строить код не надо).
            const bool mainLane = awaitedLane == Lane::Main || awaitedLane == Lane::Render;
            const bool suspicious = NowNs() - start > 5'000'000;
            if (mainLane || suspicious) {
                if (help->PumpLane(Lane::Main, 0.0, 1) + help->PumpLane(Lane::Render, 0.0, 1) > 0) {
                    if (!mainLane && !warned) {
                        warned = true;
                        LOG_WARN("Jobs") << "Главный поток ждал задачу, пока в его очереди стояла работа; "
                                            "разобрал очередь, чтобы не встать в deadlock";
                    }
                    idle = 0;
                    continue;
                }
            }
        }
        // Нечем помочь — уснуть ненадолго. Будят по завершении; таймаут —
        // страховка и шанс снова поискать работу (у помогающих он короче).
        const int64_t us = help ? std::min<int64_t>(2000, 50LL << std::min(idle, 5)) : 10000;
        ++idle;
        waiters.fetch_add(1, std::memory_order_seq_cst);
        {
            Hub& h = HubFor(hubKey);
            std::unique_lock<std::mutex> lk(h.M);
            if (!done()) h.CV.wait_for(lk, std::chrono::microseconds(us));
        }
        waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    if (releasedBackground) ctx.Owner->BackgroundRunning.fetch_add(1, std::memory_order_acq_rel);
    if (onMain) {
        help->MainWaits.fetch_add(1, std::memory_order_relaxed);
        help->MainWaitNs.fetch_add(NowNs() - start, std::memory_order_relaxed);
    }
    return ok;
}

} // namespace

void Wait(JobRecord* job) {
    if (job->Done()) return;
    WaitLoop([job] { return job->Done(); }, job, job->Waiters, HelpCoreFor(job), job->InLane, 0);
}

bool WaitFor(JobRecord* job, int64_t timeoutNs) {
    if (job->Done()) return true;
    return WaitLoop([job] { return job->Done(); }, job, job->Waiters, HelpCoreFor(job), job->InLane,
                    NowNs() + std::max<int64_t>(0, timeoutNs));
}

namespace {
Core* HelpCoreForGroup(GroupState& g) {
    ThreadCtx& ctx = Ctx();
    if (ctx.Owner) return ctx.Owner;
    Core* c = g.LastCore.load(std::memory_order_acquire);
    if (c && c->IsMainThread()) return c;
    return nullptr;
}
} // namespace

void WaitGroup(GroupState& g) {
    auto done = [&g] { return g.Pending.load(std::memory_order_seq_cst) == 0; };
    if (done()) return;
    WaitLoop(done, &g, g.Waiters, HelpCoreForGroup(g), Lane::Worker, 0);
}

bool WaitGroupFor(GroupState& g, int64_t timeoutNs) {
    auto done = [&g] { return g.Pending.load(std::memory_order_seq_cst) == 0; };
    if (done()) return true;
    return WaitLoop(done, &g, g.Waiters, HelpCoreForGroup(g), Lane::Worker,
                    NowNs() + std::max<int64_t>(0, timeoutNs));
}

} // namespace sage::jobs::detail
