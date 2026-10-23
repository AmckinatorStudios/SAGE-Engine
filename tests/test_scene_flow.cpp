// ХОД ИГРЫ ПО СЦЕНАМ (sage/scene/SceneFlow.h) и SceneManager.
//
// Что сторожится:
//   • замена активной сцены сценой с тем же именем не оставляет висячий
//     Active() (раньше указывал в освобождённую память);
//   • объект с меткой «переходит в следующую сцену» переезжает вместе с
//     потомками и на своё мировое место, а не задваивается;
//   • сцена поверх сцены добавляется и убирается группой;
//   • параметры scene:Load доходят до скриптов СЛЕДУЮЩЕЙ сцены, хотя
//     состояние Lua между сценами пересоздаётся;
//   • префаб, поставленный из скрипта, оживает (раньше его скрипт не
//     запускался вовсе).
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneFlow.h"
#include "sage/scene/SceneManager.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace fs = std::filesystem;
using namespace sage::scripting;

namespace {

fs::path FlowDir() {
    const fs::path d = fs::temp_directory_path() / "sage_scene_flow";
    fs::create_directories(d / "scenes");
    return d;
}

void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Сцены теста лежат в своей папке — так их находит scene:Load/LoadAdditive.
void UseSceneDir(const fs::path& dir) {
    sage::scene::SceneLocator where;
    where.Resolve = [dir](const std::string& name) {
        const fs::path p = dir / (name + ".sage");
        return fs::exists(p) ? p.string() : std::string();
    };
    where.List = [dir] {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".sage") out.push_back(e.path().stem().string());
        return out;
    };
    sage::scene::SetSceneLocator(std::move(where));
}

std::unique_ptr<ScriptingSystem> MakeSystem(Scene& scene, int& errors) {
    auto sys = std::make_unique<ScriptingSystem>();
    sys->AddBackend(MakeLuaBackend({}));
    ScriptServices services;
    services.ScenePtr = &scene;
    sys->Bind(services);
    sys->Runtime().SetErrorSink([&errors](const ScriptError& e) {
        ++errors;
        std::printf("       %s\n", e.Format().c_str());
    });
    return sys;
}

} // namespace

// --- SceneManager -----------------------------------------------------------

TEST(SceneManager_replacing_the_active_scene_keeps_Active_valid) {
    SceneManager sm;
    sm.CreateScene("Level");
    const std::string path = (FlowDir() / "level_same_name.sage").string();
    {
        Scene other("Level");
        other.CreateObject("FromFile");
        SceneSerializer::Save(other, path);
    }
    // Загрузить файл с ТЕМ ЖЕ именем, не делая его активным: прежняя сцена
    // уничтожается, и активной обязана стать новая, а не освобождённая память.
    sm.LoadScene(path, /*setActive=*/false);
    CHECK_TRUE(sm.Active() == sm.Get("Level"));
    CHECK_TRUE(sm.Active() != nullptr && sm.Active()->FindByName("FromFile").Valid());
    fs::remove(path);
}

TEST(SceneManager_unload_rename_and_list) {
    SceneManager sm;
    sm.CreateScene("B");
    sm.CreateScene("A");
    CHECK_EQ((int)sm.Count(), 2);
    CHECK_TRUE(sm.Names() == std::vector<std::string>({"A", "B"}));
    CHECK_TRUE(sm.Rename("B", "C"));
    CHECK_TRUE(!sm.Has("B") && sm.Has("C") && sm.Get("C")->Name() == "C");
    CHECK_TRUE(!sm.Rename("A", "C"));   // имя занято — не затираем чужую сцену
    // Активной была первая созданная (B -> C); выгрузили её — активной стала
    // оставшаяся, а не висячий указатель.
    CHECK_TRUE(sm.Active() == sm.Get("C"));
    CHECK_TRUE(sm.Unload("C"));
    CHECK_TRUE(sm.Active() == sm.Get("A"));
    CHECK_TRUE(sm.Unload("A"));
    CHECK_TRUE(sm.Active() == nullptr);
}

// --- Перенос объектов между сценами ------------------------------------------

