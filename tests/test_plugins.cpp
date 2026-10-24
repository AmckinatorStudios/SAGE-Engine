// Плагины на Lua (sage/scripting/Plugins.h): манифест, поиск, запуск части
// движка до скриптов сцены и API plugins.* для скриптов игры.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/Plugins.h"
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
