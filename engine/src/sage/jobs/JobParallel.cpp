#include "sage/jobs/JobSystemDetail.h"

#include <algorithm>

// Параллельные проходы: ParallelFor, Dispatch, Batch.
//
// УСТРОЙСТВО. Диапазон режется на куски (не меньше Grain элементов, не больше
// четырёх на поток — запас для балансировки), а куски РАЗБИРАЮТСЯ атомарным
// счётчиком: вызывающий и несколько задач-помощниц берут следующий свободный
// кусок, пока они есть. Поэтому:
//   • задач создаётся не больше числа воркеров, а не по задаче на кусок;
//   • медленный кусок не задерживает остальные — их доберут свободные;
//   • если помощница так и не стартовала (все воркеры заняты), вызывающий
//     сделает всё сам: ParallelFor никогда не ждёт воркера, который не придёт,
//     и вложенный ParallelFor внутри задачи не может повиснуть;
//   • помощница, начавшая, когда кусков не осталось, выходит сразу.
namespace sage::jobs::detail {
namespace {

struct ForState {
    const std::function<void(std::size_t, std::size_t)>* Body = nullptr;
    std::function<void(std::size_t, std::size_t)> OwnedBody;  // для асинхронного
    std::size_t Count = 0, Chunk = 1, Chunks = 0;
    std::atomic<std::size_t> Next{0};
    std::atomic<bool> Failed{false};
    std::mutex ErrM;
    std::exception_ptr Error;
    std::shared_ptr<CancelState> Token;

    void RunChunks() {
        const auto& body = Body ? *Body : OwnedBody;
        for (;;) {
            if (Failed.load(std::memory_order_relaxed)) return;
            if (Token && Token->Requested.load(std::memory_order_relaxed)) return;
            const std::size_t c = Next.fetch_add(1, std::memory_order_relaxed);
            if (c >= Chunks) return;
            const std::size_t b = c * Chunk;
            const std::size_t e = std::min(Count, b + Chunk);
            try {
                body(b, e);
            } catch (...) {
                std::lock_guard<std::mutex> lk(ErrM);
                if (!Error) Error = std::current_exception();
                Failed.store(true, std::memory_order_relaxed);
            }
        }
    }
};

// Сколько кусков: не меньше grain элементов в куске, не больше 4 на поток.
std::size_t PlanChunks(std::size_t count, std::size_t grain, int threads) {
    grain = std::max<std::size_t>(1, grain);
    const std::size_t wanted = (count + grain - 1) / grain;
    const std::size_t cap = (std::size_t)std::max(1, threads) * 4;
    return std::max<std::size_t>(1, std::min(wanted, cap));
}

} // namespace
} // namespace sage::jobs::detail

