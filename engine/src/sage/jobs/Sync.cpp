#include "sage/jobs/Sync.h"

#include <algorithm>
#include <map>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace sage::sync {

void CpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
}

// --- Реестр замков -----------------------------------------------------------
//
// Живые замки — в двусвязном списке (снимок суммирует их счётчики), умершие
// сливают свои числа в итог по имени: иначе замок, живущий секунду (кэш
// загрузки сцены), исчезал бы из панели вместе со своими ожиданиями.
namespace {
struct Totals {
    uint64_t Acq = 0, Cont = 0, WaitNs = 0, MaxNs = 0;
};
struct Registry {
    std::mutex M;
    LockCounters* Head = nullptr;
    std::map<std::string, Totals> Dead;
};
Registry& Reg() {
    // Намеренная утечка: замки бывают статическими объектами, и их деструкторы
    // идут при выходе процесса в произвольном порядке относительно реестра.
    static Registry* r = new Registry();
    return *r;
}
} // namespace

void LockCounters::RecordWait(uint64_t ns) {
    Contended.fetch_add(1, std::memory_order_relaxed);
    WaitNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = MaxWaitNs.load(std::memory_order_relaxed);
    while (ns > prev && !MaxWaitNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

namespace detail {

uint64_t NowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void RegisterCounters(LockCounters* c) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lk(r.M);
    c->Prev = nullptr;
    c->Next = r.Head;
    if (r.Head) r.Head->Prev = c;
    r.Head = c;
}

void UnregisterCounters(LockCounters* c) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lk(r.M);
    if (c->Prev) c->Prev->Next = c->Next;
    else r.Head = c->Next;
    if (c->Next) c->Next->Prev = c->Prev;
    Totals& t = r.Dead[c->Name ? c->Name : "lock"];
    t.Acq += c->Acquisitions.load(std::memory_order_relaxed);
    t.Cont += c->Contended.load(std::memory_order_relaxed);
    t.WaitNs += c->WaitNs.load(std::memory_order_relaxed);
    t.MaxNs = std::max(t.MaxNs, c->MaxWaitNs.load(std::memory_order_relaxed));
}

} // namespace detail

std::vector<LockStats> CollectLockStats() {
    Registry& r = Reg();
    std::map<std::string, LockStats> byName;
    std::lock_guard<std::mutex> lk(r.M);
    for (const auto& kv : r.Dead) {
        LockStats& s = byName[kv.first];
        s.Name = kv.first;
        s.Acquisitions += kv.second.Acq;
        s.Contended += kv.second.Cont;
        s.WaitMs += (double)kv.second.WaitNs / 1e6;
        s.MaxWaitMs = std::max(s.MaxWaitMs, (double)kv.second.MaxNs / 1e6);
    }
    for (LockCounters* c = r.Head; c; c = c->Next) {
        LockStats& s = byName[c->Name ? c->Name : "lock"];
        s.Name = c->Name ? c->Name : "lock";
        s.Acquisitions += c->Acquisitions.load(std::memory_order_relaxed);
        s.Contended += c->Contended.load(std::memory_order_relaxed);
        s.WaitMs += (double)c->WaitNs.load(std::memory_order_relaxed) / 1e6;
        s.MaxWaitMs = std::max(s.MaxWaitMs, (double)c->MaxWaitNs.load(std::memory_order_relaxed) / 1e6);
        ++s.Instances;
    }
    std::vector<LockStats> out;
    out.reserve(byName.size());
    for (auto& kv : byName) out.push_back(std::move(kv.second));
    // Сначала те, кто ждал дольше всех, — их и ищут.
    std::sort(out.begin(), out.end(),
              [](const LockStats& a, const LockStats& b) { return a.WaitMs > b.WaitMs; });
    return out;
}

