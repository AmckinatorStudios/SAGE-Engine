// Простой вид Lua-скрипта: глобальные функции Start/Update(dt)/OnKeyDown(key),
// `self` — сам объект, глобальные переменные — поля инспектора, API нынешними
// именами (Input.is_key_down, Scene.find, Time.delta, Debug.log).
//
// Связка — та же, что у редактора и собранной игры: новый бэкенд поверх
// состояния прежнего ScriptEngine (MakeLuaBackend({engine})). Проверка на
// «чистом» бэкенде пропустила бы именно то, что ломается на стыке двух API.
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
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptFields.h"
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
        auto& sc = scene.Registry().emplace<ScriptComponent>(o.Entity());
        sc.Path = path;
        sc.Fields = fields;
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

// Пример из задания — дословно: Start, Update(dt), Vector3.zero(), ввод,
// сложение векторов и запись в self.transform.position.
TEST(LuaSimple_player_example_moves_with_keys) {
    World w;
    GameObject p = w.Spawn("Player", "player", R"(
speed = 5.0
jump_force = 8.0

function Start()
    Debug.log("Player started")
    started = true
end

function Update(dt)
    local direction = Vector3.zero()
    if Input.is_key_down("W") then
        direction.z = direction.z + 1
    end
    if Input.is_key_down("S") then
        direction.z = direction.z - 1
    end
    self.transform.position =
        self.transform.position + direction * speed * dt
    if Input.is_key_pressed("Space") then
        jumps = (jumps or 0) + 1
    end
end
)");
    w.Start();
    CHECK_TRUE(w.errors.empty());
    w.Frame();
    CHECK_TRUE(w.Global<bool>(p, "started"));
    CHECK_NEAR((*p.Registry()).get<Transform>(p.Entity()).Position.z, 0.0f, 1e-5f);

    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::W));
    for (int i = 0; i < 4; ++i) w.Frame(0.1f);
    CHECK_NEAR((*p.Registry()).get<Transform>(p.Entity()).Position.z, 2.0f, 1e-4f);   // 4 × 5 × 0.1

    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    w.Frame();
    w.Frame();   // «нажата» — только в первом кадре
    CHECK_EQ(w.Global<int>(p, "jumps"), 1);
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_TRUE(w.errors.empty());
}

// `transform.position.y = …` пишет в объект: копия, которая молча ничего не
// делает, — главная ловушка скриптовых API.
TEST(LuaSimple_transform_axis_assignment_writes_through) {
    World w;
    GameObject o = w.Spawn("Cube", "cube", R"(
function Update(dt)
    self.transform.position.y = self.transform.position.y + 1 * dt
    self.transform.rotation.y = self.transform.rotation.y + 90 * dt
    local p = self.transform.position
    distance = Vector3.distance(p, Vector3.new(0, 0, 0))   -- привязанный вектор — обычный Vector3
    doubled = (2 * p).y
end
)");
    (*o.Registry()).get<Transform>(o.Entity()).Position = glm::vec3(3.0f, 0.0f, 0.0f);
    w.Start();
    w.Frame(0.5f);
    w.Frame(0.5f);
    const Transform& t = (*o.Registry()).get<Transform>(o.Entity());
    CHECK_NEAR(t.Position.y, 1.0f, 1e-5f);
    CHECK_NEAR(t.Position.x, 3.0f, 1e-5f);   // соседние оси не тронуты
    CHECK_NEAR(t.Rotation.y, 90.0f, 1e-4f);
    CHECK_NEAR(w.Global<float>(o, "distance"), std::sqrt(9.0f + 1.0f), 1e-4f);
    CHECK_NEAR(w.Global<float>(o, "doubled"), 2.0f, 1e-4f);
    CHECK_TRUE(w.errors.empty());
}

