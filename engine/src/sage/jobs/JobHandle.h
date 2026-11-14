#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "sage/jobs/JobTypes.h"
#include "sage/jobs/Sync.h"

// ---------------------------------------------------------------------------
// Задача, её ручка (JobHandle), типизированный результат (Future<T>) и ручное
// обещание (Promise<T>).
//
// ВЛАДЕНИЕ. Запись задачи (JobRecord) живёт, пока на неё есть ссылки: у
// планировщика (пока она не завершена), у каждой ручки и у каждой задачи-
// предшественницы (список последователей). Ручку можно бросить — задача всё
// равно выполнится и освободится сама; ручку можно держать сколько угодно
// после завершения — запись не исчезнет из-под неё. Висячей ручки не бывает.
//
// ПРОДОЛЖЕНИЯ. Future<T>::Then(f) — задача, которая начнётся ровно тогда,
// когда готов результат, и получит его ссылкой. Если предшественница упала,
// продолжение не запускается и наследует её исключение: Get() в конце цепочки
// бросит исходную ошибку, а не «что-то где-то отменилось».
// ---------------------------------------------------------------------------
namespace sage::jobs {

class TaskGroup;
class JobHandle;

struct JobCanceledError : std::runtime_error {
    JobCanceledError() : std::runtime_error("задача отменена") {}
};

namespace detail {

struct Core;
struct GroupState;

struct JobRecord {
    virtual ~JobRecord();
    virtual void Run() = 0;
    // Уничтожить тело (замыкание и всё, что оно захватило). Зовётся ДО того,
    // как завершение станет видно ждущим (FinishCascade): дождавшийся задачи
    // или группы вправе считать, что её захваты уже отпущены — подсистема,
    // ждущая свои задачи в деструкторе, не получит их Release после себя.
    virtual void DropBody() {}

