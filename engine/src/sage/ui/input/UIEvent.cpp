#include "sage/ui/input/UIEvent.h"

#include <algorithm>

namespace sage::ui {

const char* const* UIEventTypeNames() {
    static const char* n[] = {"PointerEnter", "PointerExit", "PointerMove", "PointerDown",
                              "PointerUp",    "Click",       "DoubleClick", "Scroll",
                              "DragStart",    "Drag",        "DragEnd",     "Drop",
                              "Focus",        "Blur",        "KeyDown",     "KeyUp",
                              "TextInput",    "ValueChanged","Submit",      "Cancel"};
    return n;
}
int UIEventTypeCount() { return 20; }

int UIEventBus::On(UINodeId node, UIEventType type, UIEventHandler handler) {
    Sub s;
    s.Id = m_nextId++;
    s.Node = node;
    s.Type = type;
    s.Handler = std::move(handler);
    m_subs.push_back(std::move(s));
    return m_subs.back().Id;
}

int UIEventBus::OnAny(UIEventHandler handler) {
    Sub s;
    s.Id = m_nextId++;
    s.Any = true;
    s.Handler = std::move(handler);
    m_subs.push_back(std::move(s));
    return m_subs.back().Id;
}

void UIEventBus::Off(int subscription) {
    m_subs.erase(std::remove_if(m_subs.begin(), m_subs.end(),
                                [&](const Sub& s) { return s.Id == subscription; }),
                 m_subs.end());
}

void UIEventBus::Clear() { m_subs.clear(); }

void UIEventBus::Dispatch(UIEvent& e) const {
    // Копия списка не нужна: обработчик, который подписывается прямо во время
    // разбора, не должен получить своё же событие — иначе «кнопка, создающая
    // кнопку» срабатывает дважды. Проходим по индексам и по РАЗМЕРУ НА ВХОДЕ.
    const size_t count = m_subs.size();
    for (size_t i = 0; i < count && i < m_subs.size(); ++i) {
        const Sub& s = m_subs[i];
        if (!s.Handler) continue;
        if (!s.Any) {
            if (s.Node != e.Current) continue;
            if (s.Type != e.Type) continue;
        }
        s.Handler(e);
        if (e.Handled) return;
    }
}

} // namespace sage::ui