TEST(SceneFlow_kept_object_moves_with_children_to_its_world_place) {
    Scene from("Level1");
    GameObject holder = from.CreateObject("Holder");
    holder.GetTransform().Position = glm::vec3(10.0f, 0.0f, 0.0f);
    GameObject player = from.CreateObject("Player");
    from.SetParent(player.Entity(), holder.Entity());
    player.GetTransform().Position = glm::vec3(1.0f, 2.0f, 0.0f);   // мировая: (11, 2, 0)
    from.Registry().emplace<KeepOnLoadComponent>(player.Entity());
    GameObject sword = from.CreateObject("Sword");
    from.SetParent(sword.Entity(), player.Entity());
    from.Registry().emplace<TagComponent>(sword.Entity(), TagComponent{"weapon"});
    from.CreateObject("Enemy");   // без метки — остаётся

    Scene to("Level2");
    auto kept = sage::scene::TakeKept(from);
    CHECK_EQ(sage::scene::PutKept(*kept, to), 1);
    GameObject p = to.FindByName("Player");
    CHECK_TRUE(p.Valid());
    CHECK_TRUE(!to.FindByName("Enemy").Valid() && !to.FindByName("Holder").Valid());
    if (!p.Valid()) return;
    CHECK_NEAR(p.GetTransform().Position.x, 11.0f, 1e-4f);
    CHECK_NEAR(p.GetTransform().Position.y, 2.0f, 1e-4f);
    CHECK_TRUE(to.Registry().all_of<KeepOnLoadComponent>(p.Entity()));   // едет и дальше
    GameObject s = to.FindByName("Sword");
    CHECK_TRUE(s.Valid() && to.ParentOf(s.Entity()) == p.Entity());
    CHECK_TRUE(s.Valid() && to.Registry().get<TagComponent>(s.Entity()).Tag == "weapon");
}

TEST(SceneFlow_kept_object_replaces_its_twin_instead_of_doubling) {
    Scene menu("Menu");
    GameObject gm = menu.CreateObject("GameManager");
    menu.Registry().emplace<KeepOnLoadComponent>(gm.Entity());
    gm.GetTransform().Position.x = 5.0f;   // «живое» состояние
    // Автор поставил менеджер и в следующую сцену — чтобы её можно было
    // запустить отдельно.
    Scene level("Level");
    GameObject local = level.CreateObject("GameManager");
    level.Registry().emplace<KeepOnLoadComponent>(local.Entity());
    auto kept = sage::scene::TakeKept(menu);
    sage::scene::PutKept(*kept, level);
    int managers = 0;
    for (auto e : level.Registry().view<NameComponent>())
        if (level.Registry().get<NameComponent>(e).Name == "GameManager") ++managers;
    CHECK_EQ(managers, 1);
    CHECK_NEAR(level.FindByName("GameManager").GetTransform().Position.x, 5.0f, 1e-5f);
}

// --- Сцена поверх сцены -------------------------------------------------------

TEST(SceneFlow_additive_scene_is_added_and_removed_as_a_group) {
    const fs::path dir = FlowDir() / "scenes";
    {
        Scene hud("Hud");
        GameObject panel = hud.CreateObject("HudPanel");
        GameObject label = hud.CreateObject("HudLabel");
        hud.SetParent(label.Entity(), panel.Entity());
        hud.CreateObject("HudLight").GetTransform().Position = glm::vec3(0.0f, 3.0f, 0.0f);
        SceneSerializer::Save(hud, (dir / "hud.sage").string());
    }
    UseSceneDir(dir);
    Scene level("Level");
    level.CreateObject("Ground");
    std::string err;
    const glm::vec3 shift(100.0f, 0.0f, 0.0f);
    const std::vector<int> made = sage::scene::LoadAdditive(level, "hud", err, {}, &shift);
    CHECK_TRUE(err.empty());
    CHECK_EQ((int)made.size(), 2);   // два корня: панель (с надписью) и свет
    CHECK_TRUE(level.FindByName("HudLabel").Valid());
    CHECK_NEAR(level.FindByName("HudLight").GetTransform().Position.x, 100.0f, 1e-4f);
    CHECK_TRUE(sage::scene::SceneExists("hud") && !sage::scene::SceneExists("nope"));
    const std::vector<std::string> all = sage::scene::ListScenes();
    CHECK_TRUE(std::find(all.begin(), all.end(), "hud") != all.end());

    CHECK_EQ(sage::scene::UnloadAdditive(level, "hud"), 2);
    CHECK_TRUE(!level.FindByName("HudPanel").Valid() && !level.FindByName("HudLabel").Valid());
    CHECK_TRUE(level.FindByName("Ground").Valid());   // своё не трогается

    sage::scene::LoadAdditive(level, "nope", err);
    CHECK_TRUE(!err.empty());
    sage::scene::SetSceneLocator({});
}

