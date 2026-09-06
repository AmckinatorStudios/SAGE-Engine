#include "sage/events/Rules.h"

#include <cmath>

namespace sage::events {

const char* CompareName(Compare op) {
    switch (op) {
        case Compare::IsTrue:         return "isTrue";
        case Compare::IsFalse:        return "isFalse";
        case Compare::Equal:          return "==";
        case Compare::NotEqual:       return "!=";
        case Compare::Less:           return "<";
        case Compare::LessOrEqual:    return "<=";
        case Compare::Greater:        return ">";
        case Compare::GreaterOrEqual: return ">=";
    }
    return "isTrue";
}

Compare ParseCompare(const std::string& name) {
    if (name == "isFalse" || name == "false") return Compare::IsFalse;
    if (name == "==" || name == "equal") return Compare::Equal;
    if (name == "!=" || name == "notEqual") return Compare::NotEqual;
    if (name == "<" || name == "less") return Compare::Less;
    if (name == "<=" || name == "lessOrEqual") return Compare::LessOrEqual;
    if (name == ">" || name == "greater") return Compare::Greater;
    if (name == ">=" || name == "greaterOrEqual") return Compare::GreaterOrEqual;
    return Compare::IsTrue;
}

bool TestCondition(const Condition& condition, const Resolver& resolver) {
    // Без резолвера мир недоступен — условие считается НЕ выполненным. Это
    // осознанный выбор в пользу молчания: правило, которое не смогло
    // проверить «дверь заперта?», не должно открывать дверь.
    if (!resolver) return false;
    const sage::vars::Value actual = resolver(condition.Var);

    switch (condition.Op) {
        case Compare::IsTrue:  return actual.AsBool();
        case Compare::IsFalse: return !actual.AsBool();
        case Compare::Equal:
        case Compare::NotEqual: {
            // Сравнение СТРОКАМИ, а не числами: переменная может быть строкой,
            // объектом или ассетом, и «состояние == открыта» должно работать
            // так же, как «здоровье == 0». Числа приводятся к одному виду
            // записи внутри Value, поэтому 1 и 1.0 не разъезжаются.
            const bool same = actual.AsString() == condition.Value.AsString();
            return condition.Op == Compare::Equal ? same : !same;
        }
        case Compare::Less:           return actual.AsFloat() < condition.Value.AsFloat();
        case Compare::LessOrEqual:    return actual.AsFloat() <= condition.Value.AsFloat();
        case Compare::Greater:        return actual.AsFloat() > condition.Value.AsFloat();
        case Compare::GreaterOrEqual: return actual.AsFloat() >= condition.Value.AsFloat();
    }
    return false;
}

bool TestAll(const std::vector<Condition>& conditions, const Resolver& resolver) {
    // Пустой список — «условий нет», правило срабатывает всегда: «КОГДА нажали
    // кнопку ТО открыть дверь» без единого «если» должно работать.
    for (const Condition& c : conditions) {
        if (!TestCondition(c, resolver)) return false;
    }
    return true;
}

RuleSet::~RuleSet() { Uninstall(); }

void RuleSet::Add(Rule rule) {
    m_rules.push_back(std::move(rule));
    // Набор уже на шине — подписываем заново, иначе добавленное правило
    // молчит до следующего Install, и разбираться с этим будут долго.
    if (m_bus) Install(*m_bus);
}

void RuleSet::Clear() {
    Uninstall();
    m_rules.clear();
    m_fired = 0;
}

void RuleSet::Install(Bus& bus) {
    Uninstall();
    m_bus = &bus;
    // Подписка на каждое правило отдельно, по имени его повода: так шина сама
    // отбирает нужные, и обработчику не приходится перебирать весь набор на
    // каждое событие в игре.
    for (size_t i = 0; i < m_rules.size(); ++i) {
        const std::string& when = m_rules[i].When;
        if (when.empty()) continue;
        const int id = bus.On(when, [this, i](const Event& cause) {
            if (i >= m_rules.size()) return;
            const Rule& rule = m_rules[i];
            if (!rule.Enabled) return;
            if (!TestAll(rule.If, m_resolver)) return;
            Fire(rule, cause);
        });
        m_subscriptions.push_back(id);
    }
}

void RuleSet::Uninstall() {
    if (m_bus) {
        for (int id : m_subscriptions) m_bus->Off(id);
    }
    m_subscriptions.clear();
    m_bus = nullptr;
}

void RuleSet::Fire(const Rule& rule, const Event& cause) {
    ++m_fired;
    if (!m_bus) return;
    for (const Binding& then : rule.Then) {
        if (!then.Enabled) continue;
        Event out;
        out.Name = then.Event;
        out.Arg = then.Arg;
        // Отправитель наследуется от повода: «дверь открылась» должна
        // сообщать, КАКАЯ дверь, а правило само себя объектом не считает.
        out.Sender = cause.Sender;
        out.Target = then.Target;
        out.Method = then.Method;
        m_bus->Emit(out);
    }
}

} // namespace sage::events
