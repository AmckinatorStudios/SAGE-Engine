// Интеграция системы задач с ECS: граф стадии в планировщике систем,
// параллельное выполнение независимых систем, конфликты доступа, явный
// порядок и циклы в нём, проверка объявлений в работе, параллельный обход
// сущностей и отложенные структурные правки. Без GL.
#include "TestFramework.h"

#include "sage/core/SystemScheduler.h"
#include "sage/ecs/Parallel.h"
#include "sage/jobs/JobSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using sage::Stage;
using sage::SystemAccess;

namespace {

struct Velocity { float X = 0, Y = 0; };
struct Health { float Value = 100; };
struct Tag {};

sage::JobSystem& Workers(int n = 4) {
    sage::JobSystem& js = sage::JobSystem::Get();
    sage::jobs::JobSystemConfig cfg;
    cfg.WorkerThreads = n;
    if (js.WorkerCount() != n || !js.IsRunning()) {
        js.Shutdown();
        js.Initialize(cfg);
    }
    js.SetEnabled(true);
    return js;
}

} // namespace

TEST(EcsGraph_undeclared_systems_stay_sequential_on_main_thread) {
    Workers();
    sage::SystemScheduler s;
    std::vector<std::string> log;
    const auto mainId = std::this_thread::get_id();
    bool allMain = true;
    for (int i = 0; i < 5; ++i)
        s.Add(Stage::Update, "sys" + std::to_string(i), [&, i](Scene&, float) {
            allMain &= std::this_thread::get_id() == mainId;
            log.push_back("sys" + std::to_string(i));  // без замка: строго по очереди
        }, i);
    Scene scene("Seq");
    s.Run(scene, 0.016f);
    CHECK_TRUE(allMain);
    CHECK_EQ((int)log.size(), 5);
    for (int i = 0; i < 5; ++i) CHECK_TRUE(log[(size_t)i] == "sys" + std::to_string(i));
    // В графе каждая следующая ждёт предыдущую (эксклюзивные).
    auto g = s.Graph();
    CHECK_EQ((int)g.size(), 5);
    CHECK_TRUE(g[4].DependsOn.size() == 4);
}

TEST(EcsGraph_independent_systems_run_concurrently) {
    Workers(4);
    sage::SystemScheduler s;
    // Две системы встречаются на защёлке: если планировщик пустил их
    // последовательно, первая не дождётся второй (таймаут) — тест провален,
    // а не завис.
    sage::sync::Latch meet(2);
    std::atomic<int> met{0};
    auto body = [&](Scene&, float) {
        meet.CountDown();
        if (meet.WaitFor(2s)) ++met;
    };
    s.Add(Stage::Update, "a", body, SystemAccess::Parallel().Write<Velocity>());
    s.Add(Stage::Update, "b", body, SystemAccess::Parallel().Write<Health>());
    Scene scene("Par");
    s.Run(scene, 0.016f);
    CHECK_EQ(met.load(), 2);
    auto g = s.Graph();
    CHECK_TRUE(g[1].DependsOn.empty());  // нет конфликта — нет ребра
    // Замеры обеих — на воркерах.
    int onWorker = 0;
    for (const auto& t : s.LastTimings()) onWorker += t.OnWorker ? 1 : 0;
    CHECK_TRUE(onWorker >= 1);
}

TEST(EcsGraph_conflicting_writers_keep_registration_order) {
    Workers(4);
    sage::SystemScheduler s;
    std::atomic<int> step{0};
    std::atomic<bool> ok{true};
    s.Add(Stage::Update, "first", [&](Scene&, float) {
        std::this_thread::sleep_for(5ms);
        if (step.exchange(1) != 0) ok = false;
    }, SystemAccess::Parallel().Write<Velocity>());
    s.Add(Stage::Update, "reader", [&](Scene&, float) {
        if (step.load() != 1) ok = false;  // читает то, что записала первая
        step = 2;
    }, SystemAccess::Parallel().Read<Velocity>(), 1);
    s.Add(Stage::Update, "free", [&](Scene&, float) {}, SystemAccess::Parallel().Read<Health>(), 2);
    Scene scene("Conf");
    for (int i = 0; i < 20; ++i) {
        step = 0;
        s.Run(scene, 0.016f);
    }
    CHECK_TRUE(ok.load());
    auto g = s.Graph();
    CHECK_TRUE(g[1].DependsOn.size() == 1 && g[1].DependsOn[0] == 0);
    CHECK_TRUE(g[2].DependsOn.empty());
}

