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

// --- Планировщик систем -------------------------------------------------------
//
// Проверяется главное свойство: ПОРЯДОК не зависит от порядка регистрации.
// Ради него планировщик и заводился — руками записанный порядок кадра успел
// разойтись между рантаймом, редактором и игрой, и в игре скрипты выполнялись
// после физики.

#include "sage/core/SystemScheduler.h"
#include "sage/scene/Scene.h"

TEST(Scheduler_runs_stages_in_order_regardless_of_registration) {
    sage::SystemScheduler s;
    std::vector<std::string> log;

    // Регистрируем в ЗАВЕДОМО неправильном порядке — от последней стадии к первой.
    s.Add(sage::Stage::LateUpdate, "ui", [&](Scene&, float) { log.push_back("ui"); });
    s.Add(sage::Stage::Effects, "particles", [&](Scene&, float) { log.push_back("particles"); });
    s.Add(sage::Stage::Physics, "physics", [&](Scene&, float) { log.push_back("physics"); });
    s.Add(sage::Stage::Update, "scripts", [&](Scene&, float) { log.push_back("scripts"); });
    s.Add(sage::Stage::PreUpdate, "input", [&](Scene&, float) { log.push_back("input"); });

    Scene scene("SchedScene");
    s.Run(scene, 1.0f / 60.0f);

    CHECK_EQ((int)log.size(), 5);
    CHECK_TRUE(log[0] == "input");
    CHECK_TRUE(log[1] == "scripts");
    CHECK_TRUE(log[2] == "physics");   // скрипты СТРОГО раньше физики
    CHECK_TRUE(log[3] == "particles");
    CHECK_TRUE(log[4] == "ui");
}

TEST(Scheduler_orders_within_a_stage_and_keeps_registration_order_on_ties) {
    sage::SystemScheduler s;
    std::vector<std::string> log;
    s.Add(sage::Stage::Effects, "audio", [&](Scene&, float) { log.push_back("audio"); }, 10);
    s.Add(sage::Stage::Effects, "fx", [&](Scene&, float) { log.push_back("fx"); }, 0);
    // Две системы с одинаковым порядком: обязаны идти как зарегистрированы.
    // Неустойчивая сортировка дала бы кадр, зависящий от реализации, — то есть
    // невоспроизводимый.
    s.Add(sage::Stage::Effects, "a", [&](Scene&, float) { log.push_back("a"); }, 5);
    s.Add(sage::Stage::Effects, "b", [&](Scene&, float) { log.push_back("b"); }, 5);

    Scene scene("TieScene");
    s.Run(scene, 0.016f);
    CHECK_TRUE(log[0] == "fx");
    CHECK_TRUE(log[1] == "a");
    CHECK_TRUE(log[2] == "b");
    CHECK_TRUE(log[3] == "audio");
}

TEST(Scheduler_replaces_by_name_instead_of_duplicating) {
    // Перезапуск Play переподключает скрипты и физику. Если бы регистрация
    // добавляла вторую систему вместо замены, каждый заход в Play удваивал бы
    // шаг физики — и мир начал бы идти вдвое быстрее.
    sage::SystemScheduler s;
    int calls = 0;
    s.Add(sage::Stage::Update, "scripts", [&](Scene&, float) { ++calls; });
    s.Add(sage::Stage::Update, "scripts", [&](Scene&, float) { ++calls; });
    CHECK_EQ(s.Count(), 1);

    Scene scene("DupScene");
    s.Run(scene, 0.016f);
    CHECK_EQ(calls, 1);

    CHECK_TRUE(s.Remove("scripts"));
    CHECK_FALSE(s.Remove("scripts"));
    CHECK_EQ(s.Count(), 0);
}

TEST(Scheduler_survives_a_throwing_system) {
    // Упавший скрипт не должен уносить с собой физику и рендер: одна опечатка
    // в Lua обязана стоить строки в консоли, а не чёрного экрана.
    sage::SystemScheduler s;
    bool after = false;
    s.Add(sage::Stage::Update, "bad", [](Scene&, float) { throw std::runtime_error("бум"); });
    s.Add(sage::Stage::Physics, "good", [&](Scene&, float) { after = true; });

    Scene scene("ThrowScene");
    s.Run(scene, 0.016f);
    CHECK_TRUE(after);
}

TEST(Scheduler_measures_each_system) {
    // Профайлер движка знает только «Обновление» целиком. Здесь видно, КАКАЯ
    // система съела кадр, — а это и есть первый вопрос при просадке.
    sage::SystemScheduler s;
    s.Add(sage::Stage::Update, "cheap", [](Scene&, float) {});
    s.Add(sage::Stage::Physics, "busy", [](Scene&, float) {
        volatile double x = 0.0;
        for (int i = 0; i < 200000; ++i) x += i * 0.5;
    });
    Scene scene("TimeScene");
    s.Run(scene, 0.016f);

    const auto& t = s.LastTimings();
    CHECK_EQ((int)t.size(), 2);
    CHECK_TRUE(t[0].Name == "cheap");
    CHECK_TRUE(t[1].Name == "busy");
    std::printf("       cheap %.3f мс, busy %.3f мс\n", t[0].Milliseconds, t[1].Milliseconds);
    CHECK_TRUE(t[1].Milliseconds >= t[0].Milliseconds);
}

// --- Профилировщик даёт вложенную разбивку ------------------------------------
//
// Механизм замеров существовал целиком — и не вызывался НИ РАЗУ: ни одного
// SetEnabled(true), ни одного читателя результата. Тест закрепляет то, ради
// чего он написан: участки складываются в дерево, а бэкенд без таймеров GPU не
// мешает мерить CPU.
#include "sage/core/Profiler.h"
#include "sage/rhi/GraphicsDevice.h"

TEST(Profiler_builds_a_nested_breakdown_without_gpu_timers) {
    // Null-бэкенд таймеров не умеет — ровно тот случай, который обязан работать.
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    sage::rhi::GraphicsDevice::SetCurrent(device.get());
    sage::profile::SetEnabled(true);

    // Три кадра: результат созревает с задержкой, и первый кадр пуст ПО ЗАМЫСЛУ.
    for (int i = 0; i < 6; ++i) {
        sage::profile::BeginFrame();
        {
            sage::profile::Scope outer("Отрисовка");
            { sage::profile::Scope inner("Тени"); }
            { sage::profile::Scope inner("Геометрия"); }
        }
        sage::profile::EndFrame();
    }

    const std::vector<sage::profile::Entry>& frame = sage::profile::Frame();
    CHECK_EQ((int)frame.size(), 3);
    CHECK_TRUE(frame[0].Name == "Отрисовка");
    CHECK_EQ(frame[0].Depth, 0);
    CHECK_TRUE(frame[1].Name == "Тени");
    CHECK_EQ(frame[1].Depth, 1);   // вложенность — то, ради чего метки, а не интервалы
    CHECK_EQ(frame[2].Depth, 1);
    // Без таймеров GPU время видеокарты честно нулевое, а CPU — измерено.
    CHECK_FALSE(sage::profile::GpuTimersAvailable());
    CHECK_TRUE(sage::profile::FrameCpuMs() >= 0.0);

    sage::profile::SetEnabled(false);
    sage::rhi::GraphicsDevice::SetCurrent(nullptr);
}