// --- Скрипты ------------------------------------------------------------------

// Параметры перехода переживают пересоздание состояния Lua: прочитать их
// обязана уже СЛЕДУЮЩАЯ сцена.
TEST(SceneFlow_load_params_reach_the_next_scene_scripts) {
    const fs::path dir = FlowDir();
    sage::scene::ResetTransit("menu");
    const std::string door = (dir / "door.lua").string();
    Write(door, R"LUA(
function Start()
    scene:Load("level2", { door = "north", coins = 7, flags = { true, false } })
end
)LUA");
    const std::string spawn = (dir / "spawn.lua").string();
    Write(spawn, R"LUA(
function Start()
    local p = scene:Params()
    if p.door == "north" and scene:Previous() == "menu" then
        self.transform:SetPosition(p.coins, #p.flags, 0)
    end
end
)LUA");
    int errors = 0;
    Scene menu("menu");
    menu.Registry().emplace<ScriptComponent>(menu.CreateObject("Door").Entity(), ScriptComponent{door});
    auto sys1 = MakeSystem(menu, errors);
    CHECK_EQ(sys1->AttachScene(menu), 1);
    std::string next;
    CHECK_TRUE(sys1->TakeSceneRequest(next));
    CHECK_TRUE(next == "level2");
    sys1->Shutdown();
    sys1.reset();   // как у хозяина кадра: старой системы больше нет

    sage::scene::EnterScene(next);
    Scene level("level2");
    GameObject hero = level.CreateObject("Hero");
    level.Registry().emplace<ScriptComponent>(hero.Entity(), ScriptComponent{spawn});
    auto sys2 = MakeSystem(level, errors);
    CHECK_EQ(sys2->AttachScene(level), 1);
    CHECK_NEAR(hero.GetTransform().Position.x, 7.0f, 1e-5f);
    CHECK_NEAR(hero.GetTransform().Position.y, 2.0f, 1e-5f);
    CHECK_EQ(errors, 0);
    sys2->Shutdown();
    sage::scene::ResetTransit();
}

// Префаб со скриптом, поставленный из скрипта, оживает сразу.
TEST(SceneFlow_instantiated_prefab_starts_its_script) {
    const fs::path dir = FlowDir();
    const std::string mover = (dir / "mover.lua").string();
    Write(mover, "function Start() self.transform:SetPosition(0, 9, 0) end\n");
    const std::string prefab = (dir / "mover.sageprefab").string();
    {
        Scene tmp("P");
        GameObject m = tmp.CreateObject("Mover");
        tmp.Registry().emplace<ScriptComponent>(m.Entity(), ScriptComponent{mover});
        std::string err;
        CHECK_TRUE(sage::scene::SavePrefab(tmp, m.Entity(), prefab, err));
    }
    sage::scene::ClearPrefabCache();
    const std::string spawner = (dir / "spawner.lua").string();
    Write(spawner, "function Start() scene:Instantiate(\"" + fs::path(prefab).generic_string() + "\") end\n");
    int errors = 0;
    Scene scene("S");
    scene.Registry().emplace<ScriptComponent>(scene.CreateObject("Spawner").Entity(), ScriptComponent{spawner});
    auto sys = MakeSystem(scene, errors);
    sys->AttachScene(scene);
    GameObject made = scene.FindByName("Mover");
    CHECK_TRUE(made.Valid());
    if (made.Valid()) CHECK_NEAR(made.GetTransform().Position.y, 9.0f, 1e-5f);
    CHECK_EQ(errors, 0);
    sys->Shutdown();
}

// scene:LoadAdditive / UnloadAdditive / Exists / List и obj:KeepOnLoad из Lua.
TEST(SceneFlow_lua_api_for_scenes) {
    const fs::path dir = FlowDir() / "scenes";
    const std::string lamp = (FlowDir() / "lamp.lua").string();
    Write(lamp, "function Start() self.transform:SetPosition(0, 4, 0) end\n");
    {
        Scene room("Room");
        GameObject l = room.CreateObject("RoomLamp");
        room.Registry().emplace<ScriptComponent>(l.Entity(), ScriptComponent{lamp});
        SceneSerializer::Save(room, (dir / "room.sage").string());
    }
    UseSceneDir(dir);
    const std::string probe = (FlowDir() / "probe.lua").string();
    Write(probe, R"LUA(
function Start()
    local roots = scene:LoadAdditive("room", Vector3(5, 0, 0))
    count = #roots
    exists = scene:Exists("room") and not scene:Exists("nope")
    listed = false
    for _, n in ipairs(scene:List()) do if n == "room" then listed = true end end
    self.gameObject:KeepOnLoad()
    kept = self.gameObject:IsKeptOnLoad()
end
function Unload() removed = scene:UnloadAdditive("room") end
)LUA");
    int errors = 0;
    Scene scene("Level");
    GameObject p = scene.CreateObject("Probe");
    scene.Registry().emplace<ScriptComponent>(p.Entity(), ScriptComponent{probe});
    auto sys = MakeSystem(scene, errors);
    sys->AttachScene(scene);
    GameObject lampObj = scene.FindByName("RoomLamp");
    CHECK_TRUE(lampObj.Valid());
    // Скрипт добавленной сцены запущен: Start поднял лампу на 4.
    if (lampObj.Valid()) CHECK_NEAR(lampObj.GetTransform().Position.y, 4.0f, 1e-5f);
    CHECK_TRUE(scene.Registry().all_of<KeepOnLoadComponent>(p.Entity()));
    CHECK_TRUE(sys->Runtime().Invoke(p.Entity(), "Unload", {}));
    CHECK_TRUE(!scene.FindByName("RoomLamp").Valid());
    CHECK_EQ(errors, 0);
    sys->Shutdown();
    sage::scene::SetSceneLocator({});
}

// То же из SAGE Lua (.slua): Scene.load(name, params) и Scene.params().
TEST(SceneFlow_sage_lua_scene_load_with_params) {
    const fs::path dir = FlowDir();
    sage::scene::ResetTransit("a");
    const std::string go = (dir / "SceneGo.slua").string();
    Write(go, R"SAGE(
class SceneGo extends Component
    function onStart()
        Scene.load("b", { spawn = "east", lives = 3 })
    end
end
)SAGE");
    const std::string read = (dir / "SceneRead.slua").string();
    Write(read, R"SAGE(
class SceneRead extends Component
    function onStart()
        local p = Scene.params()
        if p.spawn == "east" and Scene.previous() == "a" then
            self.transform:SetPosition(p.lives, 0, 0)
        end
    end
end
)SAGE");
    int errors = 0;
    Scene a("a");
    a.Registry().emplace<ScriptComponent>(a.CreateObject("Go").Entity(), ScriptComponent{go});
    auto s1 = MakeSystem(a, errors);
    s1->AttachScene(a);
    std::string next;
    CHECK_TRUE(s1->TakeSceneRequest(next) && next == "b");
    s1->Shutdown();
    s1.reset();
    sage::scene::EnterScene(next);
    Scene b("b");
    GameObject r = b.CreateObject("Read");
    b.Registry().emplace<ScriptComponent>(r.Entity(), ScriptComponent{read});
    auto s2 = MakeSystem(b, errors);
    s2->AttachScene(b);
    CHECK_NEAR(r.GetTransform().Position.x, 3.0f, 1e-5f);
    CHECK_EQ(errors, 0);
    s2->Shutdown();
    sage::scene::ResetTransit();
}
