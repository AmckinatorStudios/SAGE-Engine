#include "sage/core/JobSystem.h"
#include "sage/core/Log.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <thread>

namespace sage {

// --- Внутренний пул: очередь std::function + фоновые воркеры ---
struct JobSystem::Impl {
    std::vector<std::thread> Workers;
    std::mutex M;
    std::condition_variable CV;
    std::deque<std::function<void()>> Q;
    bool Stop = false;

    void WorkerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(M);
                CV.wait(lk, [&] { return Stop || !Q.empty(); });
                if (Stop && Q.empty()) return;
                job = std::move(Q.front());
                Q.pop_front();
            }
            job(); // job сам ловит/сохраняет исключения (см. packaged_task ниже)
        }
    }

    std::future<void> Submit(std::function<void()> fn) {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(fn));
        std::future<void> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(M);
            Q.push_back([task] { (*task)(); });
        }
        CV.notify_one();
        return fut;
    }
};

JobSystem& JobSystem::Get() {
    static JobSystem instance;
    return instance;
}

JobSystem::~JobSystem() { Shutdown(); }

void JobSystem::Initialize(int workerThreads) {
    int desired;
    if (workerThreads > 0) {
        desired = workerThreads;
    } else {
        unsigned hw = std::thread::hardware_concurrency();
        desired = hw > 1 ? (int)hw - 1 : 0; // главный поток тоже участвует
    }

    if (m_impl && (int)m_impl->Workers.size() == desired) return; // уже так — no-op
    Shutdown();

    m_impl = std::make_unique<Impl>();
    m_impl->Workers.reserve((size_t)std::max(0, desired));
    for (int i = 0; i < desired; ++i)
        m_impl->Workers.emplace_back([this] { m_impl->WorkerLoop(); });

    LOG_INFO("JobSystem") << "Пул задач: " << desired << " фоновых воркеров (+главный поток = "
                          << (desired + 1) << "-кратный параллелизм)";
}

void JobSystem::Shutdown() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->M);
        m_impl->Stop = true;
    }
    m_impl->CV.notify_all();
    for (std::thread& t : m_impl->Workers)
        if (t.joinable()) t.join();
    m_impl.reset();
}

int JobSystem::WorkerCount() const {
    return m_impl ? (int)m_impl->Workers.size() : 0;
}

void JobSystem::ParallelFor(std::size_t count,
                            const std::function<void(std::size_t, std::size_t)>& body,
                            std::size_t minPerThread) {
    if (count == 0) return;

    // Ленивая авто-инициализация, если Application не подняла пул явно.
    if (m_enabled && !m_impl) Initialize(0);

    const std::size_t workers = m_impl ? m_impl->Workers.size() : 0;
    if (!m_enabled || workers == 0) { body(0, count); return; } // последовательный фолбэк

    if (minPerThread == 0) minPerThread = 1;
    std::size_t maxChunks = workers + 1;                         // + вызывающий поток
    std::size_t wanted = (count + minPerThread - 1) / minPerThread;
    std::size_t chunks = std::min(maxChunks, std::max<std::size_t>(1, wanted));
    if (chunks <= 1) { body(0, count); return; }

    std::size_t per = (count + chunks - 1) / chunks;
    std::vector<std::future<void>> futs;
    futs.reserve(chunks - 1);
    for (std::size_t c = 1; c < chunks; ++c) {
        std::size_t begin = c * per;
        if (begin >= count) break;
        std::size_t end = std::min(count, begin + per);
        // body захватывается по ссылке — безопасно: ниже дожидаемся ВСЕХ futures
        // (в т.ч. при исключении в chunk-0), body живёт весь вызов.
        futs.push_back(m_impl->Submit([&body, begin, end] { body(begin, end); }));
    }

    // Chunk-0 исполняет вызывающий поток. Ловим его исключение, но всё равно
    // дожидаемся воркеров, иначе они бы держали висячую ссылку на body.
    std::exception_ptr firstError;
    try {
        body(0, std::min(count, per));
    } catch (...) {
        firstError = std::current_exception();
    }
    for (std::future<void>& f : futs) {
        try {
            f.get(); // пробрасывает исключение своего подотрезка
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void JobSystem::Dispatch(const std::vector<std::function<void()>>& tasks) {
    if (tasks.empty()) return;
    if (m_enabled && !m_impl) Initialize(0);

    const std::size_t workers = m_impl ? m_impl->Workers.size() : 0;
    if (!m_enabled || workers == 0) { // последовательный фолбэк
        for (const auto& t : tasks) t();
        return;
    }

    std::vector<std::future<void>> futs;
    futs.reserve(tasks.size() - 1);
    for (std::size_t i = 1; i < tasks.size(); ++i)
        futs.push_back(m_impl->Submit(tasks[i]));

    std::exception_ptr firstError;
    try {
        tasks[0]();
    } catch (...) {
        firstError = std::current_exception();
    }
    for (std::future<void>& f : futs) {
        try {
            f.get();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

} // namespace sage
