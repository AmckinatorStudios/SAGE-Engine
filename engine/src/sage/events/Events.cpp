#include "sage/events/Events.h"

#include <algorithm>

#include "sage/core/Log.h"

namespace sage::events {

namespace {
// Два обработчика, шлющие событие друг другу, иначе роняют движок
// переполнением стека C++ — то есть ошибка в НАСТРОЙКЕ кнопки убивает игру без
// единого понятного слова в логе.
constexpr int kMaxDepth = 16;
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
