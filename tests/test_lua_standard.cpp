// Стандарт Lua API SAGE: `object`, функции жизненного цикла в snake_case,
// `events.create/trigger/connect/connect_once/disconnect`, аннотации `---@`.
// Связка — та же, что у редактора (бэкенд поверх прежнего ScriptEngine).
#include "TestFramework.h"

#include <algorithm>
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
#include "sage/scene/Signals.h"
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

// Инспектор находит события по тексту, до запуска: events.create(object, "…").
TEST(LuaStandard_events_create_discovered_statically) {
    const std::string src = R"(
events.create(object, "player_died")
events.create(scene.find("Door"), 'opened')
events.create("global_only")
local function die()
    events.trigger(object, "player_died", {reason = "fall"})
    events.trigger("level_loaded", "forest")
end
object.define_event("legacy_one")
)";
    const std::vector<std::string> defined = sage::scripting::ParseDefinedEvents(src);
    auto has = [&](const std::vector<std::string>& v, const char* n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    CHECK_TRUE(has(defined, "player_died"));
    CHECK_TRUE(has(defined, "opened"));
    CHECK_FALSE(has(defined, "global_only"));   // глобальное событие — не сигнал объекта
    const std::vector<std::string> emitted = sage::scripting::ParseEmittedSignals(src);
    CHECK_TRUE(has(emitted, "player_died"));
    CHECK_FALSE(has(emitted, "forest"));
}

// Встроенные события — теми же events.connect: тот же сигнал, что слушают
// связи Inspector, и событие несёт те же поля.
TEST(LuaStandard_builtin_events_use_events_connect) {
    World w;
    GameObject b = w.Spawn("Button", "btn", R"(
clicks = 0
events.connect(object, "clicked", function(event)
    clicks = clicks + 1
    who = event.sender.name
    ev = event.name
end)
events.connect_once(object, "value_changed", function(event) value = event.data end)
)");
    w.Start();
    w.scene.Events.EmitSignal(b.Id(), sage::signals::kClicked, sage::vars::Value());
    w.scene.Events.EmitSignal(b.Id(), sage::signals::kClicked, sage::vars::Value());
    w.scene.Events.EmitSignal(b.Id(), sage::signals::kValueChanged, sage::vars::Value(0.5f));
    w.scene.Events.EmitSignal(b.Id(), sage::signals::kValueChanged, sage::vars::Value(0.9f));
    CHECK_EQ(w.Global<int>(b, "clicks"), 2);
    CHECK_EQ(w.Global<std::string>(b, "who"), std::string("Button"));
    CHECK_EQ(w.Global<std::string>(b, "ev"), std::string("clicked"));
    CHECK_NEAR(w.Global<float>(b, "value"), 0.5f, 1e-5f);   // connect_once: только первое
    CHECK_TRUE(w.errors.empty());
}

// Ввод по именованным действиям: is_action_down, was_pressed, was_released,
// get_vector, get_axis.
TEST(LuaStandard_input_actions) {
    World w;
    w.input.Register("Jump");
    w.input.AddBinding("Jump", "SPACE");
    GameObject o = w.Spawn("P", "inp", R"(
pressed, released, down = 0, 0, 0
function update(delta)
    if input.was_pressed("Jump") then pressed = pressed + 1 end
    if input.was_released("Jump") then released = released + 1 end
    if input.is_action_down("Jump") then down = down + 1 end
    local v = input.get_vector("Move")
    vec_ok = v ~= nil
    axis_ok = type(input.get_axis("Turn")) == "number"
end
)");
    w.Start();
    // Кадр с действиями: как в игре, действия обновляются после опроса ввода.
    auto frame = [&] {
        w.input.BeginFrame();
        w.input.UpdateActions(0.1f);
        w.sys->Update(0.1f);
    };
    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    frame();
    frame();
    w.input.Push(sage::input::InputEvent::KeyUp(sage::input::Key::Space));
    frame();
    CHECK_EQ(w.Global<int>(o, "pressed"), 1);
    CHECK_EQ(w.Global<int>(o, "released"), 1);
    CHECK_EQ(w.Global<int>(o, "down"), 2);
    CHECK_TRUE(w.Global<bool>(o, "vec_ok"));
    CHECK_TRUE(w.Global<bool>(o, "axis_ok"));
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
}

