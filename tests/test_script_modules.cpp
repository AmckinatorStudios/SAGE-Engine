#include "TestFramework.h"

#include <stdexcept>
#include <string>

#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptModules.h"
#include "sage/scripting/lua/LuaBackend.h"

// ---------------------------------------------------------------------------
// МОДУЛИ API СКРИПТОВ (sage/scripting/ScriptModules.h): порядок по
// зависимостям, сбой одного модуля не роняет остальные, свой модуль игры
// ставится без правки движка, выключенный модуль не ставится.
// ---------------------------------------------------------------------------

using namespace sage::scripting;

namespace {

// Реестр процесса общий для всех тестов: каждый тест убирает за собой.
struct RegistryGuard {
    std::vector<std::string> Names;
    ~RegistryGuard() {
        for (const std::string& n : Names) UnregisterScriptModule(n);
        SetDisabledScriptModules({});
    }
    void Add(ScriptModule m) {
        Names.push_back(m.Name);
        std::string err;
        if (!RegisterScriptModule(std::move(m), &err)) throw std::runtime_error(err);
    }
};

ScriptModule Simple(const std::string& name, std::vector<std::string> deps, const std::string& value) {
    return ScriptModule{name, "тест", std::move(deps), true, [name, value](ScriptModuleContext& c) {
                            c.Module(name)["value"] = value;
                        }};
}

} // namespace

TEST(ScriptModules_builtins_are_listed_and_installed) {
    ScriptEngine se;
    for (const char* name : {"math", "object", "scene", "procedural", "physics", "net"})
        CHECK_TRUE(IsScriptModuleInstalled(se.Lua(), name));
    const bool has = se.Lua().script("return sage.modules.Has('procedural') and modules.Has('tween')");
    CHECK_TRUE(has);
    const int count = se.Lua().script("return #sage.modules.List()");
    CHECK_TRUE(count >= 26);
    // Порядок установки — порядок записи: типы раньше потребителей.
    const std::string first = se.Lua().script("return sage.modules.List()[1].name");
    CHECK_EQ(first, std::string("math"));
}

TEST(ScriptModules_game_module_plugs_in_without_touching_the_engine) {
    RegistryGuard g;
    g.Add({"weather", "погода", {"lighting"}, true, [](ScriptModuleContext& c) {
               c.Module("weather").set_function("Rain", [](float k) { return k * 2.0f; });
           }});
    ScriptEngine se;
    const float r = se.Lua().script("return sage.weather.Rain(1.5)");
    CHECK_NEAR(r, 3.0f, 1e-6);
    // Короткое имя — как у встроенных.
    const float shortName = se.Lua().script("return weather.Rain(2)");
    CHECK_NEAR(shortName, 4.0f, 1e-6);
}

TEST(ScriptModules_order_follows_dependencies_not_the_list) {
    // Записан раньше своей зависимости — всё равно встаёт после неё.
    std::vector<ScriptModule> mods = {Simple("late_b", {"late_a"}, "b"), Simple("late_a", {}, "a")};
    sol::state lua;
    ScriptModuleContext ctx{lua};
    const ModuleInstallReport r = InstallScriptModules(ctx, mods);
    CHECK_EQ(r.Installed.size(), (size_t)2);
    CHECK_EQ(r.Installed[0], std::string("late_a"));
    CHECK_EQ(r.Installed[1], std::string("late_b"));
}

TEST(ScriptModules_a_failing_module_takes_only_its_dependants) {
    std::vector<ScriptModule> mods = {
        Simple("ok_one", {}, "1"),
        {"broken", "", {}, true, [](ScriptModuleContext&) { throw std::runtime_error("нет видеокарты"); }},
        Simple("needs_broken", {"broken"}, "x"),
        Simple("ok_two", {"ok_one"}, "2"),
    };
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    ScriptModuleContext ctx{lua};
    const ModuleInstallReport r = InstallScriptModules(ctx, mods);
    CHECK_EQ(r.Installed.size(), (size_t)2);
    CHECK_EQ(r.Failed.size(), (size_t)2);
    CHECK_TRUE(IsScriptModuleInstalled(lua, "ok_two"));
    CHECK_FALSE(IsScriptModuleInstalled(lua, "needs_broken"));
    // Причина видна и из скрипта.
    const std::string err = lua.script(
        "for _, m in ipairs(sage.modules.List()) do if m.name == 'broken' then return m.error end end");
    CHECK_TRUE(err.find("нет видеокарты") != std::string::npos);
}

