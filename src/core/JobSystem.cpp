#include "JobSystem.h"
#include "Log.h"
#include <algorithm>

void JobSystem::Start(unsigned int threadCount) {
    if (m_running.load()) return;

    if (threadCount == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        threadCount = (hw > 1) ? hw - 1 : 1; // оставляем ядро под главный поток
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = false;
    }
    m_running.store(true, std::memory_order_release);

    m_workers.reserve(threadCount);
    for (unsigned int i = 0; i < threadCount; ++i) {
        m_workers.emplace_back([this] { WorkerLoop(); });
    }
    LOG_INFO("Jobs") << "Пул задач запущен: " << threadCount << " воркер(ов)";
}

void JobSystem::Shutdown() {
    if (!m_running.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();

    for (std::thread& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();

    // Отбрасываем невзятые задачи (их future'ы станут broken_promise —
    // это ожидаемо при завершении работы).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<std::function<void()>> empty;
        m_queue.swap(empty);
    }
    m_running.store(false, std::memory_order_release);
    LOG_INFO("Jobs") << "Пул задач остановлен";
}

void JobSystem::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping && m_queue.empty()) return;
            job = std::move(m_queue.front());
            m_queue.pop();
        }
        job(); // вне блокировки — тяжёлая работа не держит очередь
    }
}

void JobSystem::ParallelFor(size_t count, const std::function<void(size_t)>& body) {
    if (count == 0) return;

    unsigned int workers = WorkerCount();
    // Без пула (или на один элемент) — просто выполняем последовательно здесь.
    if (workers == 0 || count == 1) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }

    // Разбиваем на (workers + 1) кусков: воркеры + сам вызывающий поток.
    size_t chunks = std::min<size_t>(count, workers + 1);
    size_t per = (count + chunks - 1) / chunks;

    std::vector<std::future<void>> futures;
    futures.reserve(chunks - 1);
    // Первый кусок делает вызывающий поток; остальные уходят в пул.
    for (size_t c = 1; c < chunks; ++c) {
        size_t begin = c * per;
        size_t end = std::min(begin + per, count);
        if (begin >= end) break;
        futures.push_back(Enqueue([&body, begin, end] {
            for (size_t i = begin; i < end; ++i) body(i);
        }));
    }
    size_t selfEnd = std::min(per, count);
    for (size_t i = 0; i < selfEnd; ++i) body(i);

    for (std::future<void>& f : futures) f.get();
}