TEST(EcsGraph_readers_do_not_conflict_resources_do) {
    SystemAccess r1 = SystemAccess::Parallel().Read<Velocity>().ReadResource("physics");
    SystemAccess r2 = SystemAccess::Parallel().Read<Velocity>().ReadResource("physics");
    CHECK_FALSE(r1.ConflictsWith(r2));
    SystemAccess w = SystemAccess::Parallel().WriteResource("physics");
    CHECK_TRUE(r1.ConflictsWith(w));
    CHECK_TRUE(SystemAccess{}.ConflictsWith(r1));  // необъявленная — эксклюзивна
}

TEST(EcsGraph_exclusive_system_is_a_barrier_inside_a_stage) {
    Workers(4);
    sage::SystemScheduler s;
    std::atomic<int> before{0};
    std::atomic<bool> barrierOk{true};
    std::atomic<bool> afterOk{true};
    s.Add(Stage::Update, "p1", [&](Scene&, float) { std::this_thread::sleep_for(3ms); ++before; },
          SystemAccess::Parallel().Write<Velocity>(), 0);
    s.Add(Stage::Update, "p2", [&](Scene&, float) { std::this_thread::sleep_for(3ms); ++before; },
          SystemAccess::Parallel().Write<Health>(), 0);
    s.Add(Stage::Update, "lua", [&](Scene&, float) {
        if (before.load() != 2) barrierOk = false;
        before = 10;
    }, 1);
    s.Add(Stage::Update, "p3", [&](Scene&, float) {
        if (before.load() != 10) afterOk = false;
    }, SystemAccess::Parallel().Write<Tag>(), 2);
    Scene scene("Barrier");
    s.Run(scene, 0.016f);
    CHECK_TRUE(barrierOk.load());
    CHECK_TRUE(afterOk.load());
}

TEST(EcsGraph_main_thread_systems_stay_on_main) {
    Workers(4);
    sage::SystemScheduler s;
    const auto mainId = std::this_thread::get_id();
    std::atomic<bool> uiOnMain{false};
    s.Add(Stage::LateUpdate, "ui", [&](Scene&, float) { uiOnMain = std::this_thread::get_id() == mainId; },
          SystemAccess::MainThreadOnly().Read<Health>());
    s.Add(Stage::LateUpdate, "work", [&](Scene&, float) { std::this_thread::sleep_for(1ms); },
          SystemAccess::Parallel().Write<Velocity>());
    Scene scene("Main");
    s.Run(scene, 0.016f);
    CHECK_TRUE(uiOnMain.load());
}

TEST(EcsGraph_run_after_orders_and_cycle_is_reported) {
    Workers(4);
    sage::SystemScheduler s;
    std::vector<std::string> log;
    std::mutex m;
    auto rec = [&](const char* n) {
        return [&, n](Scene&, float) {
            std::lock_guard<std::mutex> lk(m);
            log.push_back(n);
        };
    };
    // «a» зарегистрирована раньше, но явно идёт после «b».
    s.Add(Stage::Update, "a", rec("a"), SystemAccess::Parallel().RunAfter("b"), 0);
    s.Add(Stage::Update, "b", rec("b"), SystemAccess::Parallel(), 1);
    Scene scene("After");
    s.Run(scene, 0.016f);
    CHECK_EQ((int)log.size(), 2);
    CHECK_TRUE(log[0] == "b" && log[1] == "a");
    CHECK_TRUE(s.GraphError().empty());

    // Цикл: x после y, y после x — граф строится без явных рёбер, с ошибкой.
    sage::SystemScheduler c;
    int ran = 0;
    c.Add(Stage::Update, "x", [&](Scene&, float) { ++ran; }, SystemAccess::Parallel().RunAfter("y"));
    c.Add(Stage::Update, "y", [&](Scene&, float) { ++ran; }, SystemAccess::Parallel().RunAfter("x"));
    c.Run(scene, 0.016f);
    CHECK_EQ(ran, 2);  // кадр не потерян
    CHECK_FALSE(c.GraphError().empty());
}

