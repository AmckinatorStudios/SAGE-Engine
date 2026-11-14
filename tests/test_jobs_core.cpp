// Модульные тесты единой системы задач: результаты, зависимости, продолжения,
// группы, отмена, исключения, обещания, полосы (главный поток, ввод-вывод,
// долгие задачи), кража работы, остановка. Без GL.
//
// Каждая проверка, которая могла бы повиснуть при ошибке (deadlock), ждёт с
// таймаутом и проверяет, что дождалась, — сломанный планировщик даёт провал
// с именем теста, а не зависший CI.
#include "TestFramework.h"

#include "sage/jobs/JobSystem.h"
#include "sage/jobs/Throttle.h"

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sage::jobs;
namespace sync = sage::sync;
using sage::JobSystem;
using namespace std::chrono_literals;

namespace {

JobSystem& Fresh(int workers = 4) {
    JobSystem& js = JobSystem::Get();
    JobSystemConfig cfg;
    cfg.WorkerThreads = workers;
    cfg.IoThreads = 2;
    js.Shutdown();  // чистый лист: свои потоки и счётчики на тест
    js.Initialize(cfg);
    return js;
}

void SpinFor(std::chrono::microseconds d) {
    const auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

} // namespace

TEST(Jobs_async_returns_value) {
    JobSystem& js = Fresh();
    Future<int> f = js.Async([] { return 6 * 7; });
    CHECK_TRUE(f.WaitFor(5s));
    CHECK_EQ(f.Get(), 42);
    CHECK_TRUE(f.Succeeded());
    // Take забирает перемещением.
    Future<std::string> s = js.Async([] { return std::string("сага"); });
    CHECK_EQ(s.Take(), std::string("сага"));
}

TEST(Jobs_void_job_and_empty_handle) {
    JobSystem& js = Fresh();
    std::atomic<int> hit{0};
    JobHandle h = js.Run([&] { hit = 1; });
    h.Wait();
    CHECK_EQ(hit.load(), 1);
    JobHandle empty;
    CHECK_TRUE(empty.IsDone());  // ждать нечего — не ошибка
    empty.Wait();
}

TEST(Jobs_dependencies_run_in_order) {
    JobSystem& js = Fresh();
    std::atomic<int> step{0};
    std::atomic<bool> orderOk{true};
    JobHandle a = js.Run([&] {
        SpinFor(2ms);
        if (step.exchange(1) != 0) orderOk = false;
    });
    JobHandle b = js.Run([&] {
        SpinFor(1ms);
        if (step.load() < 1) orderOk = false;
        step = 2;
    }, JobOptions().DependsOn(a));
    JobHandle c = js.Run([&] {
        if (step.load() != 2) orderOk = false;
        step = 3;
    }, JobOptions().DependsOn(a).DependsOn(b));
    CHECK_TRUE(c.WaitFor(5s));
    CHECK_TRUE(orderOk.load());
    CHECK_EQ(step.load(), 3);
}

TEST(Jobs_diamond_graph_joins_once) {
    JobSystem& js = Fresh();
    std::atomic<int> joined{0};
    std::atomic<int> sum{0};
    JobHandle root = js.Run([&] { sum += 1; });
    std::vector<JobHandle> mids;
    for (int i = 0; i < 16; ++i) mids.push_back(js.Run([&] { sum += 10; }, JobOptions().DependsOn(root)));
    JobHandle join = js.WhenAll(mids);
    JobHandle tail = js.Run([&] { joined++; }, JobOptions().DependsOn(join));
    CHECK_TRUE(tail.WaitFor(5s));
    CHECK_EQ(sum.load(), 161);
    CHECK_EQ(joined.load(), 1);
}

TEST(Jobs_dependency_on_finished_job_starts_immediately) {
    JobSystem& js = Fresh();
    JobHandle done = js.Run([] {});
    done.Wait();
    Future<int> f = js.Async([] { return 5; }, JobOptions().DependsOn(done));
    CHECK_TRUE(f.WaitFor(5s));
    CHECK_EQ(f.Get(), 5);
}

TEST(Jobs_continuation_chain_passes_values) {
    JobSystem& js = Fresh();
    Future<int> f = js.Async([] { return 2; })
                        .Then([](int& v) { return v * 10; })
                        .Then([](int& v) { return std::to_string(v + 1); })
                        .Then([](std::string& s) { return (int)s.size(); });
    CHECK_TRUE(f.WaitFor(5s));
    CHECK_EQ(f.Get(), 2);  // "21"
}

TEST(Jobs_failure_propagates_through_continuations) {
    JobSystem& js = Fresh();
    std::atomic<bool> ranContinuation{false};
    Future<int> f = js.Async([]() -> int { throw std::runtime_error("ошибка разбора"); })
                        .Then([&](int& v) { ranContinuation = true; return v; });
    CHECK_TRUE(f.WaitFor(5s));
    CHECK_TRUE(f.Failed());
    CHECK_FALSE(ranContinuation.load());
    bool caught = false;
    try {
        f.Get();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "ошибка разбора";
    }
    CHECK_TRUE(caught);
}

TEST(Jobs_cleanup_job_runs_even_if_dependency_failed) {
    JobSystem& js = Fresh();
    JobHandle bad = js.Run([] { throw std::runtime_error("x"); });
    std::atomic<bool> cleaned{false};
    JobOptions o;
    o.After.push_back(bad);
    o.RunIfDependencyFailed = true;
    JobHandle cleanup = js.Run([&] { cleaned = true; }, o);
    CHECK_TRUE(cleanup.WaitFor(5s));
    CHECK_TRUE(cleaned.load());
    CHECK_TRUE(cleanup.Succeeded());
    (void)bad.Error();  // замечено — не уйдёт в лог потерянным
}

TEST(Jobs_exception_is_stored_not_lost) {
    JobSystem& js = Fresh();
    JobHandle h = js.Run([] { throw std::logic_error("упало"); });
    h.Wait();
    CHECK_TRUE(h.Failed());
    CHECK_TRUE(h.Error() != nullptr);
    bool rethrown = false;
    try {
        h.Rethrow();
    } catch (const std::logic_error&) {
        rethrown = true;
    }
    CHECK_TRUE(rethrown);
    // Ошибка попала в диагностику.
    bool inStats = false;
    for (const JobError& e : js.Stats().Errors) inStats |= e.Message == "упало";
    CHECK_TRUE(inStats);
}

TEST(Jobs_cancel_before_start) {
    JobSystem& js = Fresh(1);
    // Единственный воркер занят — следующая задача гарантированно ждёт в очереди.
    sync::Event release;
    JobHandle blocker = js.Run([&] { release.WaitFor(5s); });
    std::atomic<bool> ran{false};
    JobHandle victim = js.Run([&] { ran = true; }, JobOptions().WithPriority(Priority::Low));
    victim.Cancel();
    release.Set();
    CHECK_TRUE(victim.WaitFor(5s));
    CHECK_TRUE(blocker.WaitFor(5s));
    CHECK_TRUE(victim.Canceled());
    CHECK_FALSE(ran.load());
}

TEST(Jobs_cancellation_token_and_cooperative_exit) {
    JobSystem& js = Fresh();
    CancellationSource src;
    std::atomic<int> iterations{0};
    std::atomic<bool> started{false};
    JobHandle h = js.Run([&] {
        started = true;
        while (!ThisJob::IsCancelled()) {
            ++iterations;
            std::this_thread::sleep_for(100us);
        }
    }, JobOptions().WithToken(src.Token()).OnLane(Lane::Background));
    while (!started.load()) std::this_thread::yield();
    src.Cancel();
    CHECK_TRUE(h.WaitFor(5s));
    CHECK_TRUE(iterations.load() >= 0);
    // Задача вышла сама — это успешное завершение, а не отмена до старта.
    CHECK_TRUE(h.Succeeded());
}

TEST(Jobs_canceled_dependency_cancels_dependents) {
    JobSystem& js = Fresh(1);
    sync::Event release;
    JobHandle blocker = js.Run([&] { release.WaitFor(5s); });
    JobHandle a = js.Run([] {}, JobOptions().WithPriority(Priority::Low));
    std::atomic<bool> ran{false};
    JobHandle b = js.Run([&] { ran = true; }, JobOptions().DependsOn(a));
    a.Cancel();
    release.Set();
    CHECK_TRUE(b.WaitFor(5s));
    CHECK_TRUE(a.Canceled());
    CHECK_TRUE(b.Canceled());
    CHECK_FALSE(ran.load());
    blocker.Wait();
}

TEST(Jobs_long_cancel_cascade_does_not_overflow_stack) {
    JobSystem& js = Fresh(1);
    Promise<void> gate;
    JobHandle prev = gate.GetFuture();
    std::vector<JobHandle> chain;
    for (int i = 0; i < 20000; ++i) {
        prev = js.Run([] {}, JobOptions().DependsOn(prev));
        if (i % 1000 == 0) chain.push_back(prev);
    }
    gate.Cancel();  // одна отмена — двадцать тысяч отменённых без рекурсии
    CHECK_TRUE(prev.WaitFor(10s));
    CHECK_TRUE(prev.Canceled());
}

TEST(Jobs_promise_completes_future) {
    JobSystem& js = Fresh();
    Promise<int> p;
    Future<int> f = p.GetFuture();
    Future<int> twice = f.Then([](int& v) { return v * 2; });
    CHECK_FALSE(f.IsReady());
    std::thread t([&] {
        std::this_thread::sleep_for(2ms);
        p.SetValue(21);
    });
    CHECK_TRUE(twice.WaitFor(5s));
    t.join();
    CHECK_EQ(twice.Get(), 42);
    CHECK_FALSE(p.SetValue(1));  // второй раз — нельзя
}

TEST(Jobs_broken_promise_fails_waiters) {
    Fresh();
    Future<int> f;
    {
        Promise<int> p;
        f = p.GetFuture();
    }
    CHECK_TRUE(f.IsReady());
    CHECK_TRUE(f.Failed());
    (void)f.Error();
}

TEST(Jobs_task_group_waits_all_and_collects_errors) {
    Fresh();
    TaskGroup g;
    std::atomic<int> done{0};
    for (int i = 0; i < 200; ++i)
        g.Run([&, i] {
            if (i == 77) throw std::runtime_error("седьмой");
            ++done;
        });
    CHECK_TRUE(g.WaitFor(5s));
    CHECK_EQ(done.load(), 199);
    CHECK_EQ(g.Failures(), 1);
    CHECK_TRUE(g.FirstError() != nullptr);
    CHECK_TRUE(g.Idle());
}

TEST(Jobs_task_group_cancel_and_wait) {
    Fresh(1);
    TaskGroup g;
    sync::Event release;
    std::atomic<int> ran{0};
    g.Run([&] { release.WaitFor(5s); });
    for (int i = 0; i < 50; ++i) g.Run([&] { ++ran; }, JobOptions().WithPriority(Priority::Low));
    g.Cancel();
    release.Set();
    CHECK_TRUE(g.WaitFor(5s));
    CHECK_EQ(ran.load(), 0);
    // Отменённая группа не принимает новую работу, пока её не сбросят.
    JobHandle late = g.Run([&] { ++ran; });
    late.Wait();
    CHECK_TRUE(late.Canceled());
    g.Reset();
    g.Run([&] { ++ran; }).Wait();
    CHECK_EQ(ran.load(), 1);
}

TEST(Jobs_group_destructor_waits_for_running_jobs) {
    Fresh();
    auto data = std::make_unique<std::vector<int>>(1000, 0);
    {
        TaskGroup g;
        for (int i = 0; i < 1000; ++i)
            g.Run([&, i] {
                SpinFor(5us);
                (*data)[(size_t)i] = 1;
            });
        // Деструктор группы ждёт — ни одна задача не тронет data после него.
    }
    int sum = 0;
    for (int v : *data) sum += v;
    CHECK_EQ(sum, 1000);
    data.reset();
}

TEST(Jobs_main_thread_lane_runs_only_on_pump) {
    JobSystem& js = Fresh();
    const auto mainId = std::this_thread::get_id();
    std::atomic<bool> onMain{false};
    JobHandle h = js.Run([&] { onMain = std::this_thread::get_id() == mainId; },
                         JobOptions("ui", Category::Editor, Lane::Main));
    std::this_thread::sleep_for(5ms);
    CHECK_FALSE(h.IsDone());  // без PumpMainThread задача главного потока не идёт
    CHECK_EQ(js.PumpMainThread(), 1);
    CHECK_TRUE(h.IsDone());
    CHECK_TRUE(onMain.load());
}

TEST(Jobs_worker_result_returns_to_main_thread) {
    JobSystem& js = Fresh();
    const auto mainId = std::this_thread::get_id();
    std::atomic<bool> computedOffMain{false};
    std::atomic<bool> appliedOnMain{false};
    int applied = 0;  // «данные интерфейса»: трогать только с главного потока
    // Ввод-вывод: главный поток не берёт его на помощь, поэтому счёт заведомо
    // идёт не на нём (кадровую задачу ждущий главный поток мог бы взять сам).
    Future<int> f = js.Async([&] {
        computedOffMain = std::this_thread::get_id() != mainId;
        return 99;
    }, JobOptions().OnLane(Lane::IO));
    JobHandle apply = f.ThenOnMain([&](int& v) {
        appliedOnMain = std::this_thread::get_id() == mainId;
        applied = v;
    });
    // Ожидание задачи главного потока с главного потока само разбирает его
    // очередь (иначе это был бы deadlock).
    CHECK_TRUE(apply.WaitFor(5s));
    CHECK_TRUE(computedOffMain.load());
    CHECK_TRUE(appliedOnMain.load());
    CHECK_EQ(applied, 99);
}

TEST(Jobs_pump_budget_limits_work_per_frame) {
    JobSystem& js = Fresh();
    std::vector<JobHandle> hs;
    for (int i = 0; i < 20; ++i)
        hs.push_back(js.Run([] { SpinFor(1ms); }, JobOptions("slow", Category::General, Lane::Main)));
    const int first = js.PumpMainThread(2.5);
    CHECK_TRUE(first >= 1 && first < 20);
    int total = first;
    while (total < 20) total += js.PumpMainThread();
    CHECK_EQ(total, 20);
}

TEST(Jobs_io_lane_runs_off_workers_and_grows) {
    JobSystemConfig cfg;
    cfg.WorkerThreads = 2;
    cfg.IoThreads = 1;
    cfg.MaxIoThreads = 4;
    JobSystem& js = JobSystem::Get();
    js.Shutdown();
    js.Initialize(cfg);
    // Четыре «ожидания диска» разом: один поток ввода-вывода занят — пул
    // растёт до четырёх, и все четыре идут одновременно.
    std::atomic<int> concurrent{0}, peak{0};
    std::vector<JobHandle> hs;
    for (int i = 0; i < 4; ++i)
        hs.push_back(js.Run([&] {
            int c = ++concurrent;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            std::this_thread::sleep_for(30ms);
            --concurrent;
        }, JobOptions("read", Category::IO, Lane::IO)));
    for (auto& h : hs) CHECK_TRUE(h.WaitFor(5s));
    CHECK_TRUE(peak.load() >= 2);
    CHECK_TRUE(js.IoThreadCount() <= 4);
}

TEST(Jobs_background_limit_keeps_workers_for_frame) {
    JobSystemConfig cfg;
    cfg.WorkerThreads = 4;
    cfg.BackgroundLimit = 2;
    JobSystem& js = JobSystem::Get();
    js.Shutdown();
    js.Initialize(cfg);
    std::atomic<int> concurrent{0}, peak{0};
    sync::Event release;
    std::vector<JobHandle> bg;
    for (int i = 0; i < 6; ++i)
        bg.push_back(js.Run([&] {
            int c = ++concurrent;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            release.WaitFor(2s);
            --concurrent;
        }, JobOptions("bake", Category::General, Lane::Background)));
    std::this_thread::sleep_for(20ms);
    // Пока долгие задачи держат свои слоты, кадровая работа идёт на остальных.
    std::atomic<int> frame{0};
    js.ParallelFor(64, [&](size_t b, size_t e) { frame += (int)(e - b); }, 1);
    CHECK_EQ(frame.load(), 64);
    release.Set();
    for (auto& h : bg) CHECK_TRUE(h.WaitFor(5s));
    CHECK_TRUE(peak.load() <= 2);
    CHECK_TRUE(peak.load() >= 1);
}

TEST(Jobs_low_priority_is_not_starved) {
    JobSystem& js = Fresh(2);
    std::atomic<bool> lowRan{false};
    std::atomic<bool> stop{false};
    JobHandle low = js.Run([&] { lowRan = true; }, JobOptions().WithPriority(Priority::Low));
    // Непрерывный поток срочной работы.
    std::thread feeder([&] {
        while (!stop.load()) {
            js.Run([] { SpinFor(20us); }, JobOptions().WithPriority(Priority::High));
            std::this_thread::sleep_for(10us);
        }
    });
    const bool ok = low.WaitFor(5s);
    stop = true;
    feeder.join();
    CHECK_TRUE(ok);
    CHECK_TRUE(lowRan.load());
}

TEST(Jobs_work_stealing_spreads_nested_work) {
    JobSystem& js = Fresh(4);
    js.ResetStats();
    // Одна задача порождает много подзадач в СВОЙ дек — остальные воркеры
    // могут получить их только кражей.
    std::mutex m;
    std::set<std::thread::id> threads;
    JobHandle root = js.Run([&] {
        TaskGroup inner;
        for (int i = 0; i < 400; ++i)
            inner.Run([&] {
                SpinFor(50us);
                std::lock_guard<std::mutex> lk(m);
                threads.insert(std::this_thread::get_id());
            });
        inner.Wait();
    }, JobOptions().OnLane(Lane::Background));  // корень — заведомо на воркере
    CHECK_TRUE(root.WaitFor(10s));
    const JobSystemStats st = js.Stats();
    CHECK_TRUE(st.Steals > 0);
    CHECK_TRUE(threads.size() >= 2);
}

TEST(Jobs_nested_parallel_for_with_one_worker_does_not_deadlock) {
    JobSystem& js = Fresh(1);
    std::atomic<long long> sum{0};
    JobHandle h = js.Run([&] {
        js.ParallelFor(64, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i)
                js.ParallelFor(64, [&](size_t b2, size_t e2) { sum += (long long)(e2 - b2); }, 4);
        }, 4);
    });
    CHECK_TRUE(h.WaitFor(10s));
    CHECK_EQ(sum.load(), 64LL * 64LL);
}

