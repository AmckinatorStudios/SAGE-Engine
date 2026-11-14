#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "sage/jobs/JobHandle.h"
#include "sage/jobs/ParallelFor.h"
#include "sage/jobs/JobStats.h"
#include "sage/jobs/TaskGroup.h"

// ---------------------------------------------------------------------------
// JobSystem — ЕДИНАЯ система задач движка (одна на EngineContext).
//
// Все параллельные работы движка, редактора и игры идут через неё: кадровый
// счёт (отсечение, системы ECS, анимация), физика Jolt, декодирование текстур,
// импорт и запекание, задачи скриптов, миниатюры редактора. Своих пулов
// потоков у подсистем нет — иначе N пулов по «ядра − 1» потоков каждый
// дерутся за одни и те же ядра, и ни один не знает о другом.
//
// УСТРОЙСТВО (подробно — docs/multithreading.md):
//   • воркеры (аппаратные потоки − 1) с деками Чейза–Лева: своя подзадача —
//     в свой дек, свободный воркер крадёт у занятого;
//   • общие очереди по приоритетам для задач извне; низкий приоритет не
//     голодает (каждая N-я выборка начинается с него);
//   • долгие задачи (Lane::Background) занимают не больше BackgroundLimit
//     воркеров разом — кадру всегда остаются ядра;
//   • ввод-вывод (Lane::IO) — на своих потоках: ожидание диска не держит ядро;
//     число потоков растёт до MaxIoThreads, когда все заняты;
//   • главный поток и поток рендера — очереди, которые они сами разбирают раз
//     в кадр (PumpMainThread / PumpRenderThread);
//   • зависимости — счётчиком невыполненных предшественниц; задача может
//     зависеть только от уже созданных, поэтому цикл в графе задач
//     невозможен по построению;
//   • ожидание помогает: ждущий воркер или главный поток выполняет чужие
//     кадровые задачи, поэтому вложенный ParallelFor не может «съесть» все
//     потоки и повиснуть.
//
// Если система не поднята явно (Application делает это из EngineConfig),
// первая же постановка поднимает её с авто-числом потоков. После Shutdown
// новые задачи отклоняются (завершаются как Canceled), ParallelFor идёт
// последовательно на вызывающем потоке.
// ---------------------------------------------------------------------------
namespace sage {

namespace jobs {

struct JobSystemConfig {
    int WorkerThreads = 0;        // 0 — авто: аппаратные потоки − 1 (главный тоже считает)
    int IoThreads = 0;            // 0 — авто: 2 (1 на одноядерном)
    int MaxIoThreads = 0;         // 0 — авто: max(IoThreads, 8)
    int BackgroundLimit = 0;      // 0 — авто: половина воркеров (минимум 1)
    double HungJobMs = 2000.0;    // порог «зависшей» кадровой задачи
    double HungBackgroundMs = 120000.0; // для Background/IO
    bool Enabled = true;          // false — ParallelFor последовательно
};

} // namespace jobs

class JobSystem {
public:
    JobSystem();
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Система текущего EngineContext.
    static JobSystem& Get();
    // То же, но nullptr без контекста движка (утилита командной строки,
    // инструмент сборки): вызывающий выполняет работу сам, последовательно.
    static JobSystem* TryGet();

    // --- Жизненный цикл ----------------------------------------------------
    // workerThreads: 0 — авто, >0 — точное число воркеров. Повторный вызов с
    // тем же числом — ничего; с другим — остановка и подъём заново. Вызвавший
    // поток считается главным (и потоком рендера, пока не сказано иное).
    void Initialize(int workerThreads = 0);
    void Initialize(const jobs::JobSystemConfig& config);
    // Предсказуемая остановка: новые задачи отклоняются; ещё не начатые
    // отменяются (их Future сообщат Canceled); идущие видят запрос отмены и
    // дорабатывают; потоки присоединяются. Задачи главного потока и рендера,
    // оставшиеся в очередях, тоже отменяются. Идемпотентна.
    void Shutdown();
    bool IsRunning() const;

    void SetEnabled(bool enabled);
    bool Enabled() const { return m_enabled; }

    int WorkerCount() const;
    int ThreadCount() const { return WorkerCount() + 1; } // эффективный параллелизм кадра
    int IoThreadCount() const;
    const jobs::JobSystemConfig& Config() const { return m_config; }

    // --- Потоки -------------------------------------------------------------
    bool IsMainThread() const;
    bool IsRenderThread() const;
    bool IsWorkerThread() const;
    // Поток графического контекста. По умолчанию — главный.
    void SetRenderThreadToCurrent();
    // Проверка «этот вызов разрешён только в потоке рендера» — для RHI. Ложь
    // (и запись в диагностику) — вызов не оттуда.
    bool CheckRenderThread(const char* what) const;

