#include "sage/scripting/ScriptModules.h"

#include <algorithm>
#include <exception>
#include <map>

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
// Описания стоящих модулей (их уборка Uninstall — функция C++) — userdata в
// том же реестре: живёт и умирает вместе с состоянием.
constexpr const char* kBookKey = "sage.modules.book";

struct Book {
    std::map<std::string, ScriptModule> Installed;
};

Book& BookOf(sol::state& lua) {
    sol::table reg = lua.registry();
    sol::object o = reg[kBookKey];
    if (!o.is<Book>()) {
        reg[kBookKey] = Book{};
        o = reg[kBookKey];
    }
    return o.as<Book&>();
}

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

sol::table Record(sol::state& lua, sol::table& st, const ScriptModule& m, const char* state, const std::string& error) {
    sol::table rec = lua.create_table();
    rec["name"] = m.Name;
    rec["summary"] = m.Summary;
    rec["state"] = state;
    if (!error.empty()) rec["error"] = error;
    sol::table req = lua.create_table();
    for (const std::string& r : m.Requires) req.add(r);
    rec["requires"] = req;
    st[m.Name] = rec;
    // Порядок установки — для List() и для снятия в обратном порядке.
    sol::table order = st["__order"].get_or_create<sol::table>();
    bool known = false;
    for (size_t i = 1; i <= order.size(); ++i)
        if (order.get<std::string>(i) == m.Name) known = true;
    if (!known) order.add(m.Name);
    return rec;
}

// --- Что модуль положил в Lua ------------------------------------------------
//
// Снимок имён до и после установки: глобальные имена, поля глобальных таблиц
// (Scene.*, math.*…) и sage.<модуль>.<функция>. Разница — то, что
// принадлежит модулю: снятие убирает ровно это. Стандартные библиотеки Lua не
// обходятся — модули в них не пишут.
//
// Снимок и разница — на самом Lua (обход next без ссылок C++ на каждое
// значение): обход через sol2 стоил в десять раз дороже самой установки API.
// Снимок «после» модуля служит снимком «до» следующего — по одному обходу на
// модуль. Всё это — ЗАПУСК: в кадре этого кода нет.
constexpr const char* kTrackerKey = "sage.modules.tracker";
constexpr const char* kTracker = R"LUA(
local SKIP = { _G = true, package = true, string = true, table = true, coroutine = true,
               utf8 = true, debug = true, io = true, os = true }
-- next, type и глобальная таблица — аргументами: учёт обязан работать и в
-- состоянии без базовой библиотеки Lua (там нет ни next, ни _G).
local next, type, G = ...
local function keys(t)
    local out = {}
    for k, v in next, t do if type(k) == "string" then out[k] = v end end
    return out
end
local function snap()
    local s = { [""] = keys(G) }
    for k, v in next, G do
        if type(k) == "string" and type(v) == "table" and not SKIP[k] then
            s[k] = keys(v)
            if k == "sage" then
                for m, t in next, v do
                    if type(m) == "string" and type(t) == "table" then s["sage." .. m] = keys(t) end
                end
            end
        end
    end
    return s
end
local function diff(before, after)
    local added = {}
    for path, names in next, after do
        local was = before[path]
        for k, v in next, names do
            if was == nil or was[k] == nil then added[#added + 1] = { path, k, v } end
        end
    end
    return added
end
return { snap = snap, diff = diff }
)LUA";

int RawNext(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    if (lua_next(L, 1)) return 2;
    lua_pushnil(L);
    return 1;
}

int RawType(lua_State* L) {
    lua_pushstring(L, luaL_typename(L, 1));
    return 1;
}

struct Tracker {
    sol::protected_function Snap, Diff;
};

Tracker TrackerOf(sol::state& lua) {
    sol::table reg = lua.registry();
    sol::object t = reg[kTrackerKey];
    if (t.get_type() != sol::type::table) {
        sol::protected_function chunk = lua.load(kTracker, "sage.modules.tracker");
        t = chunk(&RawNext, &RawType, lua.globals()).get<sol::object>();
        reg[kTrackerKey] = t;
    }
    sol::table tt = t.as<sol::table>();
    return {tt["snap"], tt["diff"]};
}

sol::object TableAt(sol::state& lua, const std::string& path) {
    if (path.empty()) return lua.globals();
    const size_t dot = path.find('.');
    sol::object t = lua.globals()[path.substr(0, dot)];
    if (dot == std::string::npos || t.get_type() != sol::type::table) return t;
    return t.as<sol::table>()[path.substr(dot + 1)];
}

