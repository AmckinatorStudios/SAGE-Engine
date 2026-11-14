#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

// ---------------------------------------------------------------------------
// Базовые типы системы задач: куда задача идёт (полоса), насколько она срочна
// (приоритет), чья она (подсистема — для профилировщика) и как её отменить.
//
// ПОЧЕМУ ПОЛОСЫ, А НЕ ОДНА ОЧЕРЕДЬ. Задачи различаются не срочностью, а
// ПРИРОДОЙ, и одна очередь смешивает несовместимое:
//   • кадровая работа (отсечение, анимация, системы ECS) короткая и нужна
//     к концу кадра — её нельзя ставить за секундный бейк света;
//   • долгий счёт (запекание, сжатие, задачи скриптов) обязан идти, но не
//     имеет права занять все ядра — иначе кадр ждёт;
//   • ввод-вывод (чтение файла, ожидание процесса) не считает, а ЖДЁТ — на
//     ядре он только держит поток, поэтому живёт на отдельных потоках;
//   • главный поток и поток рендера — единственные, кому можно трогать окно,
//     ImGui, Lua-сцену и графический контекст.
// Планирование, приоритеты, зависимости и диагностика при этом ОДНИ на все
// полосы: задача на главный поток может ждать задачу ввода-вывода, а та —
// задачу воркера, и это один граф.
// ---------------------------------------------------------------------------
namespace sage::jobs {

enum class Lane : uint8_t {
    Worker = 0,  // короткий счёт кадра; воркеры с кражей работы
    Background,  // долгий счёт; воркеры, но не больше доли от них разом
    IO,          // блокирующий ввод-вывод и ожидания; свои потоки
    Main,        // главный поток (PumpMainThread раз в кадр)
    Render,      // поток графического контекста (PumpRenderThread)
    Count
};

enum class Priority : uint8_t { High = 0, Normal, Low, Count };

// Состояние задачи. Переходы только вперёд: Waiting → Queued → Running → итог.
enum class JobStatus : uint8_t {
    Waiting = 0,  // ждёт зависимостей
    Queued,       // готова, стоит в очереди полосы
    Running,
    Succeeded,
    Failed,       // выбросила исключение (или его унаследовала от зависимости)
    Canceled,     // отменена до запуска, отменена её зависимость или система остановлена
};

inline bool IsFinal(JobStatus s) { return s >= JobStatus::Succeeded; }

// Подсистема — для профилировщика: «чья» нагрузка на потоках. Свой код игры
// пишет Game или General; имя задачи (JobOptions::Name) уточняет подробности.
enum class Category : uint8_t {
    General = 0,
    ECS,
    Render,
    Physics,
    Animation,
    Audio,
    Scripts,
    Resources,
    AssetImport,
    IO,
    AI,
    Editor,
    Network,
    Game,
    Count
};

const char* LaneName(Lane lane);
const char* PriorityName(Priority p);
const char* StatusName(JobStatus s);
const char* CategoryName(Category c);

// --- Отмена ------------------------------------------------------------------
//
// Источник отмены раздаёт жетоны; задача проверяет жетон перед запуском (тогда
// она завершается как Canceled, не начавшись) и может проверять его сама во
// время долгой работы (jobs::ThisJob::IsCancelled()). Насильно остановить
// идущий код нельзя ни в одном языке без порчи данных — поэтому отмена
// кооперативная.
struct CancelState {
    std::atomic<bool> Requested{false};
};

class CancellationToken {
public:
    CancellationToken() = default;
    bool IsCancelled() const {
        return m_state && m_state->Requested.load(std::memory_order_acquire);
    }
    bool CanBeCancelled() const { return (bool)m_state; }
    const std::shared_ptr<CancelState>& State() const { return m_state; }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<CancelState> s) : m_state(std::move(s)) {}
    std::shared_ptr<CancelState> m_state;
};

class CancellationSource {
public:
    CancellationSource() : m_state(std::make_shared<CancelState>()) {}
    void Cancel() { m_state->Requested.store(true, std::memory_order_release); }
    bool IsCancelled() const { return m_state->Requested.load(std::memory_order_acquire); }
    CancellationToken Token() const { return CancellationToken(m_state); }

private:
    std::shared_ptr<CancelState> m_state;
};

// Исключение, которое Future::Get бросает для отменённой задачи: «значения не
// будет» — это не «значение по умолчанию», и молча вернуть T{} было бы враньём.
struct JobCanceledError;

} // namespace sage::jobs
