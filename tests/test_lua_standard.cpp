// Стандарт Lua API SAGE: `object`, функции жизненного цикла в snake_case,
// `events.create/trigger/connect/connect_once/disconnect`, аннотации `---@`.
// Связка — та же, что у редактора (бэкенд поверх прежнего ScriptEngine).
#include "TestFramework.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sage/input/InputEvent.h"
#include "sage/input/InputSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/ScriptComponent.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/lua/LuaInternal.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

using sage::scripting::Hook;
using sage::vars::Kind;

namespace {

std::string Write(const std::string& name, const std::string& body) {
    const std::string path = "sage_simple_" + name + ".lua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    return path;
}

// Мир теста: сцена, ввод, прежний движок и система скриптинга над ним.
struct World {
    Scene scene{"simple"};
    sage::input::InputSystem input;
    ScriptEngine engine;
    std::unique_ptr<sage::scripting::ScriptingSystem> sys;
    std::vector<std::string> files;
    std::vector<sage::scripting::ScriptError> errors;

    World() {
        engine.BindScene(scene);
        engine.BindInput(input);
        sys = std::make_unique<sage::scripting::ScriptingSystem>();
        sys->AddBackend(sage::scripting::MakeLuaBackend({&engine}));
        sage::scripting::ScriptServices services;
        services.ScenePtr = &scene;
        services.Input = &input;
        sys->Bind(services);
        sys->Runtime().SetErrorSink([this](const sage::scripting::ScriptError& e) { errors.push_back(e); });
    }
    ~World() {
        sys->Shutdown();
        for (const std::string& f : files) std::remove(f.c_str());
    }

    GameObject Spawn(const std::string& name, const std::string& file, const std::string& body,
                     const sage::vars::Table& fields = {}) {
        const std::string path = Write(file, body);
        files.push_back(path);
        GameObject o = scene.CreateObject(name);
        scene.Registry().emplace<ScriptComponent>(o.Entity(), path, fields);
        return o;
    }
    void Start() { sys->AttachScene(scene); }
    // Кадр: ввод, затем скрипты — как в редакторе.
    void Frame(float dt = 0.1f) {
        input.BeginFrame();
        sys->Update(dt);
    }
    // Значение глобальной переменной скрипта объекта (через его self).
    template <typename T>
    T Global(GameObject o, const char* name) {
        sol::state& L = engine.Lua();
        L["__probe"] = o.Id();
        return L.script(std::string("return Scene.FindById(__probe):GetScript().") + name).get<T>();
    }
};

} // namespace

// Скрипт в стандарте: object, start/update(delta), точки вместо двоеточий,
// строчные разделы (input, scene, debug), без self и без Pascal-имён.
TEST(LuaStandard_object_lifecycle_and_dot_calls) {
    World w;
    GameObject p = w.Spawn("Player", "player", R"(
move_speed = 5.0

function start()
    debug.log("Started")
    started = true
    name_at_start = object.name
    has_cam = object.has_component("camera")
    local c = object.add_component("camera")
    has_cam_after = object.has_component("camera")
    cam_ok = c ~= nil
end

function update(delta)
    object.transform.position = object.transform.position + vector3.new(0, 0, 1) * move_speed * delta
    ticks = (ticks or 0) + 1
    fwd = object.transform.forward()
end

function late_update(delta) late = (late or 0) + 1 end
function fixed_update(delta) fixed = (fixed or 0) + 1 end
function on_destroy() destroyed = true end
)");
    w.Start();
    CHECK_TRUE(w.errors.empty());
    w.Frame(0.5f);
    w.Frame(0.5f);
    CHECK_TRUE(w.Global<bool>(p, "started"));
    CHECK_EQ(w.Global<std::string>(p, "name_at_start"), std::string("Player"));
    CHECK_FALSE(w.Global<bool>(p, "has_cam"));
    CHECK_TRUE(w.Global<bool>(p, "has_cam_after"));
    CHECK_TRUE(w.Global<bool>(p, "cam_ok"));
    CHECK_NEAR((*p.Registry()).get<Transform>(p.Entity()).Position.z, 5.0f, 1e-4f);
    CHECK_EQ(w.Global<int>(p, "ticks"), 2);
    w.sys->LateUpdate(0.1f);
    w.sys->FixedUpdate(0.02f);
    CHECK_EQ(w.Global<int>(p, "late"), 1);
    CHECK_EQ(w.Global<int>(p, "fixed"), 1);
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
}

