// Несколько скриптов на одном объекте (ScriptComponent::Scripts): у каждого
// свой экземпляр, свои переменные и своя судьба при ошибке; порядок
// исполнения — порядок списка; группы — только раскладка инспектора. Плюс
// сохранение сцены в обоих видах (прежний — для одного скрипта) и чтение
// сцен до списка скриптов.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scene/ScriptComponent.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace {

std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = "sage_multi_" + name + ".lua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    return path;
}

sage::vars::Table Fields(const char* name, float value) {
    sage::vars::Table t;
    sage::vars::Var v;
    v.Name = name;
    v.Data = sage::vars::Value(value);
    v.Declared = true;
    t.Put(v);
    return t;
}

struct World {
    Scene scene{"multi"};
    ScriptEngine engine;
    std::unique_ptr<sage::scripting::ScriptingSystem> sys;
    std::vector<std::string> files;
    std::vector<sage::scripting::ScriptError> errors;

    World() {
        engine.BindScene(scene);
        sys = std::make_unique<sage::scripting::ScriptingSystem>();
        sys->AddBackend(sage::scripting::MakeLuaBackend({&engine}));
        sage::scripting::ScriptServices services;
        services.ScenePtr = &scene;
        sys->Bind(services);
        sys->Runtime().SetErrorSink([this](const sage::scripting::ScriptError& e) { errors.push_back(e); });
        // Общий журнал: окружения скриптов изолированы, а глобальные движка
        // видны всем — так проверяется ПОРЯДОК вызовов разных скриптов.
        engine.Lua().script("journal = {}");
    }
    ~World() {
        sys->Shutdown();
        for (const std::string& f : files) std::remove(f.c_str());
    }
    std::string File(const std::string& name, const std::string& body) {
        files.push_back(WriteScript(name, body));
        return files.back();
    }
    std::string Journal() {
        return engine.Lua().script("return table.concat(journal, ',')").get<std::string>();
    }
    template <typename T>
    T Eval(const std::string& code) {
        return engine.Lua().script(code).get<T>();
    }
};

// Журналирующий скрипт: Start и Update пишут своё имя и значение speed.
std::string Logger(const char* tag) {
    return std::string("speed = 1\nfunction Start() journal[#journal + 1] = '") + tag +
           ":start:' .. string.format('%g', speed) end\nfunction Update(dt) journal[#journal + 1] = '" + tag + "' end\n";
}

} // namespace

// Два скрипта на объекте: оба живут, у каждого свой speed, порядок Start и
// Update — порядок списка, группа его не меняет.
TEST(MultiScript_two_scripts_run_in_list_order_with_own_fields) {
    World w;
    GameObject o = w.scene.CreateObject("Hero");
    ScriptComponent sc;
    sc.Add(w.File("a", Logger("a")), Fields("speed", 3.0f));
    sc.Add(w.File("b", Logger("b")), Fields("speed", 7.0f));
    // Первый скрипт — в группе, второй — нет: на порядок это не влияет.
    sc.Groups.push_back("Movement");
    sc.Scripts[0].Group = "Movement";
    w.scene.Registry().emplace<ScriptComponent>(o.Entity(), sc);

    CHECK_EQ(w.sys->AttachScene(w.scene), 2);
    w.sys->Update(0.1f);
    CHECK_EQ(w.Journal(), std::string("a:start:3,b:start:7,a,b"));
    CHECK_EQ((int)w.sys->Runtime().FindAll(o.Entity()).size(), 2);
    CHECK_TRUE(w.errors.empty());
}

// GetScript без имени — первый, с именем — нужный; Call находит метод в
// любом скрипте объекта; поля скриптов независимы.
TEST(MultiScript_get_script_by_name_and_call_any_method) {
    World w;
    GameObject o = w.scene.CreateObject("Hero");
    ScriptComponent sc;
    sc.Add(w.File("move", "speed = 1\nfunction Go() return 'go' end\n"), Fields("speed", 2.0f));
    sc.Add(w.File("health", "speed = 1\nhp = 10\nfunction Hit(n) hp = hp - n return hp end\n"),
           Fields("speed", 9.0f));
    w.scene.Registry().emplace<ScriptComponent>(o.Entity(), sc);
    w.sys->AttachScene(w.scene);

    w.engine.Lua()["__id"] = o.Id();
    CHECK_NEAR(w.Eval<float>("return Scene.FindById(__id):GetScript().speed"), 2.0f, 1e-5);
    CHECK_NEAR(w.Eval<float>("return Scene.FindById(__id):GetScript('sage_multi_health').speed"), 9.0f, 1e-5);
    CHECK_NEAR(w.Eval<float>("return Scene.FindById(__id):GetScript('sage_multi_health.lua').speed"), 9.0f, 1e-5);
    CHECK_TRUE(w.Eval<bool>("return Scene.FindById(__id):GetScript('nobody') == nil"));
    // Метод второго скрипта — через объект, не зная, какой скрипт его объявил.
    CHECK_EQ(w.Eval<int>("return Scene.FindById(__id):Call('Hit', 3)"), 7);
    CHECK_EQ(w.Eval<std::string>("return Scene.FindById(__id):Call('Go')"), std::string("go"));
    // Изменение переменной одного скрипта не задело другой.
    w.Eval<int>("Scene.FindById(__id):GetScript().speed = 100 return 0");
    CHECK_NEAR(w.Eval<float>("return Scene.FindById(__id):GetScript('sage_multi_health').speed"), 9.0f, 1e-5);
    // Рантайм: метод ищется по всем скриптам.
    const sage::scripting::LiveScript* withHit = w.sys->Runtime().FindMethod(o.Entity(), "Hit");
    CHECK_TRUE(withHit != nullptr && withHit->Slot == 1);
}