TEST(EcsGraph_throwing_parallel_system_does_not_stall_the_stage) {
    Workers(4);
    sage::SystemScheduler s;
    std::atomic<int> ran{0};
    s.Add(Stage::Update, "bad", [](Scene&, float) { throw std::runtime_error("сломалась"); },
          SystemAccess::Parallel().Write<Velocity>());
    s.Add(Stage::Update, "next", [&](Scene&, float) { ++ran; }, SystemAccess::Parallel().Read<Velocity>(), 1);
    s.Add(Stage::Physics, "phys", [&](Scene&, float) { ++ran; });
    Scene scene("Throw");
    s.Run(scene, 0.016f);
    CHECK_EQ(ran.load(), 2);
}

TEST(EcsGraph_parallel_matches_sequential_result) {
    Workers(4);
    // Один и тот же набор систем — параллельно и строго последовательно —
    // даёт одинаковое состояние сцены.
    auto build = [](sage::SystemScheduler& s) {
        s.Add(Stage::Update, "move", [](Scene& sc, float dt) {
            sage::ecs::ParallelEach<Velocity, const Health>(sc.Registry(),
                [dt](entt::entity, Velocity& v, const Health& h) { v.X += h.Value * dt; v.Y -= dt; }, 16);
        }, SystemAccess::Parallel().Write<Velocity>().Read<Health>());
        s.Add(Stage::Update, "decay", [](Scene& sc, float) {
            sage::ecs::ParallelEach<Health>(sc.Registry(), [](entt::entity, Health& h) { h.Value *= 0.99f; }, 16);
        }, SystemAccess::Parallel().Write<Health>(), 1);
    };
    auto fill = [](Scene& sc) {
        for (int i = 0; i < 500; ++i) {
            auto e = sc.Registry().create();
            sc.Registry().emplace<Velocity>(e);
            sc.Registry().emplace<Health>(e, Health{(float)(i % 37)});
        }
    };
    Scene par("P"), seq("S");
    fill(par);
    fill(seq);
    sage::SystemScheduler sp, ss;
    build(sp);
    build(ss);
    ss.SetParallel(false);
    for (int f = 0; f < 10; ++f) {
        sp.Run(par, 0.016f);
        ss.Run(seq, 0.016f);
    }
    std::vector<float> a, b;
    for (auto e : par.Registry().view<Velocity>()) a.push_back(par.Registry().get<Velocity>(e).X);
    for (auto e : seq.Registry().view<Velocity>()) b.push_back(seq.Registry().get<Velocity>(e).X);
    CHECK_TRUE(a == b);
    CHECK_EQ((int)a.size(), 500);
}

TEST(EcsAccess_undeclared_component_use_is_detected) {
    Workers(4);
    sage::SystemScheduler s;
    const int before = sage::ComponentAccessViolations();
    s.Add(Stage::Update, "liar", [](Scene& sc, float) {
        // Объявила только чтение Velocity, а пишет Health.
        sage::ecs::ParallelEach<Health>(sc.Registry(), [](entt::entity, Health& h) { h.Value += 1; });
    }, SystemAccess::Parallel().Read<Velocity>());
    s.Add(Stage::Update, "honest", [](Scene& sc, float) {
        sage::ecs::ParallelEach<const Velocity>(sc.Registry(), [](entt::entity, const Velocity&) {});
    }, SystemAccess::Parallel().Read<Velocity>());
    Scene scene("Access");
    auto e = scene.Registry().create();
    scene.Registry().emplace<Velocity>(e);
    scene.Registry().emplace<Health>(e);
    s.Run(scene, 0.016f);
    CHECK_EQ(sage::ComponentAccessViolations(), before + 1);
    // Вне систем проверка молчит (нечего проверять).
    CHECK_TRUE(sage::CheckWrite<Health>());
}