TEST(Jobs_waiting_job_helps_instead_of_blocking) {
    JobSystem& js = Fresh(2);
    // Каждый воркер ждёт подзадачу, которая стоит в очереди за ним. Без помощи
    // при ожидании оба воркера уснули бы навсегда.
    std::vector<JobHandle> outer;
    std::atomic<int> innerDone{0};
    for (int i = 0; i < 8; ++i)
        outer.push_back(js.Run([&] {
            JobHandle inner = js.Run([&] { ++innerDone; });
            inner.Wait();
        }));
    for (auto& h : outer) CHECK_TRUE(h.WaitFor(10s));
    CHECK_EQ(innerDone.load(), 8);
}

TEST(Jobs_background_jobs_waiting_on_each_other_do_not_deadlock) {
    JobSystemConfig cfg;
    cfg.WorkerThreads = 4;
    cfg.BackgroundLimit = 1;
    JobSystem& js = JobSystem::Get();
    js.Shutdown();
    js.Initialize(cfg);
    // Единственный фоновый слот занят задачей, ждущей другую фоновую.
    Future<int> outer = js.Async([&] {
        Future<int> inner = js.Async([] { return 7; }, JobOptions().OnLane(Lane::Background));
        return inner.Get() + 1;
    }, JobOptions().OnLane(Lane::Background));
    CHECK_TRUE(outer.WaitFor(10s));
    CHECK_EQ(outer.Get(), 8);
}