// Пропавший файл и скрипт с ошибкой — сообщение в консоль, соседи живут.
TEST(MultiScript_missing_or_failing_script_does_not_break_neighbours) {
    World w;
    GameObject o = w.scene.CreateObject("Hero");
    ScriptComponent sc;
    sc.Add(w.File("ok1", Logger("ok1")));
    sc.Add("sage_multi_no_such_file.lua");
    sc.Add(w.File("broken", "function Update(dt) error('boom') end\n"));
    sc.Add(w.File("syntax", "function Update(dt\n"));
    sc.Add(std::string()); // добавлен в инспекторе, файл не выбран — не ошибка
    sc.Add(w.File("ok2", Logger("ok2")));
    w.scene.Registry().emplace<ScriptComponent>(o.Entity(), sc);

    CHECK_EQ(w.sys->AttachScene(w.scene), 3); // ok1, broken, ok2
    bool missingReported = false;
    for (const auto& e : w.errors)
        if (e.File == "sage_multi_no_such_file.lua") missingReported = true;
    CHECK_TRUE(missingReported);
    w.errors.clear();
    for (int i = 0; i < 5; ++i) w.sys->Update(0.1f);
    // Падающий скрипт замолчал после нескольких ошибок, соседи — работали.
    CHECK_TRUE(!w.errors.empty() && w.errors.size() <= 3);
    CHECK_EQ(w.Journal(), std::string("ok1:start:1,ok2:start:1,ok1,ok2,ok1,ok2,ok1,ok2,ok1,ok2,ok1,ok2"));
}

// Перезагрузка одного файла: значения полей сохраняются, сосед не
// перезапускается, порядок не меняется.
TEST(MultiScript_reload_keeps_fields_order_and_neighbour_state) {
    World w;
    GameObject o = w.scene.CreateObject("Hero");
    const std::string a = w.File("ra", "speed = 1\ncount = 0\nfunction Update(dt) count = count + 1 end\n");
    const std::string b = w.File("rb", "speed = 1\ncount = 0\nfunction Update(dt) count = count + 1 end\n");
    ScriptComponent sc;
    sc.Add(a, Fields("speed", 4.0f));
    sc.Add(b, Fields("speed", 5.0f));
    w.scene.Registry().emplace<ScriptComponent>(o.Entity(), sc);
    w.sys->AttachScene(w.scene);
    w.sys->Update(0.1f);
    w.sys->Update(0.1f);

    // Правка файла: новая переменная со своим умолчанием.
    { std::ofstream f(a, std::ios::binary); f << "speed = 1\nextra = 42\ncount = 0\nfunction Update(dt) count = count + 10 end\n"; }
    CHECK_EQ(w.sys->Runtime().Reload(a), 1);
    w.sys->Update(0.1f);

    w.engine.Lua()["__id"] = o.Id();
    CHECK_NEAR(w.Eval<float>("return Scene.FindById(__id):GetScript().speed"), 4.0f, 1e-5);  // настройка жива
    CHECK_EQ(w.Eval<int>("return Scene.FindById(__id):GetScript().extra"), 42);             // умолчание новой
    CHECK_EQ(w.Eval<int>("return Scene.FindById(__id):GetScript().count"), 10);             // свежий экземпляр
    CHECK_EQ(w.Eval<int>("return Scene.FindById(__id):GetScript('sage_multi_rb').count"), 3); // сосед не тронут
    const auto all = w.sys->Runtime().FindAll(o.Entity());
    CHECK_TRUE(all.size() == 2 && all[0]->Path == a && all[1]->Path == b);
}

// Скрипт, добавленный кодом, — к уже висящим, а не вместо; Start, создающий
// объекты со скриптами, не ломает обход (хранилище не перевыделяется).
TEST(MultiScript_add_component_adds_and_spawning_in_start_is_safe) {
    World w;
    const std::string child = w.File("child", "function Start() journal[#journal + 1] = 'child' end\n");
    const std::string extra = w.File("extra", "function Start() journal[#journal + 1] = 'extra' end\n");
    GameObject o = w.scene.CreateObject("Spawner");
    w.scene.Registry().emplace<ScriptComponent>(o.Entity(), w.File("spawner",
        "function Start()\n"
        "  for i = 1, 40 do\n"
        "    local c = Scene.Create('c' .. i)\n"
        "    c:AddComponent('Script', {path = '" + child + "'})\n"
        "  end\n"
        "  self.gameObject:AddComponent('Script', {path = '" + extra + "'})\n"
        "  journal[#journal + 1] = 'spawner'\n"
        "end\n"));
    w.sys->AttachScene(w.scene);
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    const ScriptComponent& sc = w.scene.Registry().get<ScriptComponent>(o.Entity());
    CHECK_EQ((int)sc.Scripts.size(), 2);
    CHECK_EQ((int)w.sys->Runtime().FindAll(o.Entity()).size(), 2);
    CHECK_EQ(w.Eval<int>("return #journal"), 42);
    // Снятие объекта снимает ВСЕ его скрипты.
    w.sys->Detach(o);
    CHECK_EQ((int)w.sys->Runtime().FindAll(o.Entity()).size(), 0);
}

