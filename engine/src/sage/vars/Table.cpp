#include "sage/vars/Table.h"

#include <algorithm>

namespace sage::vars {

const Var* Table::Find(const std::string& name) const {
    for (const Var& v : m_vars)
        if (v.Name == name) return &v;
    return nullptr;
}

Var* Table::Find(const std::string& name) {
    for (Var& v : m_vars)
        if (v.Name == name) return &v;
    return nullptr;
}

Value Table::Get(const std::string& name) const {
    const Var* v = Find(name);
    return v ? v->Data : Value();
}

void Table::Set(const std::string& name, const Value& value) {
    if (Var* v = Find(name)) {
        // Вид держится за переменной, а не за последним присваиванием.
        v->Data = (v->Data.Type() == value.Type()) ? value : Value::Convert(value, v->Data.Type());
        return;
    }
    Var v;
    v.Name = name;
    v.Data = value;
    m_vars.push_back(std::move(v));
}

Var& Table::Put(const Var& var) {
    if (Var* existing = Find(var.Name)) {
        *existing = var;
        return *existing;
    }
    m_vars.push_back(var);
    return m_vars.back();
}

bool Table::Remove(const std::string& name) {
    auto it = std::find_if(m_vars.begin(), m_vars.end(),
                           [&](const Var& v) { return v.Name == name; });
    if (it == m_vars.end()) return false;
    m_vars.erase(it);
    return true;
}

bool Table::Move(size_t from, size_t to) {
    if (from >= m_vars.size() || to >= m_vars.size() || from == to) return false;
    Var moved = m_vars[from];
    m_vars.erase(m_vars.begin() + (long)from);
    m_vars.insert(m_vars.begin() + (long)to, std::move(moved));
    return true;
}

void Table::MergeDeclaration(const Table& declaration) {
    std::vector<Var> merged;
    merged.reserve(std::max(declaration.Size(), m_vars.size()));

    for (const Var& decl : declaration.All()) {
        Var out = decl;
        out.Declared = true;
        if (const Var* mine = Find(decl.Name)) {
            // Значение объекта важнее умолчания скрипта — но только если оно
            // того же вида. Скрипт, сменивший тип переменной, поменял её смысл,
            // и старое число в новом поле было бы мусором с видом настройки.
            out.Data = (mine->Data.Type() == decl.Data.Type())
                           ? mine->Data
                           : Value::Convert(mine->Data, decl.Data.Type());
        }
        merged.push_back(std::move(out));
    }
    // Хвост: то, чего в объявлении нет. Не выбрасываем — см. заголовок.
    for (const Var& mine : m_vars) {
        if (declaration.Find(mine.Name)) continue;
        Var out = mine;
        out.Declared = false;
        merged.push_back(std::move(out));
    }
    m_vars = std::move(merged);
}

} // namespace sage::vars