    // --- Постановка задач ---------------------------------------------------
    // Задача с результатом: auto f = js.Async([]{ return Decode(file); },
    //     {"decode", Category::Resources, Lane::IO});
    template <class F>
    auto Async(F&& f, jobs::JobOptions opts = {}) {
        return jobs::detail::MakeAndSubmit(CoreForSubmit(), std::forward<F>(f), opts);
    }
    template <class F>
    jobs::JobHandle Run(F&& f, jobs::JobOptions opts = {}) {
        return Async(std::forward<F>(f), std::move(opts));
    }
    // Пустая задача, завершающаяся после всех deps (точка сборки графа).
    jobs::JobHandle WhenAll(std::vector<jobs::JobHandle> deps, jobs::JobOptions opts = {});
    // Множество мелких операций одним заданием: items режутся на куски по
    // perJob (0 — авто) и выполняются как ParallelFor, но асинхронно.
    jobs::JobHandle Batch(std::vector<std::function<void()>> items, jobs::JobOptions opts = {},
                          std::size_t perJob = 0);

    // --- Ожидание и помощь --------------------------------------------------
    // Выполнить одну готовую кадровую задачу на этом потоке. false — нечего.
    bool HelpOnce();

    // --- Главный поток и рендер ---------------------------------------------
    // Разобрать очередь главного потока / потока рендера. budgetMs > 0 —
    // остановиться, исчерпав бюджет (остаток — в следующем кадре): сотня
    // накопившихся загрузок не должна дать один кадр в полсекунды.
    int PumpMainThread(double budgetMs = 0.0);
    int PumpRenderThread(double budgetMs = 0.0);

    // --- Параллельные проходы -----------------------------------------------
    // body(begin, end) для непересекающихся кусков [0, count). Блокирует до
    // конца (вызывающий поток работает наравне с воркерами). Тело трогает
    // только свой кусок. Исключение любого куска пробрасывается после того,
    // как ВСЕ куски закончили (body по ссылке не переживает вызов).
    void ParallelFor(std::size_t count,
                     const std::function<void(std::size_t begin, std::size_t end)>& body,
                     std::size_t minPerThread = 256);
    void ParallelFor(std::size_t count,
                     const std::function<void(std::size_t begin, std::size_t end)>& body,
                     const jobs::ParallelOptions& options);
    // То же без блокировки: тело копируется в задачу.
    jobs::JobHandle ParallelForAsync(std::size_t count,
                                     std::function<void(std::size_t begin, std::size_t end)> body,
                                     const jobs::ParallelOptions& options = {});
    // Разнородные независимые задачи; ждёт все (первая — на вызывающем).
    void Dispatch(const std::vector<std::function<void()>>& tasks);

    // --- Диагностика --------------------------------------------------------
    jobs::JobSystemStats Stats() const;
    void ResetStats();
    // Шкала времени задач (для профилировщика). Пока выключена — не стоит ничего.
    void SetTracing(bool on);
    bool Tracing() const;
    // События шкалы за последние windowMs миллисекунд.
    std::vector<jobs::TraceEvent> Trace(double windowMs = 100.0) const;
    // Время от запуска системы (общая шкала Stats/Trace).
    double NowMs() const;

    // Внутреннее: ядро для постановки (лениво поднимает систему).
    jobs::detail::Core* CoreForSubmit();
    jobs::detail::Core* CoreOrNull() const;

private:
    std::unique_ptr<jobs::detail::Core> m_core;
    jobs::JobSystemConfig m_config;
    bool m_enabled = true;
    bool m_wasShutDown = false;
};

namespace jobs {
// Короткие имена: sage::jobs::Async(...) — то же, что JobSystem::Get().Async(...).
template <class F>
auto Async(F&& f, JobOptions opts = {}) {
    return JobSystem::Get().Async(std::forward<F>(f), std::move(opts));
}
template <class F>
JobHandle Run(F&& f, JobOptions opts = {}) {
    return JobSystem::Get().Run(std::forward<F>(f), std::move(opts));
}
inline JobHandle WhenAll(std::vector<JobHandle> deps, JobOptions opts = {}) {
    return JobSystem::Get().WhenAll(std::move(deps), std::move(opts));
}
// Выполнить на главном потоке (в ближайшем PumpMainThread). Безопасная
// передача результата из фоновой задачи в интерфейс/сцену/Lua.
template <class F>
JobHandle PostToMain(F&& f, const char* name = "main", Category cat = Category::General) {
    return Run(std::forward<F>(f), JobOptions(name, cat, Lane::Main));
}
template <class F>
JobHandle PostToRender(F&& f, const char* name = "render", Category cat = Category::Render) {
    return Run(std::forward<F>(f), JobOptions(name, cat, Lane::Render));
}
} // namespace jobs

} // namespace sage