TEST(Jobs_shutdown_cancels_pending_and_rejects_new) {
    JobSystem js;  // своя система: остановка не трогает общую
    JobSystemConfig cfg;
    cfg.WorkerThreads = 1;
    js.Initialize(cfg);
    sync::Event started;
    std::atomic<bool> sawCancel{false};
    JobHandle running = js.Run([&] {
        started.Set();
        while (!ThisJob::IsCancelled()) std::this_thread::sleep_for(100us);
        sawCancel = true;
    });
    started.WaitFor(5s);
    std::vector<JobHandle> queued;
    for (int i = 0; i < 100; ++i) queued.push_back(js.Run([] {}, JobOptions().WithPriority(Priority::Low)));
    Promise<void> never;
    JobHandle waiting = js.Run([] {}, JobOptions().DependsOn(never.GetFuture()));
    JobHandle mainJob = js.Run([] {}, JobOptions("ui", Category::Editor, Lane::Main));
    js.Shutdown();
    CHECK_TRUE(sawCancel.load());  // идущая увидела запрос остановки
    CHECK_TRUE(running.IsDone());
    int canceled = 0;
    for (auto& h : queued) canceled += h.Canceled() ? 1 : 0;
    CHECK_TRUE(canceled > 0);
    for (auto& h : queued) CHECK_TRUE(h.IsDone());
    CHECK_TRUE(waiting.Canceled());  // висящая на обещании — отменена, не забыта
    CHECK_TRUE(mainJob.Canceled());
    // После остановки — отказ, а не вечное ожидание.
    JobHandle late = js.Run([] {});
    CHECK_TRUE(late.IsDone());
    CHECK_TRUE(late.Canceled());
    // ParallelFor после остановки — последовательно на вызывающем.
    int sum = 0;
    js.ParallelFor(10, [&](size_t b, size_t e) { sum += (int)(e - b); }, 1);
    CHECK_EQ(sum, 10);
    never.Cancel();
}

