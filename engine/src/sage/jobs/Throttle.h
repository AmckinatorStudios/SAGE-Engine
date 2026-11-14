#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "sage/jobs/JobSystem.h"

// ---------------------------------------------------------------------------
// Throttle — «не больше N задач этого вида разом» поверх общей системы.
//
// ЗАЧЕМ. Раньше подсистема, которой нужно «два потока на миниатюры» или
// «ядра − 2 на задачи скриптов», заводила СВОЙ пул ровно такого размера. Пул
// давал ограничение, но ценой лишних потоков, которые не знают о кадре и друг
// о друге. Throttle даёт то же ограничение без потоков: задачи ждут в его
// очереди и уходят в общую систему по мере освобождения мест.
//
// N = 1 — последовательная очередь (strand): задачи одного вида идут строго
// по одной и в порядке постановки, без замка у вызывающего.
// ---------------------------------------------------------------------------
namespace sage::jobs {

class Throttle {
public:
    explicit Throttle(int maxConcurrent = 1, JobOptions defaults = {});
    // Ждёт идущие задачи; ещё не начатые — отменяет (их Future скажут Canceled).
    ~Throttle();
    Throttle(const Throttle&) = delete;
    Throttle& operator=(const Throttle&) = delete;

    template <class F>
    auto Async(F&& f) {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto promise = std::make_shared<Promise<R>>();
        Future<R> fut = promise->GetFuture();
        Enqueue([promise, fn = std::forward<F>(f)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    promise->SetValue();
                } else {
                    promise->SetValue(fn());
                }
            } catch (...) {
                promise->SetException(std::current_exception());
            }
        }, [promise] { promise->Cancel(); });
        return fut;
    }

    void SetLimit(int maxConcurrent);
    int Limit() const;
    int Running() const;
    int Queued() const;
    // Отменить ещё не начатые; идущие видят ThisJob::IsCancelled().
    void CancelAll();
    // Дождаться, пока очередь и идущие опустеют.
    void Wait();

private:
    struct State;
    void Enqueue(std::function<void()> run, std::function<void()> cancel);
    std::shared_ptr<State> m_s;
};

} // namespace sage::jobs