TEST(ScriptModules_cycles_and_duplicates_are_refused) {
    std::vector<ScriptModule> mods = {Simple("ring_a", {"ring_b"}, "a"), Simple("ring_b", {"ring_a"}, "b")};
    sol::state lua;
    ScriptModuleContext ctx{lua};
    const ModuleInstallReport r = InstallScriptModules(ctx, mods);
    CHECK_EQ(r.Failed.size(), (size_t)2);
    CHECK_TRUE(r.Problems[0].find("кольцо") != std::string::npos);

    RegistryGuard g;
    g.Add(Simple("dup", {}, "1"));
    std::string err;
    CHECK_FALSE(RegisterScriptModule(Simple("dup", {}, "2"), &err));
    CHECK_TRUE(err.find("уже") != std::string::npos);
    CHECK_FALSE(RegisterScriptModule(ScriptModule{"", "", {}, true, {}}, &err));
}

TEST(ScriptModules_installs_once_and_waits_for_a_missing_dependency) {
    sol::state lua;
    ScriptModuleContext ctx{lua};
    int calls = 0;
    std::vector<ScriptModule> mods = {{"once", "", {}, true, [&calls](ScriptModuleContext&) { ++calls; }},
                                      Simple("waits", {"later"}, "w")};
    ModuleInstallReport r = InstallScriptModules(ctx, mods);
    CHECK_EQ(r.Waiting.size(), (size_t)1);
    // Второй хозяин того же состояния дал зависимость — ждавший доустановился,
    // а уже стоявший второй раз не ставится.
    mods.push_back(Simple("later", {}, "l"));
    r = InstallScriptModules(ctx, mods);
    CHECK_EQ(calls, 1);
    CHECK_TRUE(IsScriptModuleInstalled(lua, "waits"));
}

TEST(ScriptModules_module_for_the_new_api_waits_for_the_backend) {
    RegistryGuard g;
    g.Add({"spawner", "", {"Scene"}, true, [](ScriptModuleContext& c) {
               // Бэкенд нового API уже есть — на нём и строится модуль.
               c.Module("spawner")["ready"] = c.Backend != nullptr;
           }});
    ScriptEngine se;
    CHECK_FALSE(IsScriptModuleInstalled(se.Lua(), "spawner"));
    auto backend = MakeLuaBackend({&se});
    CHECK_TRUE(IsScriptModuleInstalled(se.Lua(), "spawner"));
    const bool ready = se.Lua().script("return sage.spawner.ready and spawner.ready");
    CHECK_TRUE(ready);
}

TEST(ScriptModules_disabled_module_is_absent_but_core_cannot_be_disabled) {
    RegistryGuard g;
    g.Add(Simple("needs_noise", {"procedural"}, "n"));
    SetDisabledScriptModules({"procedural", "object"});
    ScriptEngine se;
    const bool noNoise = se.Lua().script("return sage.noise == nil");
    CHECK_TRUE(noNoise);
    CHECK_FALSE(IsScriptModuleInstalled(se.Lua(), "needs_noise"));
    CHECK_TRUE(IsScriptModuleInstalled(se.Lua(), "object"));   // обязательный
}

// Независимость встроенных модулей: выключается любой необязательный — и
// движок собирается, а скрипт на остальном API работает. Скрытая связь между
// областями API (одна при установке трогает другую) здесь и всплывёт.
TEST(ScriptModules_every_optional_builtin_can_be_switched_off_alone) {
    std::vector<std::string> optional;
    {
        ScriptEngine probe;
        sol::table list = probe.Lua().script("return sage.modules.List()");
        for (size_t i = 1; i <= list.size(); ++i) optional.push_back(list[i]["name"].get<std::string>());
    }
    int checked = 0;
    for (const std::string& name : optional) {
        SetDisabledScriptModules({name});
        ScriptEngine se;
        sol::protected_function_result r = se.Lua().safe_script(
            "local v = Vec3(1, 2, 3); return v.x + sage.math.Clamp(5, 0, 1)", sol::script_pass_on_error);
        CHECK_TRUE(r.valid());
        if (r.valid()) CHECK_NEAR(r.get<float>(), 2.0f, 1e-6);
        ++checked;
    }
    SetDisabledScriptModules({});
    CHECK_TRUE(checked >= 26);
}
