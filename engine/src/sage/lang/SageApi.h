#pragma once
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// СИГНАТУРЫ API СКРИПТОВ — для проверки типов, без движка и без Lua.
//
// Проверка скриптов (SageCheck) живёт в слое lang и не видит слой scripting,
// где функции регистрируются (Bind). Поэтому сигнатуры приходят к ней
// таблицей, собранной scripts/gen_script_api.py из тех же Bind(...), что и
// подсказка редактора: функция поменяла аргументы — после перегенерации
// проверка знает об этом, а `--check` в прогоне не даст забыть.
//
// Тип в таблице — как в подсказке: number, string, boolean, table, Vec3, any…
// Необязательный аргумент помечен «?», произвольное число — «...».
// ---------------------------------------------------------------------------
namespace sage::lang {

struct ApiFunction {
    const char* Name;     // "sage.noise.Perlin", "noise.Perlin", "perlin"
    const char* Params;   // "x:number,y:number,z?:number"
    const char* Ret;      // "number"; "nil"/"any" — ничего определённого
};

// Все функции таблицы — для тех, кто строит по API свои описания (узлы
// SAGE AI Graph по каждой функции движка).
std::vector<const ApiFunction*> AllApiFunctions();
// Функция API по полному имени; nullptr — такой нет.
const ApiFunction* FindApiFunction(std::string_view dotted);
// Есть ли функции с таким префиксом: "sage.noise", "noise", "sage".
bool IsApiModule(std::string_view dotted);
// Имя верхнего уровня, которое скрипт видит без объявления: API движка,
// псевдонимы модулей, стандартная библиотека Lua.
bool IsKnownGlobal(std::string_view name);
// Все такие имена — кандидаты для «возможно, имелось в виду».
const std::vector<std::string>& KnownGlobals();
// Функции модуля (без префикса): для подсказки по опечатке в noise.Perln.
std::vector<std::string> ApiModuleMembers(std::string_view module);

// Ближайшее по написанию имя из кандидатов или пусто, если похожих нет.
// Порог — треть длины: perlni → perlin, но не x → max.
std::string ClosestName(std::string_view name, const std::vector<std::string>& candidates);

} // namespace sage::lang