// Глобальные переменные верхнего уровня — поля инспектора, тип по литералу.
TEST(LuaSimple_top_level_globals_become_inspector_fields) {
    const sage::vars::Table t = sage::scripting::ParseFields("x.lua", R"(
speed = 5.0          -- float
count = 10           -- integer
enabled = true       -- bool
name = "Player"      -- string
position = Vector3.new(0, 1, 0)
health = field.number(100, 0, 200)
local hidden = 1
_private = 2
sum = count + 1
for i = 1, 3 do end
function Update(dt)
    inner = 5
end
if enabled then after_if = 1 end
last = 'end'
)");
    auto kind = [&](const char* n) {
        const sage::vars::Var* v = t.Find(n);
        return v ? (int)v->Data.Type() : -1;
    };
    CHECK_EQ(kind("speed"), (int)Kind::Float);
    CHECK_EQ(kind("count"), (int)Kind::Int);
    CHECK_EQ(kind("enabled"), (int)Kind::Bool);
    CHECK_EQ(kind("name"), (int)Kind::String);
    CHECK_EQ(kind("position"), (int)Kind::Vec3);
    CHECK_EQ(kind("health"), (int)Kind::Float);
    CHECK_EQ(kind("last"), (int)Kind::String);
    if (const sage::vars::Var* v = t.Find("speed")) CHECK_NEAR(v->Data.AsFloat(), 5.0f, 1e-6f);
    if (const sage::vars::Var* v = t.Find("position")) CHECK_NEAR(v->Data.AsVec3().y, 1.0f, 1e-6f);
    if (const sage::vars::Var* v = t.Find("health")) CHECK_NEAR(v->Max, 200.0f, 1e-6f);
    for (const char* n : {"hidden", "_private", "sum", "i", "inner", "after_if"}) {
        if (t.Find(n)) std::printf("       лишнее поле: %s\n", n);
        CHECK_TRUE(t.Find(n) == nullptr);
    }
    // У скрипта-таблицы глобальных полей не бывает — только public.
    const sage::vars::Table cls = sage::scripting::ParseFields("y.lua", R"(
local P = {}
P.public = { speed = 3 }
temp = 1
return P
)");
    CHECK_TRUE(cls.Find("speed") != nullptr);
    CHECK_TRUE(cls.Find("temp") == nullptr);
}

// Значение из инспектора (сцены) сильнее присваивания в файле и видно уже в Start.
TEST(LuaSimple_inspector_value_overrides_file_default) {
    World w;
    sage::vars::Table fields;
    sage::vars::Var v;
    v.Name = "speed";
    v.Data = sage::vars::Value(12.0f);
    fields.Put(v);
    GameObject o = w.Spawn("Runner", "runner", R"(
speed = 5.0
function Start() seen = speed end
)", fields);
    w.Start();
    w.Frame();
    CHECK_NEAR(w.Global<float>(o, "seen"), 12.0f, 1e-6f);
}

// Жизненный цикл: Start один раз, Update и FixedUpdate с шагом, Destroy()
// перед удалением. self.name / self.game_object / self:get_component.
TEST(LuaSimple_lifecycle_and_self) {
    World w;
    GameObject o = w.Spawn("Lamp", "lamp", R"(
starts, updates, fixed = 0, 0, 0
function Start()
    starts = starts + 1
    my_name = self.name
    same = self.game_object == self.gameObject
    has_transform = self:get_component("Transform") ~= nil
    typed_nil = self.Camera == nil
end
function Update(dt) updates = updates + 1 end
function FixedUpdate(dt) fixed = fixed + 1; step = dt end
function Destroy() Events.emit("lamp_gone", self.name) end
)");
    GameObject watcher = w.Spawn("Watcher", "watcher", R"(
function Start() Events.on("lamp_gone", function(e, who) gone = who end) end
)");
    w.Start();
    w.Frame();
    w.Frame();
    w.sys->FixedUpdate(1.0f / 30.0f);
    CHECK_EQ(w.Global<int>(o, "starts"), 1);
    CHECK_EQ(w.Global<int>(o, "updates"), 2);
    CHECK_TRUE(w.Global<int>(o, "fixed") >= 1);
    CHECK_EQ(w.Global<std::string>(o, "my_name"), std::string("Lamp"));
    CHECK_TRUE(w.Global<bool>(o, "same"));
    CHECK_TRUE(w.Global<bool>(o, "has_transform"));
    CHECK_TRUE(w.Global<bool>(o, "typed_nil"));
    w.sys->Detach(o);
    CHECK_EQ(w.Global<std::string>(watcher, "gone"), std::string("Lamp"));
    CHECK_TRUE(w.errors.empty());
}