    void AddRef() { Refs.fetch_add(1, std::memory_order_relaxed); }
    void Release() {
        if (Refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
    JobStatus GetStatus() const { return (JobStatus)Status.load(std::memory_order_acquire); }
    bool Done() const { return IsFinal(GetStatus()); }
    bool ShouldCancel() const;
    const char* DisplayName() const { return OwnedName.empty() ? Name : OwnedName.c_str(); }

    std::atomic<int> Refs{1};                 // 1 — ссылка планировщика
    std::atomic<uint8_t> Status{(uint8_t)JobStatus::Waiting};
    std::atomic<int> Unfinished{1};           // зависимости + 1 (страж постановки)
    std::atomic<bool> CancelRequested{false};
    std::atomic<bool> DepFailed{false};       // зависимость упала или отменена
    std::atomic<bool> ErrorObserved{false};   // ошибку кто-то прочёл (иначе — в лог)
    std::atomic<bool> InWaitingSet{false};
    std::atomic<int> Waiters{0};

    Lane InLane = Lane::Worker;
    Priority Prio = Priority::Normal;
    Category Cat = Category::General;
    bool RunIfDepFailed = false;
    bool Manual = false;                       // Promise: завершается не планировщиком
    int16_t ThreadIndex = -1;                  // какой поток её выполнил (диагностика)
    const char* Name = "job";
    std::string OwnedName;                     // имя, собранное на лету (система ECS)

    Core* Owner = nullptr;
    std::shared_ptr<GroupState> Group;
    std::shared_ptr<CancelState> Token;
    std::exception_ptr Error;

    sync::SpinLock SuccLock;
    bool SuccClosed = false;                   // задача завершена: новых не принимает
    std::vector<JobRecord*> Successors;        // каждая со своей ссылкой

    int64_t ReadyNs = 0, StartNs = 0, EndNs = 0;
};

// Полный тип для Future<T>: результат лежит прямо в записи — одно выделение
// памяти на задачу вместе с её телом.
template <class T>
struct ValueRecord : JobRecord {
    std::optional<T> Value;
};
template <>
struct ValueRecord<void> : JobRecord {};

template <class T, class F>
struct ValueJob final : ValueRecord<T> {
    explicit ValueJob(F&& f) { Fn.emplace(std::move(f)); }
    void Run() override {
        if constexpr (std::is_void_v<T>) (*Fn)();
        else this->Value.emplace((*Fn)());
    }
    void DropBody() override { Fn.reset(); }
    std::optional<F> Fn;
};

template <class T>
struct PromiseRecord final : ValueRecord<T> {
    void Run() override {}
};

// Интрузивная ссылка на запись.
template <class R>
class Ref {
public:
    Ref() = default;
    explicit Ref(R* r, bool addRef = true) : m_r(r) { if (m_r && addRef) m_r->AddRef(); }
    Ref(const Ref& o) : m_r(o.m_r) { if (m_r) m_r->AddRef(); }
    Ref(Ref&& o) noexcept : m_r(o.m_r) { o.m_r = nullptr; }
    template <class U, class = std::enable_if_t<std::is_convertible_v<U*, R*>>>
    Ref(const Ref<U>& o) : m_r(o.Get()) { if (m_r) m_r->AddRef(); }
    Ref& operator=(Ref o) noexcept { std::swap(m_r, o.m_r); return *this; }
    ~Ref() { if (m_r) m_r->Release(); }
    R* Get() const { return m_r; }
    R* operator->() const { return m_r; }
    explicit operator bool() const { return m_r != nullptr; }

private:
    R* m_r = nullptr;
};

// Реализация — JobScheduling.cpp.
void Submit(Core* core, JobRecord* job, const JobHandle* deps, size_t depCount);
void Wait(JobRecord* job);
bool WaitFor(JobRecord* job, int64_t timeoutNs);
void CompleteManual(JobRecord* job, JobStatus status, std::exception_ptr error);
void RequestCancel(JobRecord* job);
JobRecord* CurrentJob();

} // namespace detail

// --- JobHandle: ручка задачи без значения ----------------------------------
class JobHandle {
public:
    JobHandle() = default;
    explicit JobHandle(detail::Ref<detail::JobRecord> r) : m_r(std::move(r)) {}

    bool Valid() const { return (bool)m_r; }
    // Пустая ручка считается выполненной: «ждать нечего» — не ошибка.
    bool IsDone() const { return !m_r || m_r->Done(); }
    JobStatus Status() const { return m_r ? m_r->GetStatus() : JobStatus::Succeeded; }
    bool Succeeded() const { return Status() == JobStatus::Succeeded; }
    bool Failed() const { return Status() == JobStatus::Failed; }
    bool Canceled() const { return Status() == JobStatus::Canceled; }

    // Дождаться завершения. На воркере и главном потоке ожидание само выполняет
    // чужие кадровые задачи — поток не простаивает и не может «зависнуть»,
    // ожидая работу, которую некому взять. Исключений не бросает: результат —
    // Status() / Rethrow().
    void Wait() const { if (m_r) detail::Wait(m_r.Get()); }
    // false — не дождались за отведённое время.
    bool WaitFor(std::chrono::microseconds timeout) const {
        return !m_r || detail::WaitFor(m_r.Get(), (int64_t)timeout.count() * 1000);
    }
    // Попросить отменить. Ещё не начатая задача завершится как Canceled, не
    // начавшись; идущая увидит запрос через ThisJob::IsCancelled().
    void Cancel() const { if (m_r) detail::RequestCancel(m_r.Get()); }

    // Исключение задачи (или нуль). Чтение отмечает ошибку «замеченной» — она
    // не уйдёт в лог как потерянная.
    std::exception_ptr Error() const {
        if (!m_r) return nullptr;
        m_r->ErrorObserved.store(true, std::memory_order_relaxed);
        return m_r->Error;
    }
    void Rethrow() const {
        if (std::exception_ptr e = Error()) std::rethrow_exception(e);
        if (Canceled()) throw JobCanceledError();
    }
    const char* Name() const { return m_r ? m_r->DisplayName() : ""; }

