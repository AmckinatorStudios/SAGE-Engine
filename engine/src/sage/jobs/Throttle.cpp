#include "sage/jobs/Throttle.h"

#include <algorithm>

namespace sage::jobs {

struct Throttle::State : std::enable_shared_from_this<Throttle::State> {
    struct Item {
        std::function<void()> Run;
        std::function<void()> Cancel;
    };
    mutable std::mutex M;
    std::deque<Item> Pending;
    int RunningNow = 0;
    int Limit = 1;
    JobOptions Opts;
    TaskGroup Group;

    // Запустить ожидающие, пока есть места. Задача, закончив, сама зовёт это
    // снова — поэтому очередь движется без отдельного потока-диспетчера.
    void Pump() {
        for (;;) {
            Item item;
            {
                std::lock_guard<std::mutex> lk(M);
                if (Pending.empty() || RunningNow >= Limit) return;
                item = std::move(Pending.front());
                Pending.pop_front();
                ++RunningNow;
            }
            JobOptions o = Opts;
            o.Group = &Group;
            std::shared_ptr<State> self = shared_from_this();
            JobHandle h = JobSystem::Get().Run([self, run = std::move(item.Run)]() mutable {
                struct Done {
                    State* S;
                    ~Done() {
                        {
                            std::lock_guard<std::mutex> lk(S->M);
                            --S->RunningNow;
                        }
                        S->Pump();
                    }
                } done{self.get()};
                run();
            }, o);
            // Задачу отклонили (система остановлена) — место освободить сразу
            // и отменить обещание, иначе ждущий его повиснет.
            if (h.IsDone() && h.Canceled()) {
                {
                    std::lock_guard<std::mutex> lk(M);
                    --RunningNow;
                }
                if (item.Cancel) item.Cancel();
                std::deque<Item> rest;
                {
                    std::lock_guard<std::mutex> lk(M);
                    rest.swap(Pending);
                }
                for (Item& it : rest)
                    if (it.Cancel) it.Cancel();
                return;
            }
        }
    }
};

Throttle::Throttle(int maxConcurrent, JobOptions defaults) : m_s(std::make_shared<State>()) {
    m_s->Limit = std::max(1, maxConcurrent);
    m_s->Opts = std::move(defaults);
    m_s->Opts.Group = nullptr;
}

Throttle::~Throttle() {
    CancelAll();
    m_s->Group.Wait();
}

void Throttle::Enqueue(std::function<void()> run, std::function<void()> cancel) {
    {
        std::lock_guard<std::mutex> lk(m_s->M);
        m_s->Pending.push_back({std::move(run), std::move(cancel)});
    }
    m_s->Pump();
}

void Throttle::SetLimit(int maxConcurrent) {
    {
        std::lock_guard<std::mutex> lk(m_s->M);
        m_s->Limit = std::max(1, maxConcurrent);
    }
    m_s->Pump();
}

int Throttle::Limit() const {
    std::lock_guard<std::mutex> lk(m_s->M);
    return m_s->Limit;
}

int Throttle::Running() const {
    std::lock_guard<std::mutex> lk(m_s->M);
    return m_s->RunningNow;
}

int Throttle::Queued() const {
    std::lock_guard<std::mutex> lk(m_s->M);
    return (int)m_s->Pending.size();
}

void Throttle::CancelAll() {
    std::deque<State::Item> dropped;
    {
        std::lock_guard<std::mutex> lk(m_s->M);
        dropped.swap(m_s->Pending);
    }
    for (auto& it : dropped)
        if (it.Cancel) it.Cancel();
}

void Throttle::Wait() {
    for (;;) {
        m_s->Group.Wait();
        std::lock_guard<std::mutex> lk(m_s->M);
        if (m_s->Pending.empty() && m_s->RunningNow == 0) return;
    }
}

} // namespace sage::jobs