// Строчные разделы: все они на месте и ведут туда же, куда прежние.
TEST(LuaStandard_lowercase_modules_present) {
    World w;
    GameObject o = w.Spawn("P", "mods", R"(
function start()
    local checks = {
        input.is_key_down, input.is_action_down, time.delta, debug.log, debug.warn, debug.error,
        scene.find, scene.find_all, scene.create, physics.raycast, audio.play, tween.to,
        ui.find, ui.get, events.create, events.trigger, events.connect, events.connect_once,
        events.disconnect, vector3.new, vector3.zero, color,
    }
    missing = {}
    for i = 1, 22 do if checks[i] == nil then missing[#missing + 1] = i end end
    missing_count = #missing
    c = color(1, 0.5, 0.25)
    color_ok = c ~= nil
    same_events = events.connect == Events.connect
end
)");
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    CHECK_EQ(w.Global<int>(o, "missing_count"), 0);
    CHECK_TRUE(w.Global<bool>(o, "color_ok"));
    CHECK_TRUE(w.Global<bool>(o, "same_events"));
}

// Совместимость: старый скрипт (self, двоеточия, Start/Update, Events.emit/on,
// field.*) работает рядом со стандартным.
TEST(LuaStandard_legacy_script_still_works) {
    World w;
    sage::vars::Table fields;
    GameObject o = w.Spawn("Old", "old", R"(
health = field.number(100, 0, 200)
speed = 3.0
function Start()
    self:add_component("Camera")
    cam = self:has_component("Camera")
    Events.on("old_event", function(event, data) got = data end)
    self.transform.position = Vector3.new(1, 2, 3)
end
function Update(dt)
    self.transform:Translate(Vector3.new(0, 0, speed * dt))
    ticked = true
end
function OnOldEvent(data) viahook = data end
)");
    w.Start();
    w.Frame(1.0f);
    w.engine.Lua().script("Events.emit('old_event', 'hi')");
    CHECK_TRUE(w.Global<bool>(o, "cam"));
    CHECK_TRUE(w.Global<bool>(o, "ticked"));
    CHECK_EQ(w.Global<std::string>(o, "got"), std::string("hi"));
    CHECK_EQ(w.Global<std::string>(o, "viahook"), std::string("hi"));
    CHECK_NEAR((*o.Registry()).get<Transform>(o.Entity()).Position.z, 6.0f, 1e-4f);
    CHECK_TRUE(w.errors.empty());
}

// Методы прокси через точку: твин, камера, преобразование.
TEST(LuaStandard_dot_calls_on_proxies) {
    World w;
    GameObject o = w.Spawn("Cam", "proxies", R"(
function start()
    local cam = object.add_component("camera", {fov = 70})
    fov_before = cam.fov
    cam.fov = 55
    fov_after = object.get_component("camera").fov
    object.transform.look_at(vector3.new(0, 0, -10))
    local t = tween.to(object, "scale", 2.0, 0.5)
    t.ping_pong()
    chained = tostring(t.is_playing()) == "true"
    t.cancel()
    local old = object:has_component("camera")   -- двоеточие по-прежнему работает
    colon_ok = old
    local right = object.transform.right()
    right_x = right.x
end
)");
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    CHECK_NEAR(w.Global<float>(o, "fov_before"), 70.0f, 1e-4f);
    CHECK_NEAR(w.Global<float>(o, "fov_after"), 55.0f, 1e-4f);
    CHECK_TRUE(w.Global<bool>(o, "chained"));
    CHECK_TRUE(w.Global<bool>(o, "colon_ok"));
    CHECK_NEAR(w.Global<float>(o, "right_x"), 1.0f, 1e-3f);
}

// Каждое имя из docs/scripting.md («Стандарт SAGE Lua») существует.
TEST(LuaStandard_documented_names_exist) {
    World w;
    GameObject o = w.Spawn("P", "docnames", R"(
function start()
    local names = {
        "scene.find", "scene.find_all", "scene.create", "scene.instantiate", "scene.load",
        "events.create", "events.trigger", "events.connect", "events.connect_once", "events.disconnect",
        "input.is_action_down", "input.was_pressed", "input.was_released", "input.get_vector",
        "input.get_axis", "input.is_key_down", "input.is_key_pressed", "input.is_mouse_down",
        "input.mouse_position", "input.mouse_delta", "input.bind_action",
        "time.delta", "time.fixed_delta", "time.total", "time.set_scale",
        "debug.log", "debug.warn", "debug.error", "debug.draw_line", "debug.draw_sphere",
        "audio.play", "audio.play_at", "physics.raycast", "physics.overlap_sphere",
        "ui.get", "ui.find", "vector3.new", "vector3.zero", "vector3.one", "vector3.up", "vector3.forward",
        "vector3.distance", "vector3.dot", "vector3.cross", "vector3.lerp",
        "tween.to", "tween.from", "tween.sequence", "tween.play", "tween.cancel", "tween.pause",
        "tween.resume", "tween.is_playing",
    }
    missing = ""
    for _, path in ipairs(names) do
        local value = _G
        for part in path:gmatch("[^.]+") do
            if type(value) ~= "table" then value = nil break end
            value = value[part]
        end
        if value == nil then missing = missing .. path .. " " end
    end
    local o = object
    for _, m in ipairs({"destroy", "get_component", "has_component", "add_component", "remove_component", "call"}) do
        if o[m] == nil then missing = missing .. "object." .. m .. " " end
    end
    for _, m in ipairs({"forward", "right", "up", "look_at", "translate", "rotate"}) do
        if o.transform[m] == nil then missing = missing .. "transform." .. m .. " " end
    end
end
)");
    w.Start();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
    const std::string missing = w.Global<std::string>(o, "missing");
    if (!missing.empty()) std::printf("       нет имён: %s\n", missing.c_str());
    CHECK_EQ(missing, std::string(""));
}