    detail::JobRecord* Record() const { return m_r.Get(); }
    const detail::Ref<detail::JobRecord>& RecordRef() const { return m_r; }

protected:
    detail::Ref<detail::JobRecord> m_r;
};

// --- Настройки постановки задачи -------------------------------------------
struct JobOptions {
    const char* Name = "job";      // литерал (строка обязана жить вечно)
    std::string DynamicName;       // имя, собранное на лету (копируется в задачу)
    Category Cat = Category::General;
    Lane InLane = Lane::Worker;
    Priority Prio = Priority::Normal;
    std::vector<JobHandle> After;  // начать только после этих
    CancellationToken Token;
    TaskGroup* Group = nullptr;
    // Запускать, даже если зависимость упала/отменена (задача-уборщик).
    bool RunIfDependencyFailed = false;

    JobOptions() = default;
    JobOptions(const char* name, Category cat = Category::General, Lane lane = Lane::Worker,
               Priority prio = Priority::Normal)
        : Name(name), Cat(cat), InLane(lane), Prio(prio) {}

    JobOptions& WithName(const char* n) { Name = n; return *this; }
    JobOptions& WithName(std::string n) { DynamicName = std::move(n); return *this; }
    JobOptions& WithCategory(Category c) { Cat = c; return *this; }
    JobOptions& OnLane(Lane l) { InLane = l; return *this; }
    JobOptions& WithPriority(Priority p) { Prio = p; return *this; }
    JobOptions& DependsOn(JobHandle h) { After.push_back(std::move(h)); return *this; }
    JobOptions& WithToken(CancellationToken t) { Token = std::move(t); return *this; }
    JobOptions& InGroup(TaskGroup& g) { Group = &g; return *this; }
};

namespace detail {
// Заполняет запись из настроек (группа, жетон, имя) — JobScheduling.cpp.
void Prepare(JobRecord* job, const JobOptions& opts);
// Ядро системы для постановки (лениво поднимает систему) — JobSystem.cpp.
Core* CoreForSubmit();
} // namespace detail

// --- Future<T>: результат задачи --------------------------------------------
template <class T>
class Future : public JobHandle {
public:
    Future() = default;
    explicit Future(detail::Ref<detail::ValueRecord<T>> r)
        : JobHandle(detail::Ref<detail::JobRecord>(r)) {}

    // Готов ли результат (без ожидания) — главный поток проверяет так раз в
    // кадр, а не ждёт.
    bool IsReady() const { return IsDone(); }

    // Ждёт и отдаёт значение; бросает исключение задачи или JobCanceledError.
    decltype(auto) Get() const {
        Wait();
        Rethrow();
        if constexpr (!std::is_void_v<T>) return (*Typed()->Value);
    }
    // Забрать значение перемещением (один раз).
    template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
    U Take() const {
        Wait();
        Rethrow();
        return std::move(*Typed()->Value);
    }
    // Значение, если готово и успешно; иначе nullptr. Не ждёт.
    template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
    U* TryGet() const {
        if (!m_r || !m_r->Done() || m_r->GetStatus() != JobStatus::Succeeded) return nullptr;
        return &*Typed()->Value;
    }

    // Продолжение: f(T&) (или f() для void) — после успешного завершения.
    template <class F>
    auto Then(F&& f, JobOptions opts = {}) const;
    // То же на главном потоке (результат — в интерфейс, в сцену, в Lua).
    template <class F>
    auto ThenOnMain(F&& f, JobOptions opts = {}) const {
        opts.InLane = Lane::Main;
        return Then(std::forward<F>(f), std::move(opts));
    }