// Собственные события: create / trigger / connect / connect_once / disconnect.
TEST(LuaStandard_events_create_trigger_connect_disconnect) {
    World w;
    GameObject p = w.Spawn("Player", "hero", R"(
events.create(object, "player_died")
events.create(object, "health_changed")

function die(reason)
    events.trigger(object, "player_died", {reason = reason, position = object.transform.position})
end
)");
    GameObject l = w.Spawn("Listener", "listener", R"(
got = 0
once_got = 0
local player = scene.find("Player")

local function on_player_died(event)
    got = got + 1
    last_reason = event.data.reason
    sender_name = event.sender.name
    event_name = event.name
end
handler = on_player_died
events.connect(player, "player_died", on_player_died)
events.connect(player, "player_died", function(event) second = (second or 0) + 1 end)
events.connect_once(player, "player_died", function(event) once_got = once_got + 1 end)

function off() return events.disconnect(player, "player_died", on_player_died) end
)");
    w.Start();
    CHECK_TRUE(w.errors.empty());
    sol::state& L = w.engine.Lua();
    auto die = [&](const char* reason) {
        L["__probe"] = p.Id();
        L["__reason"] = reason;
        L.script("Scene.FindById(__probe):Call(\"die\", __reason)");
    };
    die("fall");
    CHECK_EQ(w.Global<int>(l, "got"), 1);
    CHECK_EQ(w.Global<int>(l, "second"), 1);          // несколько обработчиков на одно событие
    CHECK_EQ(w.Global<int>(l, "once_got"), 1);
    CHECK_EQ(w.Global<std::string>(l, "last_reason"), std::string("fall"));
    CHECK_EQ(w.Global<std::string>(l, "sender_name"), std::string("Player"));
    CHECK_EQ(w.Global<std::string>(l, "event_name"), std::string("player_died"));
    die("lava");
    CHECK_EQ(w.Global<int>(l, "got"), 2);
    CHECK_EQ(w.Global<int>(l, "once_got"), 1);        // однократный больше не звался
    L["__probe"] = l.Id();
    CHECK_EQ(L.script("return Scene.FindById(__probe):Call(\"off\")").get<int>(), 1);
    die("void");
    CHECK_EQ(w.Global<int>(l, "got"), 2);             // отключённый молчит
    CHECK_EQ(w.Global<int>(l, "second"), 3);          // соседний — нет
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
}

// Глобальные события — той же четвёркой, без объекта первым аргументом.
TEST(LuaStandard_global_events_use_same_functions) {
    World w;
    GameObject a = w.Spawn("A", "ga", R"(
count = 0
events.connect("level_loaded", function(event) count = count + 1; where = event.data end)
)");
    w.Spawn("B", "gb", R"(
function start() events.trigger("level_loaded", "forest") end
)");
    w.Start();
    CHECK_EQ(w.Global<int>(a, "count"), 1);
    CHECK_EQ(w.Global<std::string>(a, "where"), std::string("forest"));
    CHECK_TRUE(w.errors.empty());
}

// Компоненты по snake_case-именам, поля — snake_case.
TEST(LuaStandard_components_snake_case) {
    World w;
    GameObject o = w.Spawn("Box", "box", R"(
function start()
    local body = object.add_component("rigid_body", {type = "dynamic", mass = 3, linear_damping = 0.4})
    mass_added = body.mass
    body.mass = 2.0
    mass_after = object.get_component("rigid_body").mass
    damping = body.linear_damping
    has = object.has_component("rigid_body")
    object.remove_component("rigid_body")
    gone = object.get_component("rigid_body") == nil
end
)");
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    CHECK_NEAR(w.Global<float>(o, "mass_added"), 3.0f, 1e-5f);
    CHECK_NEAR(w.Global<float>(o, "mass_after"), 2.0f, 1e-5f);
    CHECK_NEAR(w.Global<float>(o, "damping"), 0.4f, 1e-5f);
    CHECK_TRUE(w.Global<bool>(o, "has"));
    CHECK_TRUE(w.Global<bool>(o, "gone"));
}

