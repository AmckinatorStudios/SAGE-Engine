// Модульные тесты примитивов синхронизации (sage/jobs/Sync.h) и дека с кражей
// работы. Проверяется то, ради чего они существуют: взаимное исключение,
// пределы одновременности, отсутствие потерянных пробуждений, учёт ожиданий
// для профилировщика и «каждый элемент дека достаётся ровно одному».
#include "TestFramework.h"

#include "sage/jobs/Sync.h"
#include "sage/jobs/WorkStealingDeque.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace sync = sage::sync;

TEST(Sync_spinlock_mutual_exclusion) {
    sync::SpinLock lock;
    long long counter = 0;  // не атомик: защищён замком
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 100000; ++i) {
                std::lock_guard<sync::SpinLock> lk(lock);
                ++counter;
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(counter, 400000LL);
}

TEST(Sync_mutex_counts_contention_by_name) {
    sync::ResetLockStats();
    {
        sync::Mutex m("test.contended");
        long long counter = 0;
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                for (int i = 0; i < 20000; ++i) {
                    std::lock_guard<sync::Mutex> lk(m);
                    ++counter;
                }
            });
        for (auto& t : ts) t.join();
        CHECK_EQ(counter, 80000LL);
    }
    // Замок уже разрушен — его числа сохранились в сводке по имени.
    bool found = false;
    for (const sync::LockStats& s : sync::CollectLockStats()) {
        if (s.Name != "test.contended") continue;
        found = true;
        CHECK_EQ(s.Acquisitions, (uint64_t)80000);
        CHECK_TRUE(s.Contended <= s.Acquisitions);
    }
    CHECK_TRUE(found);
}

TEST(Sync_shared_mutex_allows_parallel_readers) {
    sync::SharedMutex m("test.shared");
    std::atomic<int> readers{0}, peak{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            std::shared_lock<sync::SharedMutex> lk(m);
            int c = ++readers;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            std::this_thread::sleep_for(20ms);
            --readers;
        });
    for (auto& t : ts) t.join();
    CHECK_TRUE(peak.load() >= 2);
    // Писатель исключает всех.
    int value = 0;
    std::vector<std::thread> ws;
    for (int t = 0; t < 4; ++t)
        ws.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) {
                std::unique_lock<sync::SharedMutex> lk(m);
                ++value;
            }
        });
    for (auto& t : ws) t.join();
    CHECK_EQ(value, 40000);
}

TEST(Sync_semaphore_limits_concurrency) {
    sync::Semaphore sem(2);
    std::atomic<int> inside{0}, peak{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t)
        ts.emplace_back([&] {
            sem.Acquire();
            int c = ++inside;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            std::this_thread::sleep_for(5ms);
            --inside;
            sem.Release();
        });
    for (auto& t : ts) t.join();
    CHECK_TRUE(peak.load() <= 2);
    CHECK_EQ(sem.Available(), 2);
    CHECK_TRUE(sem.TryAcquire());
    CHECK_TRUE(sem.TryAcquire());
    CHECK_FALSE(sem.TryAcquire());
    CHECK_FALSE(sem.TryAcquireFor(1ms));
}

TEST(Sync_event_manual_and_auto_reset) {
    sync::Event manual(sync::Event::Mode::ManualReset);
    std::atomic<int> woke{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) ts.emplace_back([&] { manual.Wait(); ++woke; });
    std::this_thread::sleep_for(5ms);
    manual.Set();
    for (auto& t : ts) t.join();
    CHECK_EQ(woke.load(), 3);  // ручной — будит всех и остаётся поднятым
    CHECK_TRUE(manual.IsSet());

    sync::Event autoEv(sync::Event::Mode::AutoReset);
    autoEv.Set();
    CHECK_TRUE(autoEv.WaitFor(1ms));
    CHECK_FALSE(autoEv.WaitFor(1ms));  // опустился сам после одного
}

TEST(Sync_event_has_no_lost_wakeup) {
    // Сотни циклов «поднять до/после ожидания» — потерянное пробуждение
    // проявилось бы зависанием (здесь — таймаутом).
    bool allOk = true;
    for (int i = 0; i < 300; ++i) {
        sync::Event ev;
        std::thread t([&] { ev.Set(); });
        allOk &= ev.WaitFor(2s);
        t.join();
    }
    CHECK_TRUE(allOk);
}

TEST(Sync_latch_waits_for_all) {
    sync::Latch latch(5);
    std::vector<std::thread> ts;
    std::atomic<int> before{0};
    for (int t = 0; t < 5; ++t)
        ts.emplace_back([&] {
            ++before;
            latch.CountDown();
        });
    CHECK_TRUE(latch.WaitFor(2s));
    CHECK_EQ(before.load(), 5);
    for (auto& t : ts) t.join();
}

TEST(Sync_barrier_phases_and_completion) {
    constexpr int kThreads = 4, kPhases = 50;
    int merged = 0;  // пишет только завершающий фазу — под замком барьера
    sync::Barrier barrier(kThreads, [&] { ++merged; });
    std::atomic<int> work[kPhases] = {};
    std::atomic<bool> orderOk{true};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&] {
            for (int p = 0; p < kPhases; ++p) {
                ++work[p];
                barrier.ArriveAndWait();
                // После барьера фаза p завершена всеми.
                if (work[p].load() != kThreads) orderOk = false;
            }
        });
    for (auto& t : ts) t.join();
    CHECK_TRUE(orderOk.load());
    CHECK_EQ(merged, kPhases);
    CHECK_EQ((int)barrier.Phase(), kPhases);
}

TEST(Sync_barrier_arrive_and_drop) {
    sync::Barrier barrier(3);
    std::thread quitter([&] { barrier.ArriveAndDrop(); });
    std::thread a([&] { barrier.ArriveAndWait(); });
    barrier.ArriveAndWait();
    a.join();
    quitter.join();
    CHECK_EQ((int)barrier.Phase(), 1);
}

TEST(WorkStealingDeque_owner_lifo_thief_fifo) {
    sage::jobs::detail::WorkStealingDeque<int*> d(4);
    int v[8];
    for (int i = 0; i < 8; ++i) d.Push(&v[i]);  // переполнение кольца — рост
    CHECK_TRUE(d.Steal() == &v[0]);           // вор — самый старый
    CHECK_TRUE(d.Pop() == &v[7]);             // владелец — самый новый
    CHECK_EQ((int)d.SizeApprox(), 6);
}

TEST(WorkStealingDeque_every_item_taken_exactly_once) {
    sage::jobs::detail::WorkStealingDeque<int*> d(64);
    constexpr int kItems = 200000;
    std::vector<int> items(kItems);
    std::vector<std::atomic<int>> taken(kItems);
    std::atomic<bool> done{false};
    auto take = [&](int* p) { taken[(size_t)(p - items.data())]++; };
    std::vector<std::thread> thieves;
    for (int t = 0; t < 3; ++t)
        thieves.emplace_back([&] {
            while (!done.load(std::memory_order_acquire) || !d.EmptyApprox()) {
                if (int* p = d.Steal()) take(p);
            }
        });
    // Владелец кладёт и сам же берёт — спор за последний элемент постоянно.
    for (int i = 0; i < kItems; ++i) {
        d.Push(&items[(size_t)i]);
        if (i % 3 == 0)
            if (int* p = d.Pop()) take(p);
    }
    while (int* p = d.Pop()) take(p);
    done.store(true, std::memory_order_release);
    for (auto& t : thieves) t.join();
    int bad = 0;
    for (auto& c : taken) bad += c.load() != 1 ? 1 : 0;
    CHECK_EQ(bad, 0);
}