    detail::ValueRecord<T>* Typed() const { return static_cast<detail::ValueRecord<T>*>(m_r.Get()); }
};

// --- Promise<T>: результат, который выставят руками -------------------------
// Для того, что завершается не задачей: обратный вызов ОС, ответ сети, кадр
// рендера. Брошенное обещание (разрушено без значения) завершает Future
// ошибкой — ждущий не повиснет навсегда.
template <class T>
class Promise {
public:
    Promise() : m_r(new detail::PromiseRecord<T>(), false) {
        m_r->Manual = true;
        m_r->Name = "promise";
        m_r->Status.store((uint8_t)JobStatus::Running, std::memory_order_relaxed);
    }
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    ~Promise() {
        if (m_r && !m_r->Done())
            SetException(std::make_exception_ptr(std::runtime_error("обещание брошено без значения")));
    }

    Future<T> GetFuture() const { return Future<T>(detail::Ref<detail::ValueRecord<T>>(m_r.Get())); }

    template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
    bool SetValue(U value) {
        if (!m_r || m_r->Done()) return false;
        m_r->Value.emplace(std::move(value));
        detail::CompleteManual(m_r.Get(), JobStatus::Succeeded, nullptr);
        return true;
    }
    template <class U = T, class = std::enable_if_t<std::is_void_v<U>>>
    bool SetValue() {
        if (!m_r || m_r->Done()) return false;
        detail::CompleteManual(m_r.Get(), JobStatus::Succeeded, nullptr);
        return true;
    }
    bool SetException(std::exception_ptr e) {
        if (!m_r || m_r->Done()) return false;
        detail::CompleteManual(m_r.Get(), JobStatus::Failed, std::move(e));
        return true;
    }
    bool Cancel() {
        if (!m_r || m_r->Done()) return false;
        detail::CompleteManual(m_r.Get(), JobStatus::Canceled, nullptr);
        return true;
    }

private:
    detail::Ref<detail::PromiseRecord<T>> m_r;
};

// --- Постановка задачи (общая для JobSystem, TaskGroup и продолжений) --------
namespace detail {

template <class F>
using ResultOf = std::invoke_result_t<std::decay_t<F>>;

template <class F>
Future<ResultOf<F>> MakeAndSubmit(Core* core, F&& f, const JobOptions& opts,
                                  const JobHandle* extraDep = nullptr) {
    using R = ResultOf<F>;
    auto* job = new ValueJob<R, std::decay_t<F>>(std::forward<F>(f));
    Prepare(job, opts);
    Future<R> fut(Ref<ValueRecord<R>>(static_cast<ValueRecord<R>*>(job)));
    if (extraDep) {
        std::vector<JobHandle> deps = opts.After;
        deps.push_back(*extraDep);
        Submit(core, job, deps.data(), deps.size());
    } else {
        Submit(core, job, opts.After.data(), opts.After.size());
    }
    return fut;
}

} // namespace detail

template <class T>
template <class F>
auto Future<T>::Then(F&& f, JobOptions opts) const {
    detail::Ref<detail::ValueRecord<T>> parent(Typed());
    auto body = [parent, fn = std::forward<F>(f)]() mutable {
        if constexpr (std::is_void_v<T>) return fn();
        else return fn(*parent->Value);
    };
    if (opts.Cat == Category::General && m_r) opts.Cat = m_r->Cat;
    const JobHandle self = *this;
    return detail::MakeAndSubmit(detail::CoreForSubmit(), std::move(body), opts, &self);
}

// --- Текущая задача -------------------------------------------------------------
struct ThisJob {
    // Долгая задача проверяет это между шагами и выходит, если её отменили
    // (ручкой, группой, жетоном или остановкой системы).
    static bool IsCancelled() {
        detail::JobRecord* j = detail::CurrentJob();
        return j && j->ShouldCancel();
    }
    static bool InsideJob() { return detail::CurrentJob() != nullptr; }
    static const char* Name() {
        detail::JobRecord* j = detail::CurrentJob();
        return j ? j->DisplayName() : "";
    }
};

} // namespace sage::jobs
