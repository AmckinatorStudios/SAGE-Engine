// Модульные тесты JobSystem — многопоточного пула задач движка. Без GL.
// Проверяем: полное покрытие диапазона ParallelFor (каждый индекс ровно раз),
// отсутствие гонок (независимые записи по индексам), корректность при разном
// числе воркеров, последовательный фолбэк при выключенном MT, проброс
// исключений и Dispatch разнородных задач.
#include "TestFramework.h"

#include "sage/core/JobSystem.h"

#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

using sage::JobSystem;

TEST(JobSystem_parallel_for_covers_every_index_once) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(4);

    const std::size_t N = 100000;
    std::vector<int> visits(N, 0);
    js.ParallelFor(N, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) visits[i] += 1; // разные i -> разные ячейки, гонок нет
    }, /*minPerThread*/ 1000);

    // Каждый индекс посещён ровно один раз (нет пропусков/дублей на границах чанков).
    bool allOnce = true;
    for (std::size_t i = 0; i < N; ++i)
        if (visits[i] != 1) { allOnce = false; break; }
    CHECK_TRUE(allOnce);
}

TEST(JobSystem_parallel_for_result_matches_serial) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(4);

    const std::size_t N = 50000;
    std::vector<long long> out(N, 0);
    js.ParallelFor(N, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) out[i] = (long long)i * (long long)i;
    }, 512);

    // Сумма квадратов — сверяем с последовательным эталоном.
    long long parallelSum = std::accumulate(out.begin(), out.end(), 0LL);
    long long expect = 0;
    for (std::size_t i = 0; i < N; ++i) expect += (long long)i * (long long)i;
    CHECK_EQ((int)(parallelSum == expect), 1);
}

TEST(JobSystem_serial_fallback_when_disabled) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(4);
    js.SetEnabled(false); // выключенный MT -> всё на вызывающем потоке

    const std::size_t N = 10000;
    std::atomic<int> total{0};
    js.ParallelFor(N, [&](std::size_t b, std::size_t e) {
        total.fetch_add((int)(e - b));
    }, 256);
    CHECK_EQ(total.load(), (int)N);

    js.SetEnabled(true); // вернуть по умолчанию для остальных тестов
}

TEST(JobSystem_zero_count_is_noop) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(2);
    int calls = 0;
    js.ParallelFor(0, [&](std::size_t, std::size_t) { ++calls; });
    CHECK_EQ(calls, 0);
}

TEST(JobSystem_reinitialize_changes_worker_count) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(2);
    CHECK_EQ(js.WorkerCount(), 2);
    js.Initialize(3);
    CHECK_EQ(js.WorkerCount(), 3);
    CHECK_EQ(js.ThreadCount(), 4); // воркеры + главный поток
}

TEST(JobSystem_exception_propagates) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(4);

    bool caught = false;
    try {
        js.ParallelFor(10000, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                if (i == 9999) throw std::runtime_error("boom");
        }, 256);
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK_TRUE(caught);
}

TEST(JobSystem_dispatch_runs_all_tasks) {
    JobSystem& js = JobSystem::Get();
    js.Initialize(4);

    std::atomic<int> counter{0};
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 8; ++i) tasks.push_back([&] { counter.fetch_add(1); });
    js.Dispatch(tasks);
    CHECK_EQ(counter.load(), 8);
}
