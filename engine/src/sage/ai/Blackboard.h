#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// ПЕРЕМЕННЫЕ ИИ: одна таблица «имя -> значение» на любую область.
//
// Local (данные одного выполнения задачи), Instance (память персонажа) и
// Shared (общее нескольким персонажам) — это ОДИН тип с разным временем
// жизни, а не три. Три типа означали бы три набора правил записи и три
// перевода в Lua и в граф — и они разошлись бы на первой же правке.
//
// Значение — общий sage::vars::Value, тот же, что у публичных переменных,
// событий и инспектора: задача на Lua и задача на графе передают друг другу
// ОДНО и то же, и «в графе число, а в Lua строка» не случается по построению.
//
// ОБЪЯВЛЕННЫЕ ИМЕНА. Входы и выходы задачи объявляются заранее (с умолчанием,
// по нему же — тип). Таблица с объявлениями СТРОГАЯ: запись в необъявленное
// имя — ошибка с этим именем, а значение чужого вида приводится к
// объявленному. Иначе опечатка «targt» молча заводила бы новый выход, а задача,
// которая его ждёт, получала бы умолчание — и ни одной зацепки, почему.
// Таблица без объявлений (память персонажа, общие данные) принимает любое имя.
// ---------------------------------------------------------------------------
namespace sage::ai {

class Blackboard {
public:
    bool Has(const std::string& name) const { return m_values.count(name) != 0; }
    const vars::Value* Find(const std::string& name) const;
    vars::Value Get(const std::string& name, const vars::Value& fallback = {}) const;

    // false и причина в error — имя не объявлено в строгой таблице.
    bool Set(const std::string& name, const vars::Value& value, std::string* error = nullptr);
    bool Erase(const std::string& name);
    void Clear();

    // Объявить имя с умолчанием: таблица становится строгой.
    void Declare(const std::string& name, const vars::Value& defaultValue);
    bool Declared(const std::string& name) const { return m_declared.count(name) != 0; }
    bool Strict() const { return !m_declared.empty(); }

    std::vector<std::string> Names() const;
    size_t Size() const { return m_values.size(); }
    const std::map<std::string, vars::Value>& All() const { return m_values; }

private:
    std::map<std::string, vars::Value> m_values;
    // Имя -> вид, к которому приводится запись.
    std::map<std::string, vars::Kind> m_declared;
};

// Через указатель: таблицу держат и выполнение, и ручка в скрипте, и граф —
// выполнение может кончиться раньше, чем у них пропадёт интерес к выходам.
using BlackboardPtr = std::shared_ptr<Blackboard>;

} // namespace sage::ai
