#include "sage/input/Context.h"

#include <algorithm>

namespace sage::input {

void Context::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    // Выключение ОТПУСКАЕТ действия, а не замораживает их. Иначе игрок,
    // открывший меню с зажатым «вперёд», вернётся в игру, которая всё это
    // время шла вперёд сама.
    if (!m_enabled) Reset();
}

Action& Context::Add(const std::string& name, ActionType type) {
    auto it = m_actions.find(name);
    if (it != m_actions.end()) return *it->second;
    auto action = std::make_unique<Action>(name, type);
    Action& ref = *action;
    m_actions.emplace(name, std::move(action));
    m_order.push_back(name);
    return ref;
}

Action* Context::Find(const std::string& name) {
    auto it = m_actions.find(name);
    return it == m_actions.end() ? nullptr : it->second.get();
}

const Action* Context::Find(const std::string& name) const {
    auto it = m_actions.find(name);
    return it == m_actions.end() ? nullptr : it->second.get();
}

bool Context::Remove(const std::string& name) {
    auto it = m_actions.find(name);
    if (it == m_actions.end()) return false;
    m_actions.erase(it);
    m_order.erase(std::remove(m_order.begin(), m_order.end(), name), m_order.end());
    return true;
}

Action* Context::FindByBinding(const Binding& binding) {
    for (const std::string& name : m_order) {
        Action* action = Find(name);
        if (action && action->UsesSource(binding)) return action;
    }
    return nullptr;
}

uint8_t Context::Evaluate(const Devices& devices, float dt, uint8_t blocked) {
    if (!m_enabled) return DeviceNone;

    uint8_t used = DeviceNone;
    for (const std::string& name : m_order) {
        Action* action = Find(name);
        if (!action) continue;
        action->Evaluate(devices, dt, blocked);
        used |= action->ActiveDevices();
    }
    // Забираем ТОЛЬКО то, чем воспользовались и что объявили забирать: контекст
    // интерфейса, до которого в этом кадре не дотронулись, не должен глушить
    // игру.
    return (uint8_t)(used & m_blocks);
}

void Context::Reset() {
    for (const std::string& name : m_order) {
        if (Action* action = Find(name)) action->Reset();
    }
}

} // namespace sage::input
