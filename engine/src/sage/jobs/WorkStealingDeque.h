#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Дек Чейза–Лева с кражей работы (Chase & Lev 2005; модель памяти C11 — по
// Lê, Pop, Cohen, Zappa Nardelli, «Correct and Efficient Work-Stealing for
// Weak Memory Models», PPoPP 2013).
//
// Владелец (свой воркер) кладёт и берёт с НИЗА — LIFO: только что порождённая
// подзадача ещё горячая в кэше. Воры берут с ВЕРХА — FIFO: самую старую, то
// есть обычно самую крупную (родитель дробит работу сверху вниз). Владелец и
// воры встречаются на одном элементе только когда он последний — там спор
// решает один compare_exchange; в остальное время замков нет вовсе.
//
// Рост — удвоением кольца. Старое кольцо НЕ освобождается до разрушения дека:
// вор мог прочитать указатель на него за мгновение до замены и ещё читает
// элемент. Памяти это стоит ограниченно (сумма степеней двойки < 2 × размер).
// ---------------------------------------------------------------------------
namespace sage::jobs::detail {

template <class T>
class WorkStealingDeque {
    static_assert(std::is_pointer<T>::value, "дек хранит указатели");

public:
    explicit WorkStealingDeque(int64_t capacity = 256) {
        int64_t cap = 1;
        while (cap < capacity) cap <<= 1;
        m_rings.push_back(std::make_unique<Ring>(cap));
        m_ring.store(m_rings.back().get(), std::memory_order_relaxed);
    }

    // Только владелец.
    void Push(T item) {
        const int64_t b = m_bottom.load(std::memory_order_relaxed);
        const int64_t t = m_top.load(std::memory_order_acquire);
        Ring* r = m_ring.load(std::memory_order_relaxed);
        if (b - t > r->Capacity - 1) r = Grow(r, b, t);
        r->Put(b, item);
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(b + 1, std::memory_order_relaxed);
    }

    // Только владелец. nullptr — пусто (или последний элемент увёл вор).
    T Pop() {
        const int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        Ring* r = m_ring.load(std::memory_order_relaxed);
        m_bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = m_top.load(std::memory_order_relaxed);
        if (t > b) {  // было пусто
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return nullptr;
        }
        T item = r->Get(b);
        if (t == b) {  // последний элемент — спорим с ворами
            if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                               std::memory_order_relaxed))
                item = nullptr;
            m_bottom.store(b + 1, std::memory_order_relaxed);
        }
        return item;
    }

    // Любой поток. nullptr — пусто или проиграли гонку (можно попробовать снова).
    T Steal() {
        int64_t t = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t b = m_bottom.load(std::memory_order_acquire);
        if (t >= b) return nullptr;
        Ring* r = m_ring.load(std::memory_order_acquire);
        T item = r->Get(t);
        if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                           std::memory_order_relaxed))
            return nullptr;
        return item;
    }

    // Приблизительно (для статистики и эвристик «есть ли что красть»).
    int64_t SizeApprox() const {
        const int64_t b = m_bottom.load(std::memory_order_relaxed);
        const int64_t t = m_top.load(std::memory_order_relaxed);
        return b > t ? b - t : 0;
    }
    bool EmptyApprox() const { return SizeApprox() == 0; }

private:
    struct Ring {
        explicit Ring(int64_t cap) : Capacity(cap), Mask(cap - 1), Slots(new std::atomic<T>[cap]) {}
        int64_t Capacity;
        int64_t Mask;
        std::unique_ptr<std::atomic<T>[]> Slots;
        void Put(int64_t i, T v) { Slots[i & Mask].store(v, std::memory_order_relaxed); }
        T Get(int64_t i) const { return Slots[i & Mask].load(std::memory_order_relaxed); }
    };

    Ring* Grow(Ring* old, int64_t b, int64_t t) {
        auto bigger = std::make_unique<Ring>(old->Capacity * 2);
        for (int64_t i = t; i < b; ++i) bigger->Put(i, old->Get(i));
        Ring* raw = bigger.get();
        m_rings.push_back(std::move(bigger));  // старое кольцо живо до конца (см. шапку)
        m_ring.store(raw, std::memory_order_release);
        return raw;
    }

    alignas(64) std::atomic<int64_t> m_top{0};
    alignas(64) std::atomic<int64_t> m_bottom{0};
    alignas(64) std::atomic<Ring*> m_ring{nullptr};
    std::vector<std::unique_ptr<Ring>> m_rings;  // трогает только владелец
};

} // namespace sage::jobs::detail