TEST(EcsParallelEach_visits_every_entity_once) {
    Workers(4);
    Scene scene("Each");
    auto& reg = scene.Registry();
    for (int i = 0; i < 10000; ++i) {
        auto e = reg.create();
        reg.emplace<Velocity>(e);
        if (i % 2 == 0) reg.emplace<Tag>(e);
    }
    std::atomic<int> visits{0};
    sage::ecs::ParallelEach<Velocity>(reg, [&](entt::entity, Velocity& v) {
        v.X += 1.0f;
        ++visits;
    }, 64);
    CHECK_EQ(visits.load(), 10000);
    bool once = true;
    for (auto e : reg.view<Velocity>()) once &= reg.get<Velocity>(e).X == 1.0f;
    CHECK_TRUE(once);
}

TEST(EcsCommandBuffer_applies_structural_edits_deterministically) {
    Workers(4);
    Scene scene("Cmd");
    auto& reg = scene.Registry();
    std::vector<entt::entity> ents;
    for (int i = 0; i < 1000; ++i) {
        auto e = reg.create();
        reg.emplace<Health>(e, Health{(float)i});
        ents.push_back(e);
    }
    sage::ecs::CommandBuffer cmds;
    std::vector<int> order;
    // Из параллельного прохода — только запись правок, реестр не трогается.
    sage::JobSystem::Get().ParallelFor(ents.size(), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
            if (i % 3 == 0) cmds.Destroy(i, ents[i]);
            else cmds.Emplace<Tag>(i, ents[i]);
            cmds.Push(i, [&order, i](entt::registry&) { order.push_back((int)i); });
        }
    }, 16);
    CHECK_EQ((int)cmds.Size(), 2000);
    CHECK_EQ((int)cmds.Flush(reg), 2000);
    int alive = 0;
    for (auto e : reg.view<Health>()) {
        (void)e;
        ++alive;
    }
    CHECK_EQ(alive, 1000 - 334);
    CHECK_EQ((int)reg.view<Tag>().size(), 666);
    bool sorted = order.size() == 1000;
    for (size_t i = 0; sorted && i < order.size(); ++i) sorted = order[i] == (int)i;
    CHECK_TRUE(sorted);  // порядок — по ключу, не по тому, кто успел
}


TEST(EcsGraph_each_system_runs_exactly_once_per_frame) {
    // Регрессия: начальный проход «кто готов» читал счётчики после запуска
    // первой системы; та успевала закончиться на воркере и сама запустить
    // последователя — и последователь шёл второй раз (кадр «через раз»
    // применял затухание дважды).
    Workers(4);
    static std::atomic<int> moveCalls{0}, decayCalls{0};
    sage::SystemScheduler sp;
    sp.Add(Stage::Update, "move", [](Scene&, float) { ++moveCalls; },
           SystemAccess::Parallel().Write<Velocity>().Read<Health>());
    sp.Add(Stage::Update, "decay", [](Scene&, float) { ++decayCalls; },
           SystemAccess::Parallel().Write<Health>(), 1);
    sp.Add(Stage::Update, "free", [](Scene&, float) {}, SystemAccess::Parallel().Write<Tag>(), 2);
    Scene scene("Once");
    moveCalls = 0;
    decayCalls = 0;
    for (int f = 0; f < 2000; ++f) sp.Run(scene, 0.016f);
    CHECK_EQ(moveCalls.load(), 2000);
    CHECK_EQ(decayCalls.load(), 2000);
}
