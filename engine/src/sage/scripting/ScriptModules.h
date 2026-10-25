#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <sol/sol.hpp>

class ScriptEngine;

namespace sage::scripting::lua {
class Backend;
}

// ---------------------------------------------------------------------------
// МОДУЛИ API СКРИПТОВ — записи реестра, а не строки в конструкторе движка.
//
// Раньше API скриптов собирался одним списком вызовов в ScriptEngine.cpp
// (RegisterUIApi(); RegisterTweenApi(); ... — 27 строк), и у этого списка было
// три беды:
//   • добавить область API можно было только правкой движка: игре, плагину или
//     редактору было некуда положить свой sage.<что-то>;
//   • порядок держался на памяти того, кто писал список: «типы раньше
//     потребителей» нигде не записано, и перестановка двух строк ломала API
//     без единой ошибки компиляции;
//   • исключение в ОДНОЙ области обрывало конструктор целиком — без скриптов
//     оставалась вся игра, хотя сломана была, скажем, только сеть.
//
// Теперь каждая область — модуль с именем, списком того, без чего он не
// работает, и функцией установки. Установщик:
//   • ставит модули в порядке зависимостей (при равных — в порядке записи, так
//     что поведение встроенного набора не меняется);
//   • ловит сбой модуля, пишет его в журнал и пропускает только тех, кто от
//     него зависит, — остальное API работает;
//   • помнит, что уже стоит в ЭТОМ состоянии Lua: модуль ставится один раз,
//     сколько бы хозяев (прежний движок, бэкенд нового API) ни звали установку,
//     а модуль, чья зависимость появится позже, доустановится тогда.
//
// Свой модуль игры или плагина — одна запись в реестре до создания движка:
//
//      sage::scripting::RegisterScriptModule({
//          .Name = "weather", .Summary = "погода игры",
//          .Requires = {"lighting"},
//          .Install = [](sage::scripting::ScriptModuleContext& c) {
//              c.Module("weather").set_function("Rain", [](float k) { ... });
//          }});
//
// Из скрипта видно, что установлено: sage.modules.List(), sage.modules.Has(имя).
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Что модуль получает при установке.
struct ScriptModuleContext {
    sol::state& Lua;
    // Прежний движок (sage.*, глобальные Vec3/Spawn) — nullptr, если состояние
    // без него (чистый бэкенд нового API, тесты).
    ScriptEngine* Engine = nullptr;
    // Бэкенд нового API (Scene/Test/сигналы) — nullptr, если его ещё нет.
    lua::Backend* Backend = nullptr;

    // Таблица sage.<name>, создаётся по требованию и не затирается вторым
    // модулем, пишущим в ту же область.
    sol::table Module(const std::string& name);
};

struct ScriptModule {
    std::string Name;                    // уникальное имя: "procedural", "net"
    std::string Summary;                 // одна строка для sage.modules.List()
    std::vector<std::string> Requires;   // имена модулей, без которых не работает
    // false — модуль нельзя выключить (на нём стоят типы для остальных).
    bool Optional = true;
    std::function<void(ScriptModuleContext&)> Install;
};

// --- Реестр модулей процесса (игра, плагин, редактор) ------------------------
//
// Встроенные модули движка в реестр не пишутся — их список у каждого хозяина
// свой (ScriptEngine, бэкенд Lua), и ставятся они первыми. Реестр — то, что
// добавили СНАРУЖИ.

// false + err, если имя пустое, занято или нет функции установки.
bool RegisterScriptModule(ScriptModule module, std::string* err = nullptr);
bool UnregisterScriptModule(const std::string& name);
const std::vector<ScriptModule>& RegisteredScriptModules();

// Выключенные модули: в новых состояниях Lua не ставятся (и с ними — все, кто
// от них зависит). Обязательные (Optional = false) выключить нельзя.
void SetDisabledScriptModules(std::set<std::string> names);
const std::set<std::string>& DisabledScriptModules();

// --- Установка ---------------------------------------------------------------

struct ModuleInstallReport {
    std::vector<std::string> Installed;   // поставлены этим вызовом
    std::vector<std::string> Waiting;     // ждут зависимость, которой пока нет
    std::vector<std::string> Disabled;    // выключены (сами или их зависимость)
    std::vector<std::string> Failed;      // упали при установке или в цикле
    std::vector<std::string> Problems;    // по строке на каждый сбой: что и почему
};

// Ставит модули в ctx.Lua в порядке зависимостей. Уже стоящие пропускаются.
ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& modules);
// Встроенный список хозяина, затем реестр процесса.
ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& builtins,
                                         bool withRegistry);

// Стоит ли модуль в этом состоянии Lua.
bool IsScriptModuleInstalled(sol::state& lua, const std::string& name);

} // namespace sage::scripting