// Убрать имена, записанные за модулем. Только те, что всё ещё его: имя,
// которое после установки переписал кто-то другой, принадлежит уже ему.
// Глубокие сначала: поле sage.x.f снимается раньше таблицы sage.x.
void RemoveAdded(sol::state& lua, sol::table added) {
    std::vector<sol::table> items;
    for (size_t i = 1; i <= added.size(); ++i) items.push_back(added[i]);
    std::stable_sort(items.begin(), items.end(), [](const sol::table& a, const sol::table& b) {
        return a.get<std::string>(1).size() > b.get<std::string>(1).size();
    });
    for (sol::table& item : items) {
        sol::object table = TableAt(lua, item.get<std::string>(1));
        if (table.get_type() != sol::type::table) continue;
        sol::table t = table.as<sol::table>();
        const std::string key = item.get<std::string>(2);
        sol::object now = t.raw_get<sol::object>(key);
        sol::object was = item[3];
        if (now != was) continue;
        t.raw_set(key, sol::lua_nil);
        // У области sage.<имя> есть короткое имя в глобальных (game.Quit) —
        // та же таблица; снятая область уходит и оттуда.
        if (item.get<std::string>(1) == "sage" && was.get_type() == sol::type::table) {
            sol::table g = lua.globals();
            if (g.raw_get<sol::object>(key) == was) g.raw_set(key, sol::lua_nil);
        }
    }
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
            for (const char* k : {"name", "summary", "state", "error"}) copy[k] = rec[k];
            out.add(copy);
        }
        return out;
    });
}

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

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
    if (Has(module.Requires, module.Name)) return fail("модуль скриптов '" + module.Name + "' требует сам себя");
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

// Порядок Кана с устойчивостью: из готовых берётся первый по записи — так
// при зависимостях «назад по списку» порядок совпадает с записью.
ModuleCheckReport CheckScriptModules(const std::vector<ScriptModule>& modules) {
    ModuleCheckReport r;
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < modules.size(); ++i) {
        if (!index.emplace(modules[i].Name, i).second)
            r.Problems.push_back("модуль '" + modules[i].Name + "' записан дважды");
    }
    std::vector<std::vector<size_t>> deps(modules.size());
    for (size_t i = 0; i < modules.size(); ++i) {
        for (const std::string& d : modules[i].Requires) {
            const auto it = index.find(d);
            if (it == index.end()) r.Problems.push_back("модуль '" + modules[i].Name + "' требует '" + d + "', которого нет");
            else deps[i].push_back(it->second);
        }
        for (const std::string& d : modules[i].After)
            if (const auto it = index.find(d); it != index.end()) deps[i].push_back(it->second);
    }
    std::vector<bool> done(modules.size(), false);
    for (size_t placed = 0; placed < modules.size();) {
        bool progress = false;
        for (size_t i = 0; i < modules.size(); ++i) {
            if (done[i]) continue;
            if (std::all_of(deps[i].begin(), deps[i].end(), [&](size_t d) { return done[d]; })) {
                done[i] = true;
                r.Order.push_back(modules[i].Name);
                ++placed;
                progress = true;
                break;   // снова с начала списка: устойчивый порядок
            }
        }
        if (!progress) {
            std::string ring;
            for (size_t i = 0; i < modules.size(); ++i)
                if (!done[i]) ring += (ring.empty() ? "" : ", ") + modules[i].Name;
            r.Problems.push_back("кольцо зависимостей: " + ring);
            break;
        }
    }
    return r;
}

ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& modules, bool final) {
    ModuleInstallReport report;
    sol::state& lua = ctx.Lua;
    sol::table st = StateTable(lua);
    InstallIntrospection(lua);
    Book& book = BookOf(lua);
    const Tracker tracker = TrackerOf(lua);
    sol::object before;   // снимок имён перед очередным модулем

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
    auto fail = [&](const ScriptModule* m, const std::string& why) {
        Record(lua, st, *m, "failed", why);
        report.Failed.push_back(m->Name);
        report.Problems.push_back(m->Name + ": " + why);
    };

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
            // Мягкий порядок: ждём только того, кто стоит в этой же очереди.
            for (const std::string& dep : m->After)
                if (inBatch(dep)) ready = false;
            if (!blockedBy.empty()) {
                // Без зависимости модуль не встанет никогда — не ждём её зря.
                if (blockState == State::Disabled) {
                    Record(lua, st, *m, "disabled", "выключен модуль '" + blockedBy + "'");
                    report.Disabled.push_back(m->Name);
                } else {
                    fail(m, "не установлен: упал модуль '" + blockedBy + "', от которого он зависит");
                }
                drop(m);
                progress = true;
                continue;
            }
            if (!ready) {
                ++i;
                continue;
            }
            if (!before.valid()) before = tracker.Snap();
            sol::object after;
            try {
                m->Install(ctx);
                after = tracker.Snap();
                sol::table rec = Record(lua, st, *m, "installed", "");
                rec["added"] = tracker.Diff(before, after);
                book.Installed[m->Name] = *m;
                report.Installed.push_back(m->Name);
            } catch (const std::exception& e) {
                // Откат: полуустановленного API не остаётся.
                after = tracker.Snap();
                RemoveAdded(lua, tracker.Diff(before, after));
                after = tracker.Snap();
                fail(m, e.what());
            }
            before = after;
            drop(m);
            progress = true;
        }
    }

    // Остались кольца (модули ждут друг друга внутри списка) и те, чья
    // обязательная зависимость не в этом списке: дождутся следующего хозяина
    // состояния — или, если хозяин последний, это ошибка «нет модуля».
    for (const ScriptModule* m : pending) {
        const bool cyclic = std::any_of(m->Requires.begin(), m->Requires.end(), [&](const std::string& d) { return inBatch(d); }) ||
                            std::any_of(m->After.begin(), m->After.end(), [&](const std::string& d) { return inBatch(d); });
        if (cyclic) {
            fail(m, "кольцо зависимостей: модули ждут друг друга");
            continue;
        }
        if (final) {
            std::string missing;
            for (const std::string& d : m->Requires)
                if (StateOf(st, d) != State::Installed) missing += (missing.empty() ? "" : ", ") + d;
            fail(m, "нет обязательного модуля: " + missing);
            continue;
        }
        report.Waiting.push_back(m->Name);
    }
    for (const std::string& p : report.Problems) LOG_ERROR("Scripting") << "Модуль API скриптов " << p;
    return report;
}