// Сохранение: один скрипт без групп — прежние ключи; иначе список с
// группами и значениями. Загрузка восстанавливает всё.
TEST(MultiScript_scene_round_trip_and_formats) {
    Scene scene("save");
    GameObject single = scene.CreateObject("Single");
    scene.Registry().emplace<ScriptComponent>(single.Entity(), ScriptComponent("scripts/a.lua", Fields("speed", 2.0f)));
    GameObject multi = scene.CreateObject("Multi");
    ScriptComponent sc;
    sc.Add("scripts/a.lua", Fields("speed", 3.0f));
    sc.Add("scripts/b.lua", Fields("speed", 4.0f)).Group = "Combat";
    sc.Groups = {"Combat", "Empty"};
    scene.Registry().emplace<ScriptComponent>(multi.Entity(), sc);
    GameObject none = scene.CreateObject("NoFileYet");
    scene.Registry().emplace<ScriptComponent>(none.Entity());

    const std::string text = SceneSerializer::SaveToString(scene);
    const nlohmann::json j = nlohmann::json::parse(text);
    bool singleOld = false, multiNew = false;
    for (const auto& obj : j["objects"]) {
        if (obj.value("name", "") == "Single") singleOld = obj.contains("script") && !obj.contains("scripts");
        if (obj.value("name", "") == "Multi") multiNew = obj.contains("scripts") && !obj.contains("script");
    }
    CHECK_TRUE(singleOld);
    CHECK_TRUE(multiNew);

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    auto get = [&](const char* name) {
        GameObject g = loaded->FindByName(name);
        return g.Valid() ? loaded->Registry().try_get<ScriptComponent>(g.Entity()) : nullptr;
    };
    const ScriptComponent* s1 = get("Single");
    const ScriptComponent* s2 = get("Multi");
    const ScriptComponent* s3 = get("NoFileYet");
    CHECK_TRUE(s1 && s1->Scripts.size() == 1 && s1->FirstPath() == "scripts/a.lua");
    CHECK_TRUE(s1 && s1->Scripts.size() == 1 && std::abs(s1->Scripts[0].Fields.Get("speed").AsFloat() - 2.0f) < 1e-5f);
    CHECK_TRUE(s2 && s2->Scripts.size() == 2);
    if (s2 && s2->Scripts.size() == 2) {
        CHECK_EQ(s2->Scripts[1].Path, std::string("scripts/b.lua"));
        CHECK_EQ(s2->Scripts[1].Group, std::string("Combat"));
        CHECK_TRUE(s2->Scripts[0].Group.empty());
        CHECK_NEAR(s2->Scripts[0].Fields.Get("speed").AsFloat(), 3.0f, 1e-5);
        CHECK_NEAR(s2->Scripts[1].Fields.Get("speed").AsFloat(), 4.0f, 1e-5);
        CHECK_TRUE(s2->Groups == std::vector<std::string>({"Combat", "Empty"}));
    }
    // Компонент без выбранного файла раньше пропадал при сохранении.
    CHECK_TRUE(s3 && s3->Scripts.empty());
}

// Сцена прежнего формата (один "script" и "scriptVars") читается в список.
TEST(MultiScript_old_scene_format_loads) {
    Scene scene("old");
    GameObject o = scene.CreateObject("Old");
    scene.Registry().emplace<ScriptComponent>(o.Entity(), ScriptComponent("scripts/old.lua", Fields("speed", 6.0f)));
    nlohmann::json j = nlohmann::json::parse(SceneSerializer::SaveToString(scene));
    // Ровно то, что писали прежние версии: без списка и без групп.
    for (auto& obj : j["objects"]) {
        if (obj.value("name", "") != "Old") continue;
        obj.erase("scripts");
        obj.erase("scriptGroups");
        obj["script"] = "scripts/old.lua";
    }
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(j.dump());
    GameObject g = loaded->FindByName("Old");
    const ScriptComponent* sc = g.Valid() ? loaded->Registry().try_get<ScriptComponent>(g.Entity()) : nullptr;
    CHECK_TRUE(sc && sc->Scripts.size() == 1);
    if (sc && sc->Scripts.size() == 1) {
        CHECK_EQ(sc->Scripts[0].Path, std::string("scripts/old.lua"));
        CHECK_NEAR(sc->Scripts[0].Fields.Get("speed").AsFloat(), 6.0f, 1e-5);
        CHECK_TRUE(sc->Groups.empty());
    }
}
