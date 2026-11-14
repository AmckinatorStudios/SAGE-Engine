#include "sage/core/SystemScheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

#include "sage/core/EngineContext.h"
#include "sage/core/Log.h"
#include "sage/jobs/JobSystem.h"

// Граф стадии и его выполнение (см. шапку SystemScheduler.h, «Параллельно
// внутри стадии»).
namespace sage {

void SystemScheduler::BuildGraph() {
    const size_t n = m_systems.size();
    m_deps.assign(n, {});
    m_succ.assign(n, {});
    m_topo.clear();
    m_topo.reserve(n);
    m_graphError.clear();

    size_t b = 0;
    while (b < n) {
        size_t e = b;
        while (e < n && m_systems[e].InStage == m_systems[b].InStage) ++e;

        std::unordered_map<std::string, int> byName;
        for (size_t i = b; i < e; ++i) byName[m_systems[i].Name] = (int)i;

        // Явные рёбра (RunAfter) — первыми: они задают направление и для
        // конфликтующей пары, иначе «после X» у более ранней по Order системы
        // столкнулось бы с ребром конфликта «X после неё» и дало цикл там,
        // где автор просто уточнил порядок.
        std::vector<std::vector<int>> explicitDeps(e - b);
        for (size_t j = b; j < e; ++j) {
            for (const std::string& name : m_systems[j].Access.After) {
                auto it = byName.find(name);
                if (it == byName.end()) {
                    // Система другой стадии — порядок уже задан барьером стадий.
                    bool elsewhere = false;
                    for (const Entry& o : m_systems) elsewhere |= o.Name == name;
                    if (!elsewhere) {
                        m_graphError = "Система '" + m_systems[j].Name + "' ждёт неизвестную '" + name + "'";
                        LOG_WARN("Systems") << m_graphError;
                    }
                    continue;
                }
                if (it->second != (int)j) explicitDeps[j - b].push_back(it->second);
            }
        }
        auto build = [&](bool withExplicit) {
            for (size_t j = b; j < e; ++j) m_deps[j].clear();
            for (size_t j = b; j < e; ++j) {
                if (withExplicit)
                    for (int d : explicitDeps[j - b]) m_deps[j].push_back(d);
                for (size_t i = b; i < j; ++i) {
                    if (!m_systems[i].Access.ConflictsWith(m_systems[j].Access)) continue;
                    const auto& ei = explicitDeps[i - b];
                    const bool reversed = withExplicit && std::find(ei.begin(), ei.end(), (int)j) != ei.end();
                    if (!reversed) m_deps[j].push_back((int)i);
                }
                std::sort(m_deps[j].begin(), m_deps[j].end());
                m_deps[j].erase(std::unique(m_deps[j].begin(), m_deps[j].end()), m_deps[j].end());
            }
        };
        // Кан с приоритетом по исходному индексу: без явных рёбер порядок
        // совпадает с регистрацией — граф ничего не переставляет зря.
        auto topo = [&](std::vector<int>& out) {
            std::vector<int> indeg(e - b, 0);
            for (size_t j = b; j < e; ++j) indeg[j - b] = (int)m_deps[j].size();
            std::vector<std::vector<int>> succ(e - b);
            for (size_t j = b; j < e; ++j)
                for (int d : m_deps[j]) succ[(size_t)d - b].push_back((int)j);
            std::vector<int> ready;
            for (size_t j = b; j < e; ++j)
                if (indeg[j - b] == 0) ready.push_back((int)j);
            out.clear();
            while (!ready.empty()) {
                auto it = std::min_element(ready.begin(), ready.end());
                const int k = *it;
                ready.erase(it);
                out.push_back(k);
                for (int s : succ[(size_t)k - b])
                    if (--indeg[(size_t)s - b] == 0) ready.push_back(s);
            }
            return out.size() == e - b;
        };
        build(true);
        std::vector<int> order;
        if (!topo(order)) {
            // Цикл из RunAfter: строим без явных рёбер (порядок регистрации
            // плюс конфликты — циклов не бывает) и громко говорим об этом.
            std::string names;
            for (size_t j = b; j < e; ++j)
                if (std::find(order.begin(), order.end(), (int)j) == order.end())
                    names += (names.empty() ? "" : ", ") + m_systems[j].Name;
            m_graphError = std::string("Цикл в порядке систем стадии ") + StageName(m_systems[b].InStage) +
                           ": " + names + " — явный порядок (RunAfter) отброшен";
            LOG_ERROR("Systems") << m_graphError;
            build(false);
            topo(order);
        }
        for (int k : order) m_topo.push_back(k);
        for (size_t j = b; j < e; ++j)
            for (int d : m_deps[j]) m_succ[(size_t)d].push_back((int)j);
        b = e;
    }
}

std::vector<SystemScheduler::GraphNode> SystemScheduler::Graph() {
    if (m_dirty) Sort();
    std::vector<GraphNode> out;
    out.reserve(m_systems.size());
    for (size_t i = 0; i < m_systems.size(); ++i) {
        const Entry& e = m_systems[i];
        GraphNode g;
        g.Name = e.Name;
        g.InStage = e.InStage;
        g.MainThread = e.Access.MainThread;
        g.Exclusive = e.Access.Exclusive;
        g.Access = e.Access.Describe();
        g.DependsOn = m_deps[i];
        out.push_back(std::move(g));
    }
    return out;
}

void SystemScheduler::RunStageGraph(size_t begin, size_t end, Scene& scene, float dt) {
    using namespace sage::jobs;
    JobSystem& js = EngineContext::Current().Jobs();
    const int n = (int)(end - begin);

    // Состояние стадии живёт на стеке: выход отсюда только после того, как
    // ВСЕ задачи систем завершены (ожидание ручек в конце).
    struct StageRun {
        std::unique_ptr<std::atomic<int>[]> Remaining;
        std::atomic<int> Finished{0};
        sync::SpinLock Lock;
        std::vector<int> MainReady;    // локальные индексы, ждущие главный поток
        std::vector<JobHandle> Handles;
        sync::Event Wake{sync::Event::Mode::AutoReset};
        std::vector<float> Ms;
        std::vector<char> OnWorker;
    } st;
    st.Remaining.reset(new std::atomic<int>[(size_t)n]);
    st.Ms.assign((size_t)n, 0.0f);
    st.OnWorker.assign((size_t)n, 0);
    for (int k = 0; k < n; ++k) st.Remaining[(size_t)k].store((int)m_deps[begin + (size_t)k].size());

    std::function<void(int)> launch;
    auto runTimed = [&](int k, bool onMain) {
        const Entry& e = m_systems[begin + (size_t)k];
        const auto t0 = std::chrono::steady_clock::now();
        RunOne(e, scene, dt, onMain);
        st.Ms[(size_t)k] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        st.OnWorker[(size_t)k] = onMain ? 0 : 1;
    };
    auto complete = [&](int k) {
        for (int s : m_succ[begin + (size_t)k]) {
            const int ls = s - (int)begin;
            if (st.Remaining[(size_t)ls].fetch_sub(1, std::memory_order_acq_rel) == 1) launch(ls);
        }
        st.Finished.fetch_add(1, std::memory_order_acq_rel);
        st.Wake.Set();
    };
    launch = [&](int k) {
        const Entry& e = m_systems[begin + (size_t)k];
        if (e.Access.MainThread) {
            {
                std::lock_guard<sync::SpinLock> lk(st.Lock);
                st.MainReady.push_back(k);
            }
            st.Wake.Set();
            return;
        }
        JobOptions o("system", Category::ECS, Lane::Worker, Priority::High);
        o.WithName(e.Name);
        JobHandle h = js.Run([&, k] {
            runTimed(k, false);
            complete(k);
        }, o);
        // Отклонённая задача (система задач остановлена) — выполнить здесь,
        // иначе стадия ждала бы её вечно.
        if (h.IsDone() && h.Canceled()) {
            std::lock_guard<sync::SpinLock> lk(st.Lock);
            st.MainReady.push_back(k);
            return;
        }
        std::lock_guard<sync::SpinLock> lk(st.Lock);
        st.Handles.push_back(std::move(h));
    };

    // Готовые с самого начала — СПИСКОМ до первого запуска. Читать счётчики
    // по ходу запусков нельзя: первая система может успеть закончиться на
    // воркере и сама обнулить счётчик последователя — и тот был бы запущен
    // второй раз отсюда (система выполнялась дважды за кадр).
    std::vector<int> roots;
    for (int k = 0; k < n; ++k)
        if (m_deps[begin + (size_t)k].empty()) roots.push_back(k);
    for (int k : roots) launch(k);

    // Главный поток: свои системы (по порядку регистрации среди готовых),
    // а в промежутках — помощь воркерам вместо простоя.
    while (st.Finished.load(std::memory_order_acquire) < n) {
        int k = -1;
        {
            std::lock_guard<sync::SpinLock> lk(st.Lock);
            if (!st.MainReady.empty()) {
                auto it = std::min_element(st.MainReady.begin(), st.MainReady.end());
                k = *it;
                st.MainReady.erase(it);
            }
        }
        if (k >= 0) {
            runTimed(k, true);
            complete(k);
            continue;
        }
        if (js.HelpOnce()) continue;
        st.Wake.WaitFor(std::chrono::microseconds(200));
    }
    std::vector<JobHandle> handles;
    {
        std::lock_guard<sync::SpinLock> lk(st.Lock);
        handles.swap(st.Handles);
    }
    for (const JobHandle& h : handles) h.Wait();

    if (m_profiling)
        for (size_t t = 0; t < m_topo.size(); ++t) {
            const int idx = m_topo[t];
            if (idx < (int)begin || idx >= (int)end) continue;
            const int k = idx - (int)begin;
            const Entry& e = m_systems[(size_t)idx];
            m_timings.push_back({e.Name, e.InStage, st.Ms[(size_t)k], st.OnWorker[(size_t)k] != 0});
        }
}

} // namespace sage
