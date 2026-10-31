#include "sage/ai/Blackboard.h"

namespace sage::ai {

const vars::Value* Blackboard::Find(const std::string& name) const {
    auto it = m_values.find(name);
    return it == m_values.end() ? nullptr : &it->second;
}

vars::Value Blackboard::Get(const std::string& name, const vars::Value& fallback) const {
    const vars::Value* v = Find(name);
    return v ? *v : fallback;
}

bool Blackboard::Set(const std::string& name, const vars::Value& value, std::string* error) {
    if (Strict()) {
        auto it = m_declared.find(name);
        if (it == m_declared.end()) {
            if (error) *error = "неизвестное имя '" + name + "'";
            return false;
        }
        // Вид держится объявлением: граф подключает к выходу провод
        // определённого типа, и подменить ему число строкой — сломать провод.
        m_values[name] = value.Type() == it->second ? value : vars::Value::Convert(value, it->second);
        return true;
    }
    m_values[name] = value;
    return true;
}

bool Blackboard::Erase(const std::string& name) {
    // Объявленное имя не удаляется, а сбрасывается: вход или выход у задачи
    // есть всегда, даже когда его значение никто не задавал.
    auto d = m_declared.find(name);
    if (d != m_declared.end()) {
        m_values[name] = vars::Value::Default(d->second);
        return true;
    }
    return m_values.erase(name) != 0;
}

void Blackboard::Clear() {
    m_values.clear();
    m_declared.clear();
}

void Blackboard::Declare(const std::string& name, const vars::Value& defaultValue) {
    m_declared[name] = defaultValue.Type();
    m_values[name] = defaultValue;
}

std::vector<std::string> Blackboard::Names() const {
    std::vector<std::string> out;
    out.reserve(m_values.size());
    for (const auto& [name, _] : m_values) out.push_back(name);
    return out;
}

} // namespace sage::ai
