#pragma once
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

// Таблица Lua <-> JSON. Одно правило на весь движок: сохранения игры
// (sage.save) и параметры перехода между сценами (scene:Load(name, {...}))
// переводят таблицы одинаково — числа, строки, булевы, вложенные таблицы и
// массивы. Функции, userdata и объекты сцены не переводятся: указатель на
// объект прошлой сцены или прошлого запуска в JSON не имеет смысла.
namespace sage::scripting {

nlohmann::json LuaTableToJson(const sol::table& table);
sol::object JsonToLua(const nlohmann::json& j, sol::state_view lua);

} // namespace sage::scripting
