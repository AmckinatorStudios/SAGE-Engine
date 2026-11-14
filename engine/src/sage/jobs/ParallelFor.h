#pragma once
#include <cstddef>
#include <functional>

#include "sage/jobs/JobTypes.h"

// ---------------------------------------------------------------------------
// Параллельный проход без зависимости от системы задач — крючок.
//
// Код, который живёт и в движке, и в маленьких инструментах без него
// (упаковщик шаблонов собирается из пяти файлов движка, без окна и без
// EngineContext), зовёт sage::jobs::ParallelFor. В движке система задач при
// создании подставляет себя исполнителем — проход идёт на воркерах; в
// инструменте исполнителя нет — проход идёт последовательно на вызывающем
// потоке. Результат одинаков: тело обязано трогать только свой отрезок.
// ---------------------------------------------------------------------------
namespace sage::jobs {

struct ParallelOptions {
    std::size_t Grain = 256;      // минимум элементов на кусок
    const char* Name = "parallel_for";
    Category Cat = Category::General;
    Priority Prio = Priority::High;  // кадровая работа ждётся сразу — она срочная
    CancellationToken Token;
};

using ParallelForBackend = void (*)(std::size_t count,
                                    const std::function<void(std::size_t, std::size_t)>& body,
                                    const ParallelOptions& options);
using ParallelismBackend = int (*)();

// Подставляет система задач (JobSystem.cpp); nullptr — последовательно.
void SetParallelBackend(ParallelForBackend forBackend, ParallelismBackend threadsBackend);

void ParallelFor(std::size_t count, const std::function<void(std::size_t, std::size_t)>& body,
                 const ParallelOptions& options = {});
// Сколько потоков реально делят проход (1 — без системы задач).
int Parallelism();

} // namespace sage::jobs