// События: OnKeyDown(key) сам, Events.emit("door_open") → OnDoorOpen(data) у
// всех простых скриптов, Events.on — подпиской. Отсутствующая функция события
// — не ошибка.
TEST(LuaSimple_events_call_functions_by_name) {
    World w;
    GameObject door = w.Spawn("Door", "door", R"(
opened = 0
function OnDoorOpen(data) opened = opened + 1; by = data end
function OnKeyDown(key) last_key = key end
function OnKeyUp(key) released = key end
)");
    GameObject lever = w.Spawn("Lever", "lever", R"(
function Start()
    Events.on("door_open", function() heard = true end)
end
function Pull() Events.emit("door_open", "lever") end
)");
    GameObject rock = w.Spawn("Rock", "rock", "function Update(dt) end\n");
    w.Start();
    w.Frame();
    CHECK_TRUE(w.sys->Runtime().Invoke(lever.Entity(), "Pull", {}));   // вызов Lua из C++
    CHECK_EQ(w.Global<int>(door, "opened"), 1);
    CHECK_EQ(w.Global<std::string>(door, "by"), std::string("lever"));
    CHECK_TRUE(w.Global<bool>(lever, "heard"));

    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::E));
    w.Frame();
    CHECK_EQ(w.Global<std::string>(door, "last_key"), std::string("E"));
    w.input.Push(sage::input::InputEvent::KeyUp(sage::input::Key::E));
    w.Frame();
    CHECK_EQ(w.Global<std::string>(door, "released"), std::string("E"));
    // Имя — как пишут в коде: «Space», «LeftShift», а не «SPACE».
    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    w.Frame();
    CHECK_EQ(w.Global<std::string>(door, "last_key"), std::string("Space"));
    w.input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::LeftShift));
    w.Frame();
    CHECK_EQ(w.Global<std::string>(door, "last_key"), std::string("LeftShift"));
    CHECK_TRUE(sage::input::ParseKey("LeftShift") == sage::input::Key::LeftShift);
    (void)rock;
    CHECK_TRUE(w.errors.empty());
}

// Столкновения и зоны: OnCollisionEnter(other) / OnTriggerEnter(other) —
// other — объект с именем.
TEST(LuaSimple_collision_and_trigger_get_the_other_object) {
    World w;
    GameObject trig = w.Spawn("Zone", "zone", R"(
function OnTriggerEnter(other) entered = other.name end
function OnCollisionEnter(other) hit = "Hit " .. other.name end
)");
    GameObject ball = w.scene.CreateObject("Ball");
    w.Start();
    w.sys->Runtime().DispatchTo(trig.Entity(), Hook::OnTriggerEnter, ball);
    w.sys->Runtime().DispatchTo(trig.Entity(), Hook::OnCollisionEnter, ball);
    CHECK_EQ(w.Global<std::string>(trig, "entered"), std::string("Ball"));
    CHECK_EQ(w.Global<std::string>(trig, "hit"), std::string("Hit Ball"));
    // У скрипта без этих функций событие — не ошибка.
    GameObject plain = w.Spawn("Plain", "plain", "x = 1\n");
    w.sys->Attach(plain);
    w.sys->Runtime().DispatchTo(plain.Entity(), Hook::OnCollisionEnter, ball);
    CHECK_TRUE(w.errors.empty());
}

// Scene.find / find_all и player:call("TakeDamage", 10) — возврат значения.
TEST(LuaSimple_find_and_call_other_scripts) {
    World w;
    GameObject player = w.Spawn("Player", "hp", R"(
health = 100
function TakeDamage(amount)
    health = health - amount
    return health
end
)");
    GameObject enemy1 = w.Spawn("Enemy", "enemy1", "function Update(dt) end\n");
    GameObject enemy2 = w.scene.CreateObject("Goblin");
    w.scene.Registry().emplace_or_replace<TagComponent>(enemy2.Entity()).Tag = "Enemy";
    GameObject boss = w.Spawn("Boss", "boss", R"(
function Start()
    local player = Scene.find("Player")
    if player then left = player:call("TakeDamage", 10) end
    enemies = #Scene.find_all("Enemy")
    missing = Scene.find("Nobody") == nil
    nothing = player:call("NoSuchMethod")
end
)");
    w.Start();
    w.Frame();
    for (const auto& e : w.errors) std::printf("       %s\n", e.Format().c_str());
    CHECK_EQ(w.Global<int>(player, "health"), 90);
    CHECK_EQ(w.Global<int>(boss, "left"), 90);
    CHECK_EQ(w.Global<int>(boss, "enemies"), 2);
    CHECK_TRUE(w.Global<bool>(boss, "missing"));
    (void)enemy1;
    CHECK_TRUE(w.errors.empty());
}