// Сцена, объекты и разделы строчными, всё через точку.
TEST(LuaStandard_scene_and_object_dot_api) {
    World w;
    GameObject o = w.Spawn("Main", "main", R"(
function start()
    local box = scene.create("Box")
    box.transform.position = vector3.new(0, 2, 0)
    local found = scene.find("Box")
    found_y = found.transform.position.y
    box.active = false
    inactive = not box.active
    enemies = #scene.find_all("Box")
    box.destroy()
    local again = scene.find("Box")
    destroyed = again == nil
    input_ok = input.is_action_down("Nothing") == false
    ok_time = type(time.delta()) == "number"
end
)");
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    CHECK_NEAR(w.Global<float>(o, "found_y"), 2.0f, 1e-5f);
    CHECK_TRUE(w.Global<bool>(o, "inactive"));
    CHECK_EQ(w.Global<int>(o, "enemies"), 1);
    CHECK_TRUE(w.Global<bool>(o, "destroyed"));
    CHECK_TRUE(w.Global<bool>(o, "input_ok"));
    CHECK_TRUE(w.Global<bool>(o, "ok_time"));
}

// --- Аннотации Inspector -----------------------------------------------------

namespace {
const sage::vars::Var* Find(const sage::vars::Table& t, const char* n) { return t.Find(n); }
}

// Каждая аннотация из задания, разбор текстом — без запуска файла.
TEST(LuaStandard_annotations_every_kind) {
    std::vector<sage::scripting::FieldDiagnostic> problems;
    const sage::vars::Table t = sage::scripting::lua::ParsePublicFields(R"(
---@header "Player Settings"

---@group "Movement"

---@export_range 0 20
---@tooltip "Movement speed of the player"
move_speed = 5.0

---@export
can_jump = true

---@end_group

---@group "Combat"
---@export_range 0 200
health = 100
---@export
player_name = "Player"
---@end_group

---@export_enum "Idle" "Walk" "Run"
animation_state = "Idle"

---@export_color
player_color = color(1, 0.5, 0)

---@export_object
target = nil

---@export_resource "Animation"
animator = nil

not_exported = 3
local hidden = 1
function update(delta) end
)", &problems);
    for (const auto& p : problems) std::printf("       %d: %s\n", p.Line, p.Message.c_str());
    CHECK_TRUE(problems.empty());
    CHECK_EQ(t.Size(), (size_t)8);
    CHECK_TRUE(Find(t, "not_exported") == nullptr);
    CHECK_TRUE(Find(t, "hidden") == nullptr);
    const auto* speed = Find(t, "move_speed");
    CHECK_TRUE(speed != nullptr);
    if (speed) {
        CHECK_EQ((int)speed->Data.Type(), (int)Kind::Float);
        CHECK_NEAR(speed->Max, 20.0f, 1e-6f);
        CHECK_EQ(speed->Tooltip, std::string("Movement speed of the player"));
        CHECK_EQ(speed->Group, std::string("Movement"));
        CHECK_EQ(speed->Header, std::string("Player Settings"));
    }
    const auto* health = Find(t, "health");
    if (health) {
        CHECK_EQ((int)health->Data.Type(), (int)Kind::Int);   // тип — по значению
        CHECK_NEAR(health->Max, 200.0f, 1e-6f);
        CHECK_EQ(health->Group, std::string("Combat"));
    }
    if (const auto* v = Find(t, "can_jump")) CHECK_EQ((int)v->Data.Type(), (int)Kind::Bool);
    if (const auto* v = Find(t, "player_name")) CHECK_EQ(v->Group, std::string("Combat"));
    const auto* state = Find(t, "animation_state");
    if (state) {
        CHECK_EQ(state->Options.size(), (size_t)3);
        CHECK_EQ(state->Group, std::string(""));            // группа закрыта
        CHECK_EQ(state->Header, std::string("Player Settings"));
    }
    if (const auto* v = Find(t, "player_color")) {
        CHECK_EQ((int)v->Data.Type(), (int)Kind::Color);
        CHECK_NEAR(v->Data.AsVec4().y, 0.5f, 1e-6f);
    }
    if (const auto* v = Find(t, "target")) CHECK_EQ((int)v->Data.Type(), (int)Kind::Entity);
    if (const auto* v = Find(t, "animator")) {
        CHECK_EQ((int)v->Data.Type(), (int)Kind::Asset);
        CHECK_EQ(v->Hint, std::string("Animation"));
    }
}