namespace sage {

using namespace jobs;
using jobs::detail::Core;
using jobs::detail::ForState;

void JobSystem::ParallelFor(std::size_t count,
                            const std::function<void(std::size_t, std::size_t)>& body,
                            std::size_t minPerThread) {
    ParallelOptions o;
    o.Grain = minPerThread;
    ParallelFor(count, body, o);
}

void JobSystem::ParallelFor(std::size_t count,
                            const std::function<void(std::size_t, std::size_t)>& body,
                            const ParallelOptions& options) {
    if (count == 0) return;
    Core* core = m_enabled ? CoreForSubmit() : nullptr;
    const int workers = core ? (int)core->Workers.size() : 0;
    // Последовательный путь: выключено, одно ядро или делить нечего. Поведение
    // идентично параллельному (тело то же, куски — один).
    std::size_t chunks = detail::PlanChunks(count, options.Grain, workers + 1);
    if (!core || workers == 0 || chunks <= 1) {
        if (options.Token.IsCancelled()) return;
        body(0, count);
        return;
    }
    ForState st;
    st.Body = &body;
    st.Count = count;
    st.Chunk = (count + chunks - 1) / chunks;
    st.Chunks = (count + st.Chunk - 1) / st.Chunk;
    st.Token = options.Token.State();

    // Помощниц — не больше, чем воркеров и чем кусков сверх своего.
    const std::size_t helpers = std::min<std::size_t>((std::size_t)workers, st.Chunks - 1);
    JobOptions jo(options.Name, options.Cat, Lane::Worker, options.Prio);
    std::vector<JobHandle> hs;
    hs.reserve(helpers);
    for (std::size_t i = 0; i < helpers; ++i)
        hs.push_back(Run([&st] { st.RunChunks(); }, jo));
    st.RunChunks();
    // st на стеке: дожидаемся ВСЕХ помощниц, даже упавших и отменённых, —
    // иначе они читали бы его после выхода. Ожидание помогает (см. Wait).
    for (const JobHandle& h : hs) h.Wait();
    if (st.Error) std::rethrow_exception(st.Error);
}

namespace {
// Асинхронный проход; after — зависимости (помощницы стартуют после них).
JobHandle LaunchFor(JobSystem& js, std::size_t count, std::function<void(std::size_t, std::size_t)> body,
                    const ParallelOptions& options, const std::vector<JobHandle>& after) {
    JobOptions jo(options.Name, options.Cat, Lane::Worker, options.Prio);
    jo.Token = options.Token;
    jo.After = after;
    if (count == 0) return js.Async([] {}, jo);
    Core* core = js.CoreForSubmit();
    const int workers = core ? (int)core->Workers.size() : 0;
    auto st = std::make_shared<ForState>();
    st->OwnedBody = std::move(body);
    st->Count = count;
    const std::size_t chunks = detail::PlanChunks(count, options.Grain, std::max(1, workers));
    st->Chunk = (count + chunks - 1) / chunks;
    st->Chunks = (count + st->Chunk - 1) / st->Chunk;
    st->Token = options.Token.State();
    const std::size_t helpers =
        std::max<std::size_t>(1, std::min<std::size_t>((std::size_t)std::max(1, workers), st->Chunks));
    std::vector<JobHandle> hs;
    hs.reserve(helpers);
    for (std::size_t i = 0; i < helpers; ++i) hs.push_back(js.Run([st] { st->RunChunks(); }, jo));
    // Итоговая задача пробрасывает первую ошибку тела — Future её увидит.
    // Упавшая/отменённая зависимость пакета передаётся дальше как есть.
    JobOptions fin(options.Name, options.Cat, Lane::Worker, options.Prio);
    fin.After = std::move(hs);
    return js.Async([st] {
        if (st->Error) std::rethrow_exception(st->Error);
    }, fin);
}
} // namespace

JobHandle JobSystem::ParallelForAsync(std::size_t count,
                                      std::function<void(std::size_t, std::size_t)> body,
                                      const ParallelOptions& options) {
    return LaunchFor(*this, count, std::move(body), options, {});
}

void JobSystem::Dispatch(const std::vector<std::function<void()>>& tasks) {
    if (tasks.empty()) return;
    ParallelOptions o;
    o.Grain = 1;
    o.Name = "dispatch";
    ParallelFor(tasks.size(), [&tasks](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) tasks[i]();
    }, o);
}

JobHandle JobSystem::Batch(std::vector<std::function<void()>> items, JobOptions opts, std::size_t perJob) {
    auto shared = std::make_shared<std::vector<std::function<void()>>>(std::move(items));
    const std::size_t n = shared->size();
    if (perJob == 0) {
        const int threads = std::max(1, WorkerCount());
        perJob = std::max<std::size_t>(1, n / ((std::size_t)threads * 4));
    }
    ParallelOptions po;
    po.Grain = perJob;
    po.Name = opts.Name;
    po.Cat = opts.Cat;
    po.Prio = opts.Prio;
    po.Token = opts.Token;
    return LaunchFor(*this, n, [shared](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) (*shared)[i]();
    }, po, opts.After);
}

} // namespace sage
