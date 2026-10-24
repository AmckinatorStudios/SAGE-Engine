// Плагины на Lua (sage/scripting/Plugins.h): манифест, поиск, запуск части
// движка до скриптов сцены и API plugins.* для скриптов игры.
#include "TestFramework.h"

#include <filesystem>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/Plugins.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace fs = std::filesystem;
namespace pl = sage::plugins;

namespace {

void Put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

} // namespace

TEST(Plugins_manifest_reads_simple_settings_and_round_trips) {
    const pl::Manifest m = pl::ParseManifest(R"({
        "name": "Счётчик монет", "version": "2.1", "author": "Я",
        "runtime": "runtime.lua", "editor": "editor.lua", "enabled": false,
        "settings": { "speed": 5.5, "lives": 3, "greet": "hi", "debug": true }
    })", "plugins/coins");
    CHECK_TRUE(m.Error.empty());
    CHECK_EQ(m.Id, std::string("coins"));   // id не указан — имя папки
    CHECK_TRUE(!m.Enabled);
    CHECK_TRUE(m.HasRuntime() && m.HasEditor());
    CHECK_NEAR(m.Settings.Get("speed").AsFloat(), 5.5f, 1e-5f);
    CHECK_EQ(m.Settings.Get("lives").AsInt(), 3);
    CHECK_TRUE(m.Settings.Get("debug").AsBool());

    const pl::Manifest back = pl::ParseManifest(pl::ManifestToText(m), "plugins/coins");
    CHECK_EQ(back.Name, m.Name);
    CHECK_EQ(back.Settings.Get("greet").AsString(), std::string("hi"));
    CHECK_TRUE(!back.Enabled);
    // Простое значение пишется простым — файл читают и правят руками.
    CHECK_TRUE(pl::ManifestToText(m).find("\"lives\": 3") != std::string::npos);
}

TEST(Plugins_broken_manifest_is_reported_not_thrown) {
    CHECK_TRUE(!pl::ParseManifest("{ name: oops", "plugins/x").Error.empty());
    CHECK_TRUE(!pl::ParseManifest(R"({"name":"nothing to run"})", "plugins/x").Error.empty());
}

TEST(Plugins_make_id_transliterates_and_cleans) {
    CHECK_EQ(pl::MakeId("Счётчик монет"), std::string("schetchik_monet"));
    CHECK_EQ(pl::MakeId("My Cool Plugin!"), std::string("my_cool_plugin"));
    CHECK_EQ(pl::MakeId("3D tools"), std::string("p_3d_tools"));
    CHECK_EQ(pl::MakeId("!!!"), std::string("plugin"));
}

// Часть движка: запускается в каждой сцене до скриптов объектов, отдаёт им
// своё API, получает настройки из plugin.json и не попадает в файл сцены.
TEST(Plugins_runtime_part_runs_before_scene_scripts_and_shares_its_api) {
    const fs::path root = fs::absolute("sage_plugins_test");
    fs::remove_all(root);
    Put(root / "coins" / "plugin.json",
        R"({"name":"Coins","runtime":"runtime.lua","settings":{"start":10}})");
    Put(root / "coins" / "runtime.lua", R"LUA(
start = 0
local Coins = { total = 0 }
function Coins.add(n) Coins.total = Coins.total + n; return Coins.total end
plugins.Register("coins", Coins)
function Start() Coins.total = start end
)LUA");
    Put(root / "off" / "plugin.json", R"({"runtime":"runtime.lua","enabled":false})");
    Put(root / "off" / "runtime.lua", "plugins.Register('off', {})\n");

    const std::vector<pl::Manifest> found = pl::Scan(root);
    CHECK_EQ((int)found.size(), 2);

    const std::string coin = (root / "coin.lua").string();
    Put(coin, R"LUA(
function Start()
    local c = plugins.Get("coins")
    local got = c and c.add(5) or -2
    if plugins.Has("off") then got = -3 end
    self.transform:SetPosition(got, 0, 0)
end
)LUA");

    Scene scene("plugins");
    GameObject obj = scene.CreateObject("Coin");
    scene.Registry().emplace<ScriptComponent>(obj.Entity()).Path = coin;

    sage::scripting::ScriptingSystem sys;
    sys.AddBackend(sage::scripting::MakeLuaBackend({}));
    sage::scripting::ScriptServices services;
    services.ScenePtr = &scene;
    sys.Bind(services);
    sys.SetPlugins(found);
    sys.AttachScene(scene);
    CHECK_EQ(sys.AttachedPlugins(), 1);   // выключенный не запускается

    // Start плагина (total = start = 10 из plugin.json) — раньше Start монеты:
    // монета получила 10 + 5.
    CHECK_NEAR(obj.GetTransform().Position.x, 15.0f, 1e-4f);
    // Служебный объект плагина — не часть сцены: в файл не пишется.
    const std::string saved = SceneSerializer::SaveToString(scene);
    CHECK_TRUE(saved.find("plugin:coins") == std::string::npos);
    int hosts = 0;
    for (auto e : scene.Registry().view<pl::PluginHostComponent>()) { (void)e; ++hosts; }
    CHECK_EQ(hosts, 1);
    fs::remove_all(root);
}