TEST(Jobs_shutdown_is_idempotent_and_reinit_works) {
    JobSystem js;
    js.Initialize(2);
    js.Shutdown();
    js.Shutdown();
    js.Initialize(3);
    CHECK_EQ(js.WorkerCount(), 3);
    CHECK_EQ(js.Async([] { return 1; }).Get(), 1);
}

TEST(Jobs_batch_runs_every_item_once) {
    JobSystem& js = Fresh();
    std::vector<std::atomic<int>> hits(5000);
    std::vector<std::function<void()>> items;
    for (size_t i = 0; i < hits.size(); ++i) items.push_back([&hits, i] { hits[i]++; });
    JobHandle gate = js.Run([] { SpinFor(1ms); });
    JobHandle b = js.Batch(std::move(items), JobOptions("batch").DependsOn(gate));
    CHECK_TRUE(b.WaitFor(5s));
    bool once = true;
    for (auto& h : hits) once &= h.load() == 1;
    CHECK_TRUE(once);
}

TEST(Jobs_parallel_for_async_reports_errors) {
    JobSystem& js = Fresh();
    std::atomic<int> n{0};
    JobHandle ok = js.ParallelForAsync(1000, [&](size_t b, size_t e) { n += (int)(e - b); });
    CHECK_TRUE(ok.WaitFor(5s));
    CHECK_EQ(n.load(), 1000);
    ParallelOptions po;
    po.Grain = 10;
    JobHandle bad = js.ParallelForAsync(100, [](size_t b, size_t) {
        if (b == 50) throw std::runtime_error("кусок");
    }, po);
    CHECK_TRUE(bad.WaitFor(5s));
    CHECK_TRUE(bad.Failed());
    (void)bad.Error();
}