// Ошибки: переменная и аннотация связаны только прямо; мусор — понятное
// сообщение со строкой; вложенные группы — путь через «/».
TEST(LuaStandard_annotations_errors_and_binding) {
    std::vector<sage::scripting::FieldDiagnostic> problems;
    const sage::vars::Table t = sage::scripting::lua::ParsePublicFields(R"(
---@export_range 0 20

orphan_by_blank_line = 1

---@export_range 5 1
bad_range = 3

---@export_enum "A" "B"
not_a_string = 1

---@export_colour
typo_name = 1

---@export
from_nil = nil

---@export
sum = 1 + 2

---@export_resource
no_kind = nil

---@export
---@export_range 0 1
two_exports = 0.5

---@group "Outer"
---@group "Inner"
---@export
nested = 1
---@end_group
---@end_group
---@end_group

---@export
ok_one = 1

---@tooltip "alone"
just_tooltip = 2

local function helper() end
---@export
local local_var = 5
)", &problems);
    auto has = [&](const char* needle) {
        for (const auto& p : problems)
            if (p.Message.find(needle) != std::string::npos) return true;
        return false;
    };
    for (const auto& p : problems) std::printf("       %d: %s\n", p.Line, p.Message.c_str());
    CHECK_TRUE(has("не связана с переменной"));          // пустая строка оборвала связь
    CHECK_TRUE(has("минимум должен быть меньше"));
    CHECK_TRUE(has("значение должно быть строкой"));
    CHECK_TRUE(has("неизвестная аннотация @export_colour"));
    CHECK_TRUE(has("тип не определить по nil"));
    CHECK_TRUE(has("значение должно быть литералом"));
    CHECK_TRUE(has("нужен вид ресурса"));
    CHECK_TRUE(has("уже есть @export"));
    CHECK_TRUE(has("@end_group без открытой"));
    CHECK_TRUE(has("@tooltip без @export"));
    CHECK_TRUE(Find(t, "orphan_by_blank_line") == nullptr);
    CHECK_TRUE(Find(t, "just_tooltip") == nullptr);
    CHECK_TRUE(Find(t, "ok_one") != nullptr);
    if (const auto* v = Find(t, "nested")) CHECK_EQ(v->Group, std::string("Outer/Inner"));
    else CHECK_TRUE(false);
    if (const auto* v = Find(t, "ok_one")) CHECK_EQ(v->Group, std::string(""));
}

// Файл без аннотаций и без snake_case-функций — прежний вид: поля — все литералы.
TEST(LuaStandard_legacy_globals_still_fields) {
    const sage::vars::Table t = sage::scripting::ParseFields("legacy.lua", R"(
speed = 5.0
health = field.number(100, 0, 200)
function Update(dt) end
)");
    CHECK_TRUE(Find(t, "speed") != nullptr);
    CHECK_TRUE(Find(t, "health") != nullptr);
    // А новый вид без @export полей не даёт.
    const sage::vars::Table n = sage::scripting::ParseFields("new.lua", R"(
speed = 5.0
function update(delta) end
)");
    CHECK_EQ(n.Size(), (size_t)0);
}

// Поля работают и в игре: значение из Inspector ложится поверх присваивания,
// ссылка на объект приходит объектом, цвет — цветом.
TEST(LuaStandard_annotated_fields_reach_script) {
    World w;
    sage::vars::Table fields;
    {
        sage::vars::Var v;
        v.Name = "move_speed";
        v.Data = sage::vars::Value(9.0f);
        fields.Put(v);
    }
    GameObject o = w.Spawn("P", "annot", R"(
---@export_range 0 20
move_speed = 5.0
---@export_color
tint = color(1, 0, 0)
---@export_enum "Idle" "Walk"
state = "Idle"
---@export_object
target = nil
function start()
    speed_seen = move_speed
    tint_ok = tint ~= nil
    state_seen = state
    target_missing = target == nil
end
)", fields);
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    CHECK_NEAR(w.Global<float>(o, "speed_seen"), 9.0f, 1e-5f);
    CHECK_TRUE(w.Global<bool>(o, "tint_ok"));
    CHECK_EQ(w.Global<std::string>(o, "state_seen"), std::string("Idle"));
    CHECK_TRUE(w.Global<bool>(o, "target_missing"));
}