// Ошибка одного скрипта: файл и строка в сообщении, соседний скрипт работает.
TEST(LuaSimple_error_names_file_and_line_and_others_keep_running) {
    World w;
    GameObject bad = w.Spawn("Bad", "bad", R"(
function Update(dt)
    this_does_not_exist()
end
)");
    GameObject good = w.Spawn("Good", "good", "ticks = 0\nfunction Update(dt) ticks = ticks + 1 end\n");
    w.Start();
    w.Frame();
    w.Frame();
    CHECK_TRUE(!w.errors.empty());
    if (!w.errors.empty()) {
        const auto& e = w.errors.front();
        CHECK_TRUE(e.File.find("sage_simple_bad.lua") != std::string::npos);
        CHECK_EQ(e.Line, 3);
        CHECK_TRUE(e.Message.find("this_does_not_exist") != std::string::npos);
    }
    CHECK_EQ(w.Global<int>(good, "ticks"), 2);
    (void)bad;
}

// Два объекта с ОДНИМ файлом — у каждого свои глобальные; разные файлы не
// видят друг друга. Старый стиль (OnUpdate(entity, dt)) работает как раньше.
TEST(LuaSimple_instances_are_isolated_and_legacy_style_still_works) {
    World w;
    const std::string body = "count = 0\nfunction Update(dt) count = count + 1 end\n";
    GameObject a = w.Spawn("A", "shared", body);
    GameObject b = w.scene.CreateObject("B");
    w.scene.Registry().emplace<ScriptComponent>(b.Entity()).Path = w.files.back();
    GameObject legacy = w.Spawn("Old", "legacy", R"(
function OnUpdate(entity, dt)
    entity.Transform.Position.x = entity.Transform.Position.x + 1
end
)");
    w.Start();
    w.Frame();
    w.sys->Runtime().Invoke(a.Entity(), "Update", {sage::vars::Value(0.1f)});
    CHECK_EQ(w.Global<int>(a, "count"), 2);
    CHECK_EQ(w.Global<int>(b, "count"), 1);
    CHECK_NEAR((*legacy.Registry()).get<Transform>(legacy.Entity()).Position.x, 1.0f, 1e-6f);
    CHECK_TRUE(w.errors.empty());
}

// Vector3: new/zero()/one(), число с любой стороны, zero() — свой вектор каждый раз.
TEST(LuaSimple_vector3_basics) {
    World w;
    GameObject o = w.Spawn("Math", "math", R"(
function Start()
    local a = Vector3.zero()
    a.z = 1
    fresh = Vector3.zero().z == 0
    local d = Vector3.new(1, 2, 3)
    left = (2 * d).z
    right = (d * 2).z
    len = Vector3.new(3, 4, 0):length()
    unit = Vector3.new(0, 0, 5):normalized().z
end
)");
    w.Start();
    w.Frame();
    CHECK_TRUE(w.Global<bool>(o, "fresh"));
    CHECK_NEAR(w.Global<float>(o, "left"), 6.0f, 1e-6f);
    CHECK_NEAR(w.Global<float>(o, "right"), 6.0f, 1e-6f);
    CHECK_NEAR(w.Global<float>(o, "len"), 5.0f, 1e-6f);
    CHECK_NEAR(w.Global<float>(o, "unit"), 1.0f, 1e-6f);
    CHECK_TRUE(w.errors.empty());
}

// Правка файла на ходу: новая логика, значение из инспектора сохраняется.
TEST(LuaSimple_hot_reload_keeps_inspector_values) {
    World w;
    sage::vars::Table fields;
    sage::vars::Var v;
    v.Name = "speed";
    v.Data = sage::vars::Value(7.0f);
    fields.Put(v);
    GameObject o = w.Spawn("Hot", "hot", "speed = 1.0\nfunction Update(dt) seen = speed end\n", fields);
    w.Start();
    w.Frame();
    CHECK_NEAR(w.Global<float>(o, "seen"), 7.0f, 1e-6f);
    // Штамп времени правки обязан смениться: ждём и переписываем файл.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Write("hot", "speed = 1.0\nfunction Update(dt) seen = speed * 2 end\n");
    CHECK_EQ(w.sys->ReloadChanged(), 1);
    w.Frame();
    CHECK_NEAR(w.Global<float>(o, "seen"), 14.0f, 1e-6f);
    CHECK_TRUE(w.errors.empty());
}
