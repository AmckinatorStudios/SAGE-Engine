#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Примитивы синхронизации движка — ОДИН набор на весь код.
//
// ПРАВИЛА (полностью — docs/multithreading.md, «Синхронизация»):
//   1. Сначала — не делить данные: каждый поток пишет своё (ParallelFor по
//      непересекающимся отрезкам), результат сливается у вызывающего.
//   2. Один счётчик или флаг — std::atomic, не замок.
//   3. Короткая секция в несколько инструкций без вызовов наружу — SpinLock.
//   4. Обычная секция — sync::Mutex (std::mutex + учёт ожиданий для
//      профилировщика: имя замка видно в панели «Потоки»).
//   5. Много читателей, редкий писатель — sync::SharedMutex.
//   6. «Дождаться N событий» — Latch; «все потоки дошли до шага» — Barrier;
//      «разрешено не больше N разом» — Semaphore; «случилось / не случилось» —
//      Event. Своих condition_variable с флагом рядом не заводить: это и есть
//      Event, только с багом потерянного пробуждения.
//   7. Под замком не ждать задачу (Wait/Get) и не звать чужой код — это
//      готовый deadlock: ждущий держит замок, которого ждёт задача.
//   8. Воркер не засыпает на примитиве, если может помочь: ожидание задачи
//      (JobHandle::Wait, TaskGroup::Wait) само исполняет чужие задачи, пока
//      ждёт. Блокирующие ожидания примитивов — для потоков ввода-вывода и
//      для синхронизации ВНЕ системы задач.
// ---------------------------------------------------------------------------
namespace sage::sync {

// Пауза в цикле ожидания: подсказка ядру, что мы крутимся (освобождает ресурсы
// соседнему гиперпотоку и не мешает тому, кто держит замок).
void CpuRelax();

// --- SpinLock: замок на несколько инструкций --------------------------------
// TTAS: сначала читаем (без записи в линию кэша), и только увидев свободным —
// пробуем захватить. После нескольких сотен оборотов уступает квант (yield):
// крутиться дальше значит отнимать ядро у того, кто держит замок.
class SpinLock {
public:
    void lock() {
        for (int spins = 0;; ++spins) {
            if (!m_flag.exchange(true, std::memory_order_acquire)) return;
            while (m_flag.load(std::memory_order_relaxed)) {
                if (++spins < 256) CpuRelax();
                else std::this_thread::yield();
            }
        }
    }
    bool try_lock() {
        return !m_flag.load(std::memory_order_relaxed) &&
               !m_flag.exchange(true, std::memory_order_acquire);
    }
    void unlock() { m_flag.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_flag{false};
};

// --- Учёт замков для профилировщика -----------------------------------------
struct LockStats {
    std::string Name;
    uint64_t Acquisitions = 0;  // всего захватов
    uint64_t Contended = 0;     // из них пришлось ждать
    double WaitMs = 0.0;        // суммарное ожидание
    double MaxWaitMs = 0.0;     // самое долгое одиночное ожидание
    int Instances = 0;          // живых замков с этим именем
};

// Счётчики одного замка. Лежат В САМОМ замке (рядом с его линией кэша, которую
// захват и так гоняет между ядрами), а не в общей таблице по имени — иначе
// счётчик сам стал бы точкой конкуренции.
struct LockCounters {
    std::atomic<uint64_t> Acquisitions{0};
    std::atomic<uint64_t> Contended{0};
    std::atomic<uint64_t> WaitNs{0};
    std::atomic<uint64_t> MaxWaitNs{0};
    const char* Name = "lock";
    LockCounters* Prev = nullptr;
    LockCounters* Next = nullptr;
    void RecordWait(uint64_t ns);
};

// Сводка по всем именованным замкам (живым и уже разрушенным) — по имени.
std::vector<LockStats> CollectLockStats();
void ResetLockStats();

namespace detail {
void RegisterCounters(LockCounters* c);
void UnregisterCounters(LockCounters* c);
uint64_t NowNs();
} // namespace detail

// --- Mutex: std::mutex + учёт ожиданий --------------------------------------
// Быстрый путь — try_lock без часов; часы включаются только когда замок занят,
// то есть платит лишь тот, кто и так ждёт.
class Mutex {
public:
    explicit Mutex(const char* name = "mutex") {
        m_c.Name = name;
        detail::RegisterCounters(&m_c);
    }
    ~Mutex() { detail::UnregisterCounters(&m_c); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        m_c.Acquisitions.fetch_add(1, std::memory_order_relaxed);
        if (m_m.try_lock()) return;
        const uint64_t t0 = detail::NowNs();
        m_m.lock();
        m_c.RecordWait(detail::NowNs() - t0);
    }
    bool try_lock() {
        if (!m_m.try_lock()) return false;
        m_c.Acquisitions.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void unlock() { m_m.unlock(); }
    std::mutex& Native() { return m_m; }

private:
    std::mutex m_m;
    LockCounters m_c;
};

// --- SharedMutex: много читателей, редкий писатель --------------------------
class SharedMutex {
public:
    explicit SharedMutex(const char* name = "shared_mutex") {
        m_c.Name = name;
        detail::RegisterCounters(&m_c);
    }
    ~SharedMutex() { detail::UnregisterCounters(&m_c); }
    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator=(const SharedMutex&) = delete;

