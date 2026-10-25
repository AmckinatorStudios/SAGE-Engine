#include "sage/scripting/ScriptModules.h"

#include <algorithm>
#include <exception>

#include "sage/core/Log.h"

namespace sage::scripting {

namespace {

std::vector<ScriptModule>& Registry() {
    static std::vector<ScriptModule> modules;
    return modules;
}

std::set<std::string>& Disabled() {
    static std::set<std::string> names;
    return names;
}

// Учёт ЭТОГО состояния Lua — в его собственном реестре, а не в C++: у одной
// игры может быть несколько состояний (редактор, Play, тесты), и «модуль уже
// стоит» — свойство состояния, а не процесса. Скрипту эта таблица не видна.
constexpr const char* kStateKey = "sage.modules.state";

sol::table StateTable(sol::state& lua) {
    sol::table reg = lua.registry();
    sol::object t = reg[kStateKey];
    if (t.get_type() == sol::type::table) return t.as<sol::table>();
    sol::table made = lua.create_table();
    reg[kStateKey] = made;
    return made;
}

enum class State { None, Installed, Failed, Disabled };

State StateOf(sol::table& st, const std::string& name) {
    sol::object rec = st[name];
    if (rec.get_type() != sol::type::table) return State::None;
    const std::string s = rec.as<sol::table>().get_or<std::string>("state", "");
    if (s == "installed") return State::Installed;
    if (s == "failed") return State::Failed;
    if (s == "disabled") return State::Disabled;
    return State::None;
}

void Record(sol::state& lua, sol::table& st, const ScriptModule& m, const char* state, const std::string& error) {
    sol::table rec = lua.create_table();
    rec["name"] = m.Name;
    rec["summary"] = m.Summary;
    rec["state"] = state;
    if (!error.empty()) rec["error"] = error;
    st[m.Name] = rec;
    // Порядок установки — для List(): в нём видно, кто кого ждал.
    sol::table order = st["__order"].get_or_create<sol::table>();
    bool known = false;
    for (size_t i = 1; i <= order.size(); ++i)
        if (order.get<std::string>(i) == m.Name) known = true;
    if (!known) order.add(m.Name);
}

// sage.modules — что стоит в этом состоянии и почему не стоит остальное.
void InstallIntrospection(sol::state& lua) {
    sol::table sage = lua["sage"].get_or_create<sol::table>();
    sol::table mods = sage["modules"].get_or_create<sol::table>();
    if (mods["List"].valid()) return;
    mods.set_function("Has", [](const std::string& name, sol::this_state ts) {
        sol::state_view L(ts);
        sol::table st = L.registry()[kStateKey].get_or_create<sol::table>();
        return StateOf(st, name) == State::Installed;
    });
    mods.set_function("List", [](sol::this_state ts) {
        sol::state_view L(ts);
        sol::table st = L.registry()[kStateKey].get_or_create<sol::table>();
        sol::table out = L.create_table();
        sol::table order = st["__order"].get_or_create<sol::table>();
        for (size_t i = 1; i <= order.size(); ++i) {
            sol::table rec = st[order.get<std::string>(i)];
            sol::table copy = L.create_table();
            for (const auto& kv : rec) copy[kv.first] = kv.second;
            out.add(copy);
        }
        return out;
    });
}

} // namespace

sol::table ScriptModuleContext::Module(const std::string& name) {
    sol::table root = Lua["sage"].get_or_create<sol::table>();
    return root[name].get_or_create<sol::table>();
}

bool RegisterScriptModule(ScriptModule module, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (module.Name.empty()) return fail("у модуля скриптов нет имени");
    if (!module.Install) return fail("модуль скриптов '" + module.Name + "' без функции установки");
    for (const ScriptModule& m : Registry())
        if (m.Name == module.Name) return fail("модуль скриптов '" + module.Name + "' уже зарегистрирован");
    Registry().push_back(std::move(module));
    return true;
}

bool UnregisterScriptModule(const std::string& name) {
    auto& r = Registry();
    const auto it = std::find_if(r.begin(), r.end(), [&](const ScriptModule& m) { return m.Name == name; });
    if (it == r.end()) return false;
    r.erase(it);
    return true;
}

const std::vector<ScriptModule>& RegisteredScriptModules() { return Registry(); }

void SetDisabledScriptModules(std::set<std::string> names) { Disabled() = std::move(names); }
const std::set<std::string>& DisabledScriptModules() { return Disabled(); }

bool IsScriptModuleInstalled(sol::state& lua, const std::string& name) {
    sol::table st = StateTable(lua);
    return StateOf(st, name) == State::Installed;
}

ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& modules) {
    ModuleInstallReport report;
    sol::state& lua = ctx.Lua;
    sol::table st = StateTable(lua);
    InstallIntrospection(lua);

    // Кандидаты — то, чего в состоянии ещё нет. Один и тот же модуль в двух
    // списках (встроенный и реестр) ставится один раз — первым по порядку.
    std::vector<const ScriptModule*> pending;
    for (const ScriptModule& m : modules) {
        if (StateOf(st, m.Name) != State::None) continue;
        if (std::any_of(pending.begin(), pending.end(), [&](const ScriptModule* p) { return p->Name == m.Name; }))
            continue;
        pending.push_back(&m);
    }
    auto inBatch = [&](const std::string& name) {
        return std::any_of(pending.begin(), pending.end(), [&](const ScriptModule* p) { return p->Name == name; });
    };
    auto drop = [&](const ScriptModule* m) { pending.erase(std::find(pending.begin(), pending.end(), m)); };

    // Проходы по списку: за проход ставится всё, что уже можно. Так при
    // зависимостях «назад по списку» порядок установки совпадает с порядком
    // записи — встроенный набор ведёт себя как прежняя цепочка вызовов.
    bool progress = true;
    while (progress && !pending.empty()) {
        progress = false;
        for (size_t i = 0; i < pending.size();) {
            const ScriptModule* m = pending[i];
            if (m->Optional && Disabled().count(m->Name)) {
                Record(lua, st, *m, "disabled", "выключен");
                report.Disabled.push_back(m->Name);
                drop(m);
                progress = true;
                continue;
            }
            bool ready = true;
            std::string blockedBy;
            State blockState = State::None;
            for (const std::string& dep : m->Requires) {
                const State s = StateOf(st, dep);
                if (s == State::Installed) continue;
                ready = false;
                if (s == State::Failed || s == State::Disabled) {
                    blockedBy = dep;
                    blockState = s;
                    break;
                }
            }
            if (!blockedBy.empty()) {
                // Без зависимости модуль не встанет никогда — не ждём её зря.
                if (blockState == State::Disabled) {
                    Record(lua, st, *m, "disabled", "выключен модуль '" + blockedBy + "'");
                    report.Disabled.push_back(m->Name);
                } else {
                    const std::string why = "не установлен: упал модуль '" + blockedBy + "', от которого он зависит";
                    Record(lua, st, *m, "failed", why);
                    report.Failed.push_back(m->Name);
                    report.Problems.push_back(m->Name + ": " + why);
                }
                drop(m);
                progress = true;
                continue;
            }
            if (!ready) {
                ++i;
                continue;
            }
            try {
                m->Install(ctx);
                Record(lua, st, *m, "installed", "");
                report.Installed.push_back(m->Name);
            } catch (const std::exception& e) {
                Record(lua, st, *m, "failed", e.what());
                report.Failed.push_back(m->Name);
                report.Problems.push_back(m->Name + ": " + e.what());
            }
            drop(m);
            progress = true;
        }
    }

    // Остались те, чья зависимость не в этом списке (дождутся следующей
    // установки), и кольца — модули, ждущие друг друга внутри списка.
    for (const ScriptModule* m : pending) {
        const bool cyclic = std::any_of(m->Requires.begin(), m->Requires.end(),
                                        [&](const std::string& d) { return inBatch(d); });
        if (cyclic) {
            const std::string why = "кольцо зависимостей: модули ждут друг друга";
            Record(lua, st, *m, "failed", why);
            report.Failed.push_back(m->Name);
            report.Problems.push_back(m->Name + ": " + why);
        } else {
            report.Waiting.push_back(m->Name);
        }
    }
    for (const std::string& p : report.Problems) LOG_ERROR("Scripting") << "Модуль API скриптов " << p;
    return report;
}

ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& builtins,
                                         bool withRegistry) {
    if (!withRegistry) return InstallScriptModules(ctx, builtins);
    std::vector<ScriptModule> all = builtins;
    all.insert(all.end(), Registry().begin(), Registry().end());
    return InstallScriptModules(ctx, all);
}

} // namespace sage::scripting
