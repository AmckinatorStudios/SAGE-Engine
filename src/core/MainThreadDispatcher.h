#pragma once
#include <functional>
#include <mutex>
#include <queue>
#include <cstddef>

// ---------------------------------------------------------------------
// MainThreadDispatcher — потокобезопасная очередь замыканий, которые должны
// выполниться именно в ГЛАВНОМ потоке. Нужна как «обратный мост» к
// JobSystem: воркер сделал CPU-часть в фоне и хочет доделать то, что можно
// только в главном потоке — прежде всего GL-загрузку (glGenTextures/
// glBufferData и т.п., которые валидны лишь на потоке с GL-контекстом).
//
// Post() можно звать из любого потока; Drain() — только из главного, раз в
// кадр. Часть ЯДРА движка.
//
// Использование:
//   // в фоновой задаче (воркер JobSystem):
//   MainThreadDispatcher::Instance().Post([data]{ UploadToGpu(data); });
//   // в главном цикле, раз в кадр:
//   MainThreadDispatcher::Instance().Drain(2.0); // не дольше ~2 мс за кадр
// ---------------------------------------------------------------------
class MainThreadDispatcher {
public:
    static MainThreadDispatcher& Instance() {
        static MainThreadDispatcher instance;
        return instance;
    }

    // Ставит задачу на выполнение в главном потоке. Потокобезопасно.
    void Post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(fn));
    }

    // Выполняет накопленные задачи В ГЛАВНОМ ПОТОКЕ. maxMillis > 0 задаёт
    // бюджет времени на кадр: как только он превышен, оставшиеся задачи
    // переносятся на следующий кадр — так пачка тяжёлых GL-загрузок не даёт
    // фриз, а «размазывается» по кадрам. maxMillis == 0 — выполнить все.
    // Возвращает число выполненных задач. Вызывать ТОЛЬКО из главного потока.
    size_t Drain(double maxMillis = 0.0);

    size_t Pending() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    MainThreadDispatcher(const MainThreadDispatcher&) = delete;
    MainThreadDispatcher& operator=(const MainThreadDispatcher&) = delete;

private:
    MainThreadDispatcher() = default;

    mutable std::mutex m_mutex;
    std::queue<std::function<void()>> m_queue;
};
