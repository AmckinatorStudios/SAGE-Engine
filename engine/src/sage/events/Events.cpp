#include "sage/events/Events.h"

#include <algorithm>

#include "sage/core/Log.h"

namespace sage::events {

namespace {
// Два обработчика, шлющие событие друг другу, иначе роняют движок
// переполнением стека C++ — то есть ошибка в НАСТРОЙКЕ кнопки убивает игру без
// единого понятного слова в логе.
constexpr int kMaxDepth = 16;
// Столько проходов делает разбор очереди — см. DispatchQueued.
constexpr int kMaxQueuePasses = 8;
} // namespace

int Bus::On(const std::string& name, Handler handler) {
    if (!handler) return 0;
    const int id = m_next++;
    m_slots.push_back(Slot{id, name, std::move(handler), false, false});
    return id;
}

int Bus::Once(const std::string& name, Handler handler) {
    if (!handler) return 0;
    const int id = m_next++;
    m_slots.push_back(Slot{id, name, std::move(handler), true, false});
    return id;
}

int Bus::OnAny(Handler handler) {
    if (!handler) return 0;
    const int id = m_next++;
    Slot s{id, std::string(), std::move(handler), false, false};
    s.Any = true;
    m_slots.push_back(std::move(s));
    return id;
}

void Bus::Off(int id) {
    for (Slot& s : m_slots)
        if (s.Id == id) s.Dead = true;
}

void Bus::OffAll(const std::string& name) {
    for (Slot& s : m_slots)
        if (s.Name == name) s.Dead = true;
}

void Bus::Clear() {
    // Пометкой, а не очисткой: Clear могут позвать ИЗ обработчика (скрипт
    // просит сменить уровень), и стирание вектора под ногами у Emit — это
    // висячая ссылка.
    for (Slot& s : m_slots) s.Dead = true;
    // Неразобранные события выгруженного уровня не должны догнать следующий:
    // «дверь открылась» из прошлой сцены в новой означает чужую дверь.
    m_queued.clear();
    m_deferred.clear();
    if (!m_sweeping && m_depth == 0) m_slots.clear();
}

void Bus::Emit(const Event& event) {
    if (m_depth >= kMaxDepth) {
        LOG_ERROR("Events") << "Превышена глубина вложенной рассылки (" << kMaxDepth
                            << ") на событии '" << event.Name
                            << "' — обработчики шлют события друг другу по кругу";
        return;
    }
    ++m_depth;

    // Снимок ЖИВЫХ подписчиков: обработчик волен подписаться, отписаться и
    // уничтожить объект — любое из этого меняет вектор под итератором.
    std::vector<Handler> snapshot;
    std::vector<int> onceIds;
    for (const Slot& s : m_slots) {
        if (s.Dead) continue;
        if (!s.Any && s.Name != event.Name) continue;
        snapshot.push_back(s.Fn);
        if (s.Once) onceIds.push_back(s.Id);
    }
    for (int id : onceIds) Off(id);   // до вызова: обработчик может слать то же событие
    for (const Handler& fn : snapshot) fn(event);

    --m_depth;

    // Уборка мёртвых — только на верхнем уровне рассылки: внутри вложенной
    // чужой снимок ещё может ссылаться на слот.
    if (m_depth == 0 && !m_sweeping) {
        m_sweeping = true;
        m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
                                     [](const Slot& s) { return s.Dead; }),
                      m_slots.end());
        m_sweeping = false;
    }
}

void Bus::Queue(const std::string& name, const sage::vars::Value& arg, int sender) {
    Event e;
    e.Name = name;
    e.Arg = arg;
    e.Sender = sender;
    Queue(e);
}

void Bus::Defer(const std::string& name, const sage::vars::Value& arg, int sender) {
    Event e;
    e.Name = name;
    e.Arg = arg;
    e.Sender = sender;
    Defer(e);
}

void Bus::DispatchQueued() {
    // Очередь забирается ЦЕЛИКОМ на каждый проход: обработчик волен положить в
    // неё новое событие, а дописывание в вектор, по которому идёт цикл, — это
    // переаллокация под ногами у итератора.
    //
    // Проходов несколько, потому что цепочка «событие -> условие -> действие ->
    // событие» (§17 ТЗ) обязана доиграться в ЭТОМ кадре: разложенная по кадрам,
    // она превращает открытие двери в лестницу задержек. Но и не бесконечно:
    // два события, кладущих друг друга в очередь, иначе зациклят кадр насмерть.
    for (int pass = 0; pass < kMaxQueuePasses && !m_queued.empty(); ++pass) {
        std::vector<Event> batch;
        batch.swap(m_queued);
        for (const Event& e : batch) Emit(e);
    }
    if (!m_queued.empty()) {
        LOG_ERROR("Events") << "Очередь событий не разобралась за " << kMaxQueuePasses
                            << " проходов — события кладут друг друга по кругу; осталось "
                            << m_queued.size();
        m_queued.clear();
    }
}

void Bus::DispatchDeferred() {
    std::vector<Event> batch;
    batch.swap(m_deferred);
    for (const Event& e : batch) Emit(e);
}

int Bus::Count(const std::string& name) const {
    int n = 0;
    for (const Slot& s : m_slots)
        if (!s.Dead && !s.Any && s.Name == name) ++n;
    return n;
}

std::vector<std::string> Bus::Names() const {
    std::vector<std::string> out;
    for (const Slot& s : m_slots) {
        if (s.Dead || s.Any || s.Name.empty()) continue;
        if (std::find(out.begin(), out.end(), s.Name) == out.end()) out.push_back(s.Name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const Binding*> ForTrigger(const Bindings& bindings, const std::string& trigger) {
    std::vector<const Binding*> out;
    for (const Binding& b : bindings) {
        if (!b.Enabled) continue;
        // Пустой триггер — «на любое»: связь, у которой забыли выбрать момент,
        // должна срабатывать заметно, а не молчать.
        if (!b.Trigger.empty() && b.Trigger != trigger) continue;
        out.push_back(&b);
    }
    return out;
}

const std::vector<std::string>& UITriggers() {
    static const std::vector<std::string> t = {"click",   "press",    "release",
                                               "hoverIn", "hoverOut", "change"};
    return t;
}

} // namespace sage::events