ModuleInstallReport InstallScriptModules(ScriptModuleContext& ctx, const std::vector<ScriptModule>& builtins,
                                         bool withRegistry, bool final) {
    if (!withRegistry) return InstallScriptModules(ctx, builtins, final);
    std::vector<ScriptModule> all = builtins;
    all.insert(all.end(), Registry().begin(), Registry().end());
    return InstallScriptModules(ctx, all, final);
}

std::vector<std::string> UninstallScriptModule(ScriptModuleContext& ctx, const std::string& name) {
    sol::state& lua = ctx.Lua;
    sol::table st = StateTable(lua);
    std::vector<std::string> removed;
    if (StateOf(st, name) != State::Installed) return removed;

    // Кого снимать: сам модуль и всех, кто от него зависит, — транзитивно.
    std::set<std::string> doomed = {name};
    sol::table order = st["__order"].get_or_create<sol::table>();
    std::vector<std::string> names;
    for (size_t i = 1; i <= order.size(); ++i) names.push_back(order.get<std::string>(i));
    for (bool grew = true; grew;) {
        grew = false;
        for (const std::string& n : names) {
            if (doomed.count(n) || StateOf(st, n) != State::Installed) continue;
            sol::table req = st[n]["requires"];
            for (size_t i = 1; i <= req.size(); ++i)
                if (doomed.count(req.get<std::string>(i))) {
                    doomed.insert(n);
                    grew = true;
                    break;
                }
        }
    }

    // Обратный порядок установки: зависящий снимается раньше того, на ком стоит.
    Book& book = BookOf(lua);
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!doomed.count(*it) || StateOf(st, *it) != State::Installed) continue;
        if (auto b = book.Installed.find(*it); b != book.Installed.end()) {
            if (b->second.Uninstall) {
                try {
                    b->second.Uninstall(ctx);
                } catch (const std::exception& e) {
                    LOG_ERROR("Scripting") << "Модуль API скриптов " << *it << ": уборка упала: " << e.what();
                }
            }
            book.Installed.erase(b);
        }
        sol::table rec = st[*it];
        if (rec["added"].get_type() == sol::type::table) RemoveAdded(lua, rec["added"]);
        st[*it] = sol::lua_nil;
        removed.push_back(*it);
    }
    // Из порядка — тоже: снятый модуль ставится заново в конец.
    sol::table kept = lua.create_table();
    for (const std::string& n : names)
        if (!doomed.count(n)) kept.add(n);
    st["__order"] = kept;
    return removed;
}

} // namespace sage::scripting
