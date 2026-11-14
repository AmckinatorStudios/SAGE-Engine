#include "sage/jobs/JobSystemDetail.h"

#include <algorithm>

// Диагностика системы задач: снимок счётчиков, зависшие задачи, ошибки, шкала
// времени. Всё читается без остановки потоков — атомики и короткие замки на
// данные самого снимка; цена — когда смотрят (панель открыта), а не всегда.
namespace sage {

using namespace jobs;
using jobs::detail::Core;
using jobs::detail::NowNs;
using jobs::detail::ThreadSlot;

namespace {

ThreadStats SnapshotThread(const Core& core, ThreadSlot& s, int64_t now, bool& hung, HungJob& hj) {
    ThreadStats t;
    t.Name = s.Name;
    t.Kind = s.Kind;
    t.Executed = s.Executed.load(std::memory_order_relaxed);
    t.Stolen = s.Stolen.load(std::memory_order_relaxed);
    t.Parks = s.Parks.load(std::memory_order_relaxed);
    int64_t busy = s.BusyNs.load(std::memory_order_relaxed);
    Lane lane = Lane::Worker;
    Category cat = Category::General;
    int64_t start = 0;
    {
        std::lock_guard<sync::SpinLock> lk(s.InfoLock);
        start = s.CurrentStartNs.load(std::memory_order_relaxed);
        if (start) {
            t.CurrentJob = s.CurrentStatic ? s.CurrentStatic : s.CurrentName;
            lane = s.CurrentLane;
            cat = s.CurrentCat;
        }
    }
    if (start) {
        t.CurrentJobMs = (double)(now - start) / 1e6;
        busy += now - start;  // идущая задача — тоже занятость
        const double limit = (lane == Lane::Worker || lane == Lane::Main || lane == Lane::Render)
                                 ? core.Cfg.HungJobMs
                                 : core.Cfg.HungBackgroundMs;
        if (t.CurrentJobMs > limit) {
            hung = true;
            hj.Name = t.CurrentJob;
            hj.Thread = s.Name;
            hj.InLane = lane;
            hj.Cat = cat;
            hj.RunningMs = t.CurrentJobMs;
        }
    }
    t.BusyMs = (double)busy / 1e6;
    // Доля занятости — между двумя снимками (окно — промежуток между ними;
    // панель спрашивает раз в кадр, получается сглаженная по кадру цифра).
    const int64_t dt = now - s.LastSampleNs;
    if (s.LastSampleNs != 0 && dt > 0) {
        const double u = (double)(busy - s.LastBusyNs) / (double)dt;
        // Сглаживание: кадр к кадру доля скачет, читать нужно тренд.
        s.LastUtil = s.LastUtil * 0.8 + std::min(1.0, std::max(0.0, u)) * 0.2;
    }
    s.LastBusyNs = busy;
    s.LastSampleNs = now;
    t.Utilization = s.LastUtil;
    return t;
}

} // namespace

JobSystemStats JobSystem::Stats() const {
    JobSystemStats st;
    Core* core = m_core.get();
    st.Locks = sync::CollectLockStats();
    if (!core) return st;
    std::lock_guard<std::mutex> lk(core->StatsM);
    const int64_t now = NowNs();
    st.Running = true;
    st.Workers = (int)core->Workers.size();
    st.IoThreads = core->IoCount.load(std::memory_order_relaxed);
    st.BackgroundLimit = core->BackgroundLimit;
    for (int l = 0; l < (int)Lane::Count; ++l)
        st.Queued[l] = core->Queues[(size_t)l]->Size.load(std::memory_order_relaxed);
    for (auto& w : core->Workers) st.Queued[(int)Lane::Worker] += (int)w->Deque.SizeApprox();
    {
        std::lock_guard<std::mutex> wl(core->WaitingM);
        st.WaitingOnDeps = (int)core->WaitingSet.size();
    }
    st.BackgroundRunning = core->BackgroundRunning.load(std::memory_order_relaxed);
    st.Steals = core->Steals.load(std::memory_order_relaxed);
    st.StealAttempts = core->StealAttempts.load(std::memory_order_relaxed);
    st.HelpedWhileWaiting = core->Helped.load(std::memory_order_relaxed);
    st.DuplicateRunsPrevented = core->DupPrevented.load(std::memory_order_relaxed);
    st.MainThreadWaits = core->MainWaits.load(std::memory_order_relaxed);
    st.MainThreadWaitMs = (double)core->MainWaitNs.load(std::memory_order_relaxed) / 1e6;
    st.UptimeSeconds = (double)(now - core->StartNs) / 1e9;

    auto add = [&](ThreadSlot& s) {
        bool hung = false;
        HungJob hj;
        st.Threads.push_back(SnapshotThread(*core, s, now, hung, hj));
        if (hung) st.Hung.push_back(std::move(hj));
    };
    add(core->MainSlot);
    for (auto& w : core->Workers) add(*w);
    {
        std::lock_guard<std::mutex> il(core->IoM);
        for (auto& s : core->Io) add(*s);
    }
    // Счётчики — по всем потокам (каждый пишет свои, см. SlotCounters).
    std::vector<jobs::detail::SlotCounters*> counters{&core->MainSlot.C, &core->Foreign};
    for (auto& w : core->Workers) counters.push_back(&w->C);
    {
        std::lock_guard<std::mutex> il(core->IoM);
        for (auto& s : core->Io) counters.push_back(&s->C);
    }
    for (auto* sc : counters) {
        st.Submitted += sc->Submitted.load(std::memory_order_relaxed);
        st.Completed += sc->Completed.load(std::memory_order_relaxed);
        st.Failed += sc->Failed.load(std::memory_order_relaxed);
        st.Canceled += sc->Canceled.load(std::memory_order_relaxed);
    }
    for (const ThreadStats& t : st.Threads) st.Running_ += t.CurrentJob.empty() ? 0 : 1;
    for (int c = 0; c < (int)Category::Count; ++c) {
        CategoryStats cs;
        cs.Cat = (Category)c;
        int64_t exec = 0, queue = 0, mx = 0;
        for (auto* sc : counters) {
            const auto& cc = sc->Cats[c];
            cs.Executed += cc.Executed.load(std::memory_order_relaxed);
            cs.Failed += cc.Failed.load(std::memory_order_relaxed);
            cs.Canceled += cc.Canceled.load(std::memory_order_relaxed);
            exec += cc.ExecNs.load(std::memory_order_relaxed);
            queue += cc.QueueNs.load(std::memory_order_relaxed);
            mx = std::max<int64_t>(mx, cc.MaxExecNs.load(std::memory_order_relaxed));
        }
        if (cs.Executed == 0 && cs.Failed == 0 && cs.Canceled == 0) continue;
        cs.ExecMs = (double)exec / 1e6;
        cs.QueueMs = (double)queue / 1e6;
        cs.MaxExecMs = (double)mx / 1e6;
        st.Categories.push_back(cs);
    }
    {
        std::lock_guard<std::mutex> el(core->ErrorsM);
        st.Errors.assign(core->Errors.begin(), core->Errors.end());
    }
    return st;
}

void JobSystem::ResetStats() {
    Core* core = m_core.get();
    sync::ResetLockStats();
    if (!core) return;
    core->Steals = 0;
    core->StealAttempts = 0;
    core->Helped = 0;
    core->DupPrevented = 0;
    core->MainWaits = 0;
    core->MainWaitNs = 0;
    core->Foreign.Reset();
    auto reset = [](ThreadSlot& s) {
        s.Executed = 0;
        s.Stolen = 0;
        s.Parks = 0;
        s.C.Reset();
    };
    reset(core->MainSlot);
    for (auto& w : core->Workers) reset(*w);
    std::lock_guard<std::mutex> il(core->IoM);
    for (auto& s : core->Io) reset(*s);
    std::lock_guard<std::mutex> el(core->ErrorsM);
    core->Errors.clear();
}

void JobSystem::SetTracing(bool on) {
    if (Core* c = m_core.get()) c->Tracing.store(on, std::memory_order_relaxed);
}

bool JobSystem::Tracing() const {
    Core* c = m_core.get();
    return c && c->Tracing.load(std::memory_order_relaxed);
}

double JobSystem::NowMs() const {
    Core* c = m_core.get();
    return c ? (double)(NowNs() - c->StartNs) / 1e6 : 0.0;
}

std::vector<TraceEvent> JobSystem::Trace(double windowMs) const {
    std::vector<TraceEvent> out;
    Core* core = m_core.get();
    if (!core) return out;
    const int64_t now = NowNs();
    const int64_t from = now - (int64_t)(windowMs * 1e6);
    auto collect = [&](ThreadSlot& s) {
        std::lock_guard<sync::SpinLock> lk(s.Trace.L);
        for (int i = 0; i < s.Trace.Count; ++i) {
            const int idx = (s.Trace.Head - 1 - i + jobs::detail::TraceRing::kSize) % jobs::detail::TraceRing::kSize;
            const auto& it = s.Trace.Items[(size_t)idx];
            if (it.EndNs < from) break;  // дальше — только старее
            TraceEvent e;
            e.Name = it.Name;
            e.Cat = it.Cat;
            e.Thread = s.Index;
            e.StartMs = (double)(it.StartNs - core->StartNs) / 1e6;
            e.EndMs = (double)(it.EndNs - core->StartNs) / 1e6;
            out.push_back(std::move(e));
        }
    };
    collect(core->MainSlot);
    for (auto& w : core->Workers) collect(*w);
    {
        std::lock_guard<std::mutex> il(core->IoM);
        for (auto& s : core->Io) collect(*s);
    }
    std::sort(out.begin(), out.end(), [](const TraceEvent& a, const TraceEvent& b) { return a.StartMs < b.StartMs; });
    return out;
}

} // namespace sage