TEST(Jobs_throttle_limits_concurrency_and_keeps_order) {
    Fresh(4);
    Throttle serial(1);
    std::vector<int> order;
    std::mutex m;
    std::atomic<int> concurrent{0}, peak{0};
    std::vector<Future<int>> fs;
    for (int i = 0; i < 50; ++i)
        fs.push_back(serial.Async([&, i] {
            int c = ++concurrent;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            {
                std::lock_guard<std::mutex> lk(m);
                order.push_back(i);
            }
            --concurrent;
            return i;
        }));
    serial.Wait();
    CHECK_EQ(peak.load(), 1);
    bool ordered = order.size() == 50;
    for (size_t i = 0; ordered && i < order.size(); ++i) ordered = order[i] == (int)i;
    CHECK_TRUE(ordered);
    CHECK_EQ(fs[49].Get(), 49);

    Throttle two(2);
    concurrent = 0;
    peak = 0;
    for (int i = 0; i < 20; ++i)
        two.Async([&] {
            int c = ++concurrent;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {
            }
            SpinFor(300us);
            --concurrent;
        });
    two.Wait();
    CHECK_TRUE(peak.load() <= 2);
}

TEST(Jobs_throttle_cancel_all_cancels_futures) {
    Fresh(1);
    Throttle t(1);
    sync::Event release;
    Future<void> first = t.Async([&] { release.WaitFor(5s); });
    Future<int> second = t.Async([] { return 1; });
    t.CancelAll();
    release.Set();
    t.Wait();
    CHECK_TRUE(first.Succeeded());
    CHECK_TRUE(second.Canceled());
}