void ResetLockStats() {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lk(r.M);
    r.Dead.clear();
    for (LockCounters* c = r.Head; c; c = c->Next) {
        c->Acquisitions.store(0, std::memory_order_relaxed);
        c->Contended.store(0, std::memory_order_relaxed);
        c->WaitNs.store(0, std::memory_order_relaxed);
        c->MaxWaitNs.store(0, std::memory_order_relaxed);
    }
}

// --- Semaphore ---------------------------------------------------------------

void Semaphore::Release(int n) {
    if (n <= 0) return;
    {
        std::lock_guard<std::mutex> lk(m_m);
        m_count.fetch_add(n, std::memory_order_release);
    }
    if (n == 1) m_cv.notify_one();
    else m_cv.notify_all();
}

bool Semaphore::TryAcquire() {
    int c = m_count.load(std::memory_order_relaxed);
    while (c > 0) {
        if (m_count.compare_exchange_weak(c, c - 1, std::memory_order_acquire)) return true;
    }
    return false;
}

void Semaphore::Acquire() {
    if (TryAcquire()) return;
    std::unique_lock<std::mutex> lk(m_m);
    m_cv.wait(lk, [&] { return TryAcquire(); });
}

bool Semaphore::TryAcquireFor(std::chrono::microseconds timeout) {
    if (TryAcquire()) return true;
    std::unique_lock<std::mutex> lk(m_m);
    return m_cv.wait_for(lk, timeout, [&] { return TryAcquire(); });
}

// --- Event -------------------------------------------------------------------

void Event::Set() {
    {
        std::lock_guard<std::mutex> lk(m_m);
        m_set.store(true, std::memory_order_release);
    }
    if (m_mode == Mode::ManualReset) m_cv.notify_all();
    else m_cv.notify_one();
}

void Event::Reset() {
    std::lock_guard<std::mutex> lk(m_m);
    m_set.store(false, std::memory_order_release);
}

void Event::Wait() {
    std::unique_lock<std::mutex> lk(m_m);
    m_cv.wait(lk, [&] { return m_set.load(std::memory_order_acquire); });
    if (m_mode == Mode::AutoReset) m_set.store(false, std::memory_order_release);
}

bool Event::WaitFor(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lk(m_m);
    if (!m_cv.wait_for(lk, timeout, [&] { return m_set.load(std::memory_order_acquire); }))
        return false;
    if (m_mode == Mode::AutoReset) m_set.store(false, std::memory_order_release);
    return true;
}

// --- Latch -------------------------------------------------------------------

void Latch::CountDown(int n) {
    if (m_count.fetch_sub(n, std::memory_order_acq_rel) - n <= 0) {
        std::lock_guard<std::mutex> lk(m_m);
        m_cv.notify_all();
    }
}

void Latch::Wait() {
    if (TryWait()) return;
    std::unique_lock<std::mutex> lk(m_m);
    m_cv.wait(lk, [&] { return TryWait(); });
}

bool Latch::WaitFor(std::chrono::microseconds timeout) {
    if (TryWait()) return true;
    std::unique_lock<std::mutex> lk(m_m);
    return m_cv.wait_for(lk, timeout, [&] { return TryWait(); });
}

// --- Barrier -----------------------------------------------------------------

void Barrier::CompletePhaseLocked() {
    if (m_onPhase) m_onPhase();
    m_arrived = 0;
    ++m_phase;
    m_cv.notify_all();
}

void Barrier::ArriveAndWait() {
    std::unique_lock<std::mutex> lk(m_m);
    const uint64_t phase = m_phase;
    if (++m_arrived >= m_expected) {
        CompletePhaseLocked();
        return;
    }
    m_cv.wait(lk, [&] { return m_phase != phase; });
}

void Barrier::ArriveAndDrop() {
    std::unique_lock<std::mutex> lk(m_m);
    --m_expected;
    if (m_expected > 0 && m_arrived >= m_expected) CompletePhaseLocked();
}

uint64_t Barrier::Phase() const {
    std::lock_guard<std::mutex> lk(m_m);
    return m_phase;
}

} // namespace sage::sync
