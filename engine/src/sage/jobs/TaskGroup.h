#pragma once
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>

#include "sage/jobs/JobHandle.h"

// ---------------------------------------------------------------------------
// TaskGroup — набор задач с общим ожиданием и общей отменой.
//
// ЗАЧЕМ. Подсистема, запускающая фоновую работу (кэш ресурсов, миниатюры,
// задачи скриптов), обязана перед смертью убедиться, что ни одна её задача не
// тронет её поля после деструктора. Группа отвечает ровно на это:
// `CancelAndWait()` в деструкторе подсистемы — и обращения к уничтоженному
// объекту не бывает по построению, а не по удаче.
//
// Группа не владеет потоками и не держит очередь: задачи идут в общую систему
// с её приоритетами и полосами, группа лишь считает свои.
//
// Деструктор группы ЖДЁТ её задачи (не отменяя): разрушить группу, пока её
// задачи бегут, значит оставить их с данными, которые уже умерли.
// ---------------------------------------------------------------------------
namespace sage::jobs {

namespace detail {
struct GroupState {
    std::atomic<int> Pending{0};
    std::atomic<int> Failures{0};
    std::atomic<int> Waiters{0};
    std::atomic<Core*> LastCore{nullptr};
    CancellationSource Cancel;
    std::mutex ErrorM;
    std::exception_ptr FirstError;
};
void WaitGroup(GroupState& g);
bool WaitGroupFor(GroupState& g, int64_t timeoutNs);
} // namespace detail

class TaskGroup {
public:
    TaskGroup() : m_s(std::make_shared<detail::GroupState>()) {}
    ~TaskGroup() { Wait(); }
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    // Поставить задачу в группу (настройки — как у JobSystem::Async).
    template <class F>
    auto Async(F&& f, JobOptions opts = {}) {
        opts.Group = this;
        return detail::MakeAndSubmit(detail::CoreForSubmit(), std::forward<F>(f), opts);
    }
    template <class F>
    JobHandle Run(F&& f, JobOptions opts = {}) {
        return Async(std::forward<F>(f), std::move(opts));
    }

    // Дождаться всех задач группы (помогая их выполнять).
    void Wait() { detail::WaitGroup(*m_s); }
    bool WaitFor(std::chrono::microseconds timeout) {
        return detail::WaitGroupFor(*m_s, (int64_t)timeout.count() * 1000);
    }
    // Отменить все ещё не начатые и попросить идущие выйти. Группа после
    // отмены остаётся отменённой: новые задачи в ней завершатся, не начавшись.
    void Cancel() { m_s->Cancel.Cancel(); }
    void CancelAndWait() { Cancel(); Wait(); }
    // Снять отмену (группа снова принимает работу), например после перезапуска
    // подсистемы. Зовётся, когда группа уже пуста.
    void Reset() {
        Wait();
        m_s->Cancel = CancellationSource();
        m_s->Failures.store(0);
        std::lock_guard<std::mutex> lk(m_s->ErrorM);
        m_s->FirstError = nullptr;
    }

    bool IsCancelled() const { return m_s->Cancel.IsCancelled(); }
    bool Idle() const { return m_s->Pending.load(std::memory_order_acquire) == 0; }
    int Pending() const { return m_s->Pending.load(std::memory_order_acquire); }
    int Failures() const { return m_s->Failures.load(std::memory_order_acquire); }
    // Первое исключение задач группы (замечается — не уходит в лог потерянным).
    std::exception_ptr FirstError() const {
        std::lock_guard<std::mutex> lk(m_s->ErrorM);
        return m_s->FirstError;
    }
    void RethrowFirstError() const {
        if (std::exception_ptr e = FirstError()) std::rethrow_exception(e);
    }
    CancellationToken Token() const { return m_s->Cancel.Token(); }

    const std::shared_ptr<detail::GroupState>& State() const { return m_s; }

private:
    std::shared_ptr<detail::GroupState> m_s;
};

} // namespace sage::jobs