TEST(Jobs_render_thread_check) {
    JobSystem& js = Fresh();
    CHECK_TRUE(js.IsMainThread());
    CHECK_TRUE(js.IsRenderThread());
    CHECK_TRUE(js.CheckRenderThread("тест"));
    std::atomic<bool> fromWorker{true};
    js.Run([&] { fromWorker = js.CheckRenderThread("тест не из потока рендера"); },
           JobOptions().OnLane(Lane::IO)).Wait();
    CHECK_FALSE(fromWorker.load());
    // Задача рендера идёт только при разборе его очереди.
    std::atomic<bool> uploaded{false};
    JobHandle up = PostToRender([&] { uploaded = js.IsRenderThread(); });
    CHECK_EQ(js.PumpRenderThread(), 1);
    CHECK_TRUE(up.IsDone());
    CHECK_TRUE(uploaded.load());
}

TEST(Jobs_no_job_runs_twice_under_stress) {
    JobSystem& js = Fresh(4);
    js.ResetStats();
    constexpr int kJobs = 50000;
    std::vector<std::atomic<int>> runs(kJobs);
    TaskGroup g;
    // Постановка из нескольких потоков разом + вложенная постановка из задач.
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p)
        producers.emplace_back([&, p] {
            for (int i = p; i < kJobs; i += 8) g.Run([&, i] { runs[(size_t)i]++; });
        });
    for (int i = 4; i < kJobs; i += 8)
        g.Run([&, i] {
            for (int k = 0; k < 4 && i + k < kJobs; ++k) {
                const int idx = i + k;
                if (idx % 8 >= 4) runs[(size_t)idx]++;
            }
        });
    for (auto& t : producers) t.join();
    CHECK_TRUE(g.WaitFor(30s));
    int bad = 0;
    for (auto& r : runs) bad += r.load() != 1 ? 1 : 0;
    CHECK_EQ(bad, 0);
    const JobSystemStats st = js.Stats();
    CHECK_EQ(st.DuplicateRunsPrevented, (uint64_t)0);
    CHECK_TRUE(st.Completed >= (uint64_t)(kJobs / 2));
}
