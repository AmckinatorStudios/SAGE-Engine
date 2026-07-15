#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------
// JobSystem — пул рабочих потоков движка для фоновой CPU-работы: декод
// текстур/моделей, генерация геометрии, любая тяжёлая задача, которую не
// хочется делать в главном потоке и ронять частоту кадров. Часть ЯДРА
// движка, ни от чего игрового не зависит.
//
// ВАЖНО про OpenGL: воркеры НЕ имеют GL-контекста, поэтому в задачах нельзя
// вызывать gl*-функции. Схема асинхронной загрузки такая: воркер делает
// только CPU-часть (прочитать файл, декодировать пиксели/вершины), а
// финальную GL-загрузку возвращает в главный поток через MainThreadDispatcher
// (см. render/ResourceManager.h, *Async-методы — там это уже связано).
//
// Использование:
//   JobSystem::Instance().Start();                 // один раз при старте
//   auto fut = JobSystem::Instance().Enqueue([]{ return HeavyCompute(); });
//   ... позже: T result = fut.get();               // ждёт, если ещё не готово
//   JobSystem::Instance().Shutdown();              // при выходе (join потоков)
// ---------------------------------------------------------------------
class JobSystem {
public:
    static JobSystem& Instance() {
        static JobSystem instance;
        return instance;
    }

    // Запускает пул. threadCount==0 — авто: (число ядер - 1), минимум 1
    // (главный поток и так занят, оставляем ядро под него). Повторный вызов
    // при уже запущенном пуле игнорируется.
    void Start(unsigned int threadCount = 0);

    // Останавливает пул: даёт доработать взятым задачам, будит и join'ит все
    // потоки. Оставшиеся в очереди, но не начатые задачи отбрасываются.
    // Идемпотентно; вызывается и из деструктора на случай, если забыли.
    void Shutdown();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    unsigned int WorkerCount() const { return static_cast<unsigned int>(m_workers.size()); }

    // Кладёт задачу в очередь и возвращает std::future с её результатом.
    // Работает и до Start() (задача выполнится, как только пул запустится),
    // и корректно с любым возвращаемым типом, включая void.
    template <class F>
    auto Enqueue(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using Ret = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<Ret()>>(std::forward<F>(fn));
        std::future<Ret> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.emplace([task]() { (*task)(); });
        }
        m_cv.notify_one();
        return future;
    }

    // Параллельный цикл [0..count): разбивает диапазон между воркерами и
    // ЖДЁТ завершения всех частей. Вызывающий поток тоже берёт часть работы —
    // поэтому не зависает, даже если все воркеры сейчас заняты, и работает
    // (последовательно) до Start(). Тело должно быть потокобезопасным по
    // отношению к разным индексам. Вызывать из главного потока.
    void ParallelFor(size_t count, const std::function<void(size_t)>& body);

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

private:
    JobSystem() = default;
    ~JobSystem() { Shutdown(); }

    void WorkerLoop();

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{false};
    bool m_stopping = false; // под m_mutex
};