    void lock() {
        m_c.Acquisitions.fetch_add(1, std::memory_order_relaxed);
        if (m_m.try_lock()) return;
        const uint64_t t0 = detail::NowNs();
        m_m.lock();
        m_c.RecordWait(detail::NowNs() - t0);
    }
    bool try_lock() { return m_m.try_lock(); }
    void unlock() { m_m.unlock(); }
    void lock_shared() {
        m_c.Acquisitions.fetch_add(1, std::memory_order_relaxed);
        if (m_m.try_lock_shared()) return;
        const uint64_t t0 = detail::NowNs();
        m_m.lock_shared();
        m_c.RecordWait(detail::NowNs() - t0);
    }
    bool try_lock_shared() { return m_m.try_lock_shared(); }
    void unlock_shared() { m_m.unlock_shared(); }

private:
    std::shared_mutex m_m;
    LockCounters m_c;
};

// --- Semaphore: не больше N разом --------------------------------------------
// Свой, а не std::counting_semaphore: тот есть лишь с C++20 и на macOS требует
// новой системы (availability), а движок собирается C++17 под три ОС.
class Semaphore {
public:
    explicit Semaphore(int initial = 0) : m_count(initial) {}
    void Release(int n = 1);
    void Acquire();
    bool TryAcquire();
    bool TryAcquireFor(std::chrono::microseconds timeout);
    int Available() const { return m_count.load(std::memory_order_relaxed); }

private:
    std::atomic<int> m_count;
    std::mutex m_m;
    std::condition_variable m_cv;
};

// --- Event: «случилось» -------------------------------------------------------
// ManualReset — остаётся поднятым до Reset (будит всех ждущих).
// AutoReset — пропускает ровно одного ждущего и опускается сам.
class Event {
public:
    enum class Mode { ManualReset, AutoReset };
    explicit Event(Mode mode = Mode::ManualReset, bool initiallySet = false)
        : m_mode(mode), m_set(initiallySet) {}
    void Set();
    void Reset();
    bool IsSet() const { return m_set.load(std::memory_order_acquire); }
    void Wait();
    bool WaitFor(std::chrono::microseconds timeout);

private:
    Mode m_mode;
    std::atomic<bool> m_set;
    std::mutex m_m;
    std::condition_variable m_cv;
};

// --- Latch: дождаться N отметок (одноразовый) ------------------------------------
class Latch {
public:
    explicit Latch(int count) : m_count(count) {}
    void CountDown(int n = 1);
    bool TryWait() const { return m_count.load(std::memory_order_acquire) <= 0; }
    void Wait();
    bool WaitFor(std::chrono::microseconds timeout);

private:
    std::atomic<int> m_count;
    std::mutex m_m;
    std::condition_variable m_cv;
};

// --- Barrier: N участников, многоразовый -------------------------------------
// onPhase (если задан) выполняется последним пришедшим ДО того, как остальных
// отпустят, — удобное место слить результаты фазы без отдельного замка.
class Barrier {
public:
    explicit Barrier(int participants, std::function<void()> onPhase = {})
        : m_expected(participants), m_onPhase(std::move(onPhase)) {}
    void ArriveAndWait();
    // Выйти из участников навсегда (поток закончил работу раньше остальных).
    void ArriveAndDrop();
    uint64_t Phase() const;

private:
    void CompletePhaseLocked();
    int m_expected;
    int m_arrived = 0;
    uint64_t m_phase = 0;
    std::function<void()> m_onPhase;
    mutable std::mutex m_m;
    std::condition_variable m_cv;
};

} // namespace sage::sync
