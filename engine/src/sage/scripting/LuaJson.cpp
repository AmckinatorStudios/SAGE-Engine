#include "sage/scripting/LuaJson.h"

#include <vector>

namespace sage::scripting {

namespace {

// --- sol::table <-> JSON ------------------------------------------------------
//
// Игра думает таблицами Lua, файл хранит JSON. Перевод здесь, в одном месте, и
// он намеренно ограничен: числа, строки, булевы, вложенные таблицы и массивы.
// Функции, userdata и сущности НЕ сохраняются — сохранить указатель на объект
// прошлого запуска нельзя в принципе, а сделать вид, что можно, значит вернуть
// игроку мусор вместо прогресса.
nlohmann::json TableToJsonImpl(const sol::table& table, int depth,
                               std::vector<const void*>& openTables) {
    // ЦИКЛЫ ловим по факту, а не глубиной рекурсии.
    //
    // Одной только глубины мало, и это выяснилось падением: у t.self = t каждый
    // уровень держит ОТКРЫТЫЙ обход таблицы (lua_next требует ключ на стеке всё
    // время обхода), и три десятка вложенных обходов переполняют стек Lua. Игра
    // при этом не падает сразу — она падает потом, при закрытии состояния, и
    // связать это с сохранением уже невозможно.
    //
    // Поэтому помним таблицы, обход которых сейчас открыт: повторная встреча —
    // это цикл, и в JSON он не представим ни при какой глубине.
    const void* id = table.pointer();
    for (const void* open : openTables) {
        if (open == id) return nlohmann::json::object();
    }
    if (depth > 16) return nlohmann::json::object();
    openTables.push_back(id);
    struct PopGuard {
        std::vector<const void*>& V;
        ~PopGuard() { V.pop_back(); }
    } guard{openTables};

    // Массив или словарь решается по ключам: таблица Lua — и то и другое сразу,
    // а в JSON это разные вещи. Считаем массивом только сплошную нумерацию с 1.
    bool isArray = table.size() > 0;
    if (isArray) {
        size_t counted = 0;
        for (const auto& kv : table) {
            if (kv.first.get_type() != sol::type::number) { isArray = false; break; }
            ++counted;
        }
        if (counted != table.size()) isArray = false;
    }

    nlohmann::json out = isArray ? nlohmann::json::array() : nlohmann::json::object();
    auto convert = [&](const sol::object& v) -> nlohmann::json {
        switch (v.get_type()) {
            case sol::type::number: {
                const double d = v.as<double>();
                // Целое пишем целым: иначе счётчик предметов возвращается как
                // 7.0 и «== 7» в скрипте продолжает работать, а вот вывод в
                // интерфейсе внезапно показывает 7.0.
                if (d == (double)(long long)d) return (long long)d;
                return d;
            }
            case sol::type::boolean: return v.as<bool>();
            case sol::type::string: return v.as<std::string>();
            case sol::type::table: return TableToJsonImpl(v.as<sol::table>(), depth + 1, openTables);
            default: return nullptr;
        }
    };

    for (const auto& kv : table) {
        const nlohmann::json value = convert(kv.second);
        if (value.is_null()) continue;   // несохраняемое молча пропускаем
        if (isArray) out.push_back(value);
        else if (kv.first.get_type() == sol::type::string) out[kv.first.as<std::string>()] = value;
        else if (kv.first.get_type() == sol::type::number)
            out[std::to_string(kv.first.as<long long>())] = value;
    }
    return out;
}

} // namespace

nlohmann::json LuaTableToJson(const sol::table& table) {
    std::vector<const void*> open;
    return TableToJsonImpl(table, 0, open);
}

sol::object JsonToLua(const nlohmann::json& j, sol::state_view lua) {
    if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
    if (j.is_number_integer()) return sol::make_object(lua, j.get<long long>());
    if (j.is_number()) return sol::make_object(lua, j.get<double>());
    if (j.is_string()) return sol::make_object(lua, j.get<std::string>());
    if (j.is_array()) {
        sol::table t = lua.create_table((int)j.size(), 0);
        int i = 1;
        for (const auto& v : j) t[i++] = JsonToLua(v, lua);
        return t;
    }
    if (j.is_object()) {
        sol::table t = lua.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = JsonToLua(it.value(), lua);
        return t;
    }
    return sol::nil;
}


} // namespace sage::scripting