// --- Часть редактора (editor/src/LuaPlugins.h) --------------------------------
#include "../editor/src/EditorHistory.h"
#include "../editor/src/LuaPlugins.h"

namespace lpl = sage::editor::luaplugins;

// Шаблон «Создать плагин» — первое, что человек видит. Он обязан выполниться
// без единой ошибки и дать ровно то, что обещает: пункт меню и окно.
TEST(Plugins_editor_template_runs_and_registers_menu_and_panel) {
    const lpl::DryRun r = lpl::Try(lpl::TemplateEditorLua("Проба"), "editor.lua");
    CHECK_TRUE(r.Ok);
    if (!r.Ok) std::printf("       %s\n", r.Error.c_str());
    CHECK_EQ((int)r.Menus.size(), 1);
    CHECK_EQ((int)r.Panels.size(), 1);
    CHECK_EQ(r.Panels.empty() ? std::string() : r.Panels[0], std::string("Проба"));
}

TEST(Plugins_runtime_template_is_valid_lua_with_settings) {
    const std::string src = lpl::TemplateRuntimeLua("proba", "Проба");
    const sage::vars::Table fields = sage::scripting::ParseFields("runtime.lua", src);
    CHECK_TRUE(fields.Find("message") != nullptr);   // настройка видна окну «Плагины»
}

// Ошибка и бесконечный цикл — в отчёт, а не в падение или зависание редактора.
TEST(Plugins_editor_errors_and_endless_loops_are_reported) {
    const lpl::DryRun bad = lpl::Try("editor.menu(", "bad.lua");
    CHECK_TRUE(!bad.Ok && !bad.Error.empty());
    const lpl::DryRun wrong = lpl::Try("editor.on('jump', function() end)", "wrong.lua");
    CHECK_TRUE(!wrong.Ok && wrong.Error.find("update") != std::string::npos);   // подсказка, какие есть
    const auto t0 = std::chrono::steady_clock::now();
    const lpl::DryRun loop = lpl::Try("while true do end", "loop.lua");
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_TRUE(!loop.Ok);
    CHECK_TRUE(sec < 5.0);
}

// Один вызов плагина — один шаг отмены, сколько бы снимков он ни положил.
TEST(Plugins_history_collapses_one_call_into_one_step) {
    int state = 0;
    EditorHistory h([&] { return std::to_string(state); },
                    [&](const std::string& s) { state = std::stoi(s); return true; });
    h.Push(); state = 1;          // правка до плагина
    const size_t depth = h.UndoDepth();
    h.Push(); state = 2;          // плагин: три создания
    h.Push(); state = 3;
    h.Push(); state = 4;
    h.CollapseSince(depth);
    CHECK_EQ((int)h.UndoDepth(), 2);
    CHECK_TRUE(h.Undo());
    CHECK_EQ(state, 1);            // одним шагом — до плагина
}

// Установка: папка плагина копируется в «куда»; повторная установка из той же
// папки не стирает единственную копию.
TEST(Plugins_install_copies_folder_and_survives_reinstall_from_itself) {
    const fs::path root = fs::absolute("sage_plugins_install");
    fs::remove_all(root);
    Put(root / "downloads" / "Tool" / "plugin.json", R"({"id":"tool","editor":"editor.lua"})");
    Put(root / "downloads" / "Tool" / "editor.lua", "editor.menu('x', function() end)\n");
    std::string err;
    const std::string id = lpl::Install(root / "downloads" / "Tool", root / "plugins", err);
    CHECK_EQ(id, std::string("tool"));
    CHECK_TRUE(fs::exists(root / "plugins" / "tool" / "editor.lua"));
    // Выбрали уже установленную папку — плагин на месте.
    CHECK_EQ(lpl::Install(root / "plugins" / "tool", root / "plugins", err), std::string("tool"));
    CHECK_TRUE(fs::exists(root / "plugins" / "tool" / "editor.lua"));
    // Не плагин — понятная ошибка, а не пустая папка в plugins/.
    fs::create_directories(root / "empty");
    CHECK_TRUE(lpl::Install(root / "empty", root / "plugins", err).empty());
    CHECK_TRUE(!err.empty());
    fs::remove_all(root);
}
