#include "MainThreadDispatcher.h"
#include <chrono>

size_t MainThreadDispatcher::Drain(double maxMillis) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    size_t done = 0;

    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty()) break;
            job = std::move(m_queue.front());
            m_queue.pop();
        }
        job(); // вне блокировки: задача может сама вызвать Post() без дедлока
        ++done;

        if (maxMillis > 0.0) {
            double elapsed = std::chrono::duration<double, std::milli>(clock::now() - start).count();
            if (elapsed >= maxMillis) break; // остаток — на следующий кадр
        }
    }
    return done;
}
