#include "sage/events/TypedBus.h"

#include <algorithm>

#include "sage/core/Log.h"

namespace sage::events {

namespace {
// Столько раз событие может породить событие внутри одной рассылки. Без
// предела ошибка в НАСТРОЙКЕ связи (два обработчика шлют друг другу) убивает
// игру переполнением стека — без единого понятного слова в логе.
constexpr int kMaxDepth = 16;
// Столько проходов делает разбор очереди. Цепочка «событие -> условие ->
// действие -> событие» обязана доиграться в этом же кадре, но цикл из двух
// событий, кладущих друг друга в очередь, обязан остановиться.
constexpr int kMaxQueuePasses = 8;
} // namespace

void TypedBus::Unsubscribe(int id) {
    for (Slot& s : m_slots)
        if (s.Id == id) s.Dead = true;
}

void TypedBus::Clear() {
    // Пометкой, а не очисткой: Clear зовут ИЗ обработчика (смена уровня), и
    // стирание вектора под ногами у рассылки — висячая ссылка.
    for (Slot& s : m_slots) s.Dead = true;
    m_queued.clear();
    m_deferred.clear();
    if (m_depth == 0) m_slots.clear();
}

void TypedBus::Dispatch(TypeId type, const void* payload) {
    if (m_depth >= kMaxDepth) {
        LOG_ERROR("Events") << "Превышена глубина вложенной рассылки (" << kMaxDepth
                            << ") — обработчики шлют события друг другу по кругу";
        return;
    }
    ++m_depth;

    // Снимок живых подписчиков: обработчик волен подписаться, отписаться и
    // уничтожить объект — любое из этого меняет вектор под итератором.
    std::vector<std::function<void(const void*)>> snapshot;
    std::vector<int> onceIds;
    for (const Slot& s : m_slots) {
        if (s.Dead || s.Type != type) continue;
        snapshot.push_back(s.Fn);
        if (s.Once) onceIds.push_back(s.Id);
    }
    for (int id : onceIds) Unsubscribe(id); // до вызова: обработчик может слать то же событие
    for (const auto& fn : snapshot) fn(payload);

    --m_depth;
    if (m_depth == 0) Sweep();
}

void TypedBus::Sweep() {
    m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
                                 [](const Slot& s) { return s.Dead; }),
                  m_slots.end());
}

void TypedBus::DispatchQueued() {
    for (int pass = 0; pass < kMaxQueuePasses && !m_queued.empty(); ++pass) {
        // Забираем очередь ЦЕЛИКОМ: обработчик волен положить в неё новое
        // событие, и дописывание в вектор, по которому идёт цикл, — это
        // переаллокация под ногами у итератора.
        std::vector<std::function<void()>> batch;
        batch.swap(m_queued);
        for (const auto& fn : batch) fn();
    }
    if (!m_queued.empty()) {
        LOG_ERROR("Events") << "Очередь событий не разобралась за " << kMaxQueuePasses
                            << " проходов — события кладут друг друга по кругу; "
                            << "осталось " << m_queued.size();
        m_queued.clear();
    }
}

void TypedBus::DispatchDeferred() {
    std::vector<std::function<void()>> batch;
    batch.swap(m_deferred);
    for (const auto& fn : batch) fn();
}

} // namespace sage::events
