// SAGE Lua: компилятор (лексер, парсер, типы, аннотации, генератор) и
// runtime (классы, события, задачи, Result) — на примерах из спецификации
// языка. Каждый пример не только собирается, но и ИСПОЛНЯЕТСЯ: сгенерированный
// Lua запускается вместе с прелюдией, и проверяется результат, а не текст.
#include "TestFramework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "sage/lang/SageCompiler.h"

using namespace sage::lang;

namespace {

// Состояние Lua с прелюдией SAGE и заглушкой Debug, собирающей журнал.
struct Lua {
    sol::state L;
    std::vector<std::string> logs;
    std::vector<std::string> errors;

    Lua() {
        L.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                         sol::lib::coroutine, sol::lib::debug);
        sol::table dbg = L.create_table();
        dbg.set_function("Log", [this](const std::string& s) { logs.push_back(s); });
        dbg.set_function("Warning", [this](const std::string& s) { logs.push_back("W:" + s); });
        dbg.set_function("Error", [this](const std::string& s) { errors.push_back(s); });
        L["Debug"] = dbg;
        sol::protected_function_result r = L.safe_script(PreludeSource(), sol::script_pass_on_error);
        if (!r.valid()) {
            sol::error e = r;
            std::printf("       прелюдия: %s\n", e.what());
        }
    }

    // Компилирует и исполняет файл; возвращает то, что файл вернул.
    sol::object Run(const std::string& path, const std::string& src, bool expectOk = true) {
        CompileResult c = Compile(path, src);
        if (!c.Ok) {
            if (expectOk)
                for (const Diagnostic& d : c.Diagnostics) std::printf("       %s\n", d.Format().c_str());
            return sol::lua_nil;
        }
        sol::load_result chunk = L.load(c.Lua, "@" + path);
        if (!chunk.valid()) {
            sol::error e = chunk;
            std::printf("       Lua не собрался: %s\n--- код ---\n%s\n", e.what(), c.Lua.c_str());
            return sol::lua_nil;
        }
        sol::protected_function fn = chunk;
        sol::protected_function_result r = fn();
        if (!r.valid()) {
            sol::error e = r;
            lastError = e.what();
            if (expectOk) std::printf("       ошибка исполнения: %s\n--- код ---\n%s\n", e.what(), c.Lua.c_str());
            return sol::lua_nil;
        }
        if (r.return_count() == 0) return sol::lua_nil;
        return r.get<sol::object>();
    }
    std::string lastError;

    void Tick(float dt, int frames = 1) {
        sol::protected_function tick = L["__sage"]["tick"];
        for (int k = 0; k < frames; ++k) {
            sol::protected_function_result r = tick(dt);
            if (!r.valid()) {
                sol::error e = r;
                std::printf("       tick: %s\n", e.what());
            }
        }
    }
    template <typename T>
    T Get(const char* expr) {
        return L.script(std::string("return ") + expr).get<T>();
    }
};

bool HasError(const CompileResult& r, const std::string& fragment) {
    for (const Diagnostic& d : r.Diagnostics)
        if (d.IsError() && d.Format().find(fragment) != std::string::npos) return true;
    return false;
}
bool HasWarning(const CompileResult& r, const std::string& fragment) {
    for (const Diagnostic& d : r.Diagnostics)
        if (!d.IsError() && d.Format().find(fragment) != std::string::npos) return true;
    return false;
}
void Dump(const CompileResult& r) {
    for (const Diagnostic& d : r.Diagnostics) std::printf("       %s\n", d.Format().c_str());
}

} // namespace

// --- 2. Переменные, константы, типы ----------------------------------------------
TEST(SageLua_variables_constants_and_types) {
    Lua lua;
    sol::object r = lua.Run("vars.slua", R"(
local playerName: string = "Hero"
local health: number = 100
local speed: number = 5.5
local isAlive: boolean = true
local score := 0
local title := "My Game"
const MAX_HEALTH: number = 100
const GAME_TITLE: string = "SAGE Adventure"
var coins: integer = 0
local target: Entity? = nil
score = score + 10
coins = coins + 1
return playerName .. ":" .. health .. ":" .. score .. ":" .. coins .. ":" .. MAX_HEALTH
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "Hero:100:10:1:100");

    // Ошибки: тип, константа, целое.
    CompileResult bad = Compile("bad.slua", R"(
local speed: number = "fast"
const MAX: number = 1
MAX = 2
local c: integer = 5.5
local t := nil
)");
    CHECK_FALSE(bad.Ok);
    CHECK_TRUE(HasError(bad, "bad.slua:2:23"));
    CHECK_TRUE(HasError(bad, "Ожидалось значение number."));
    CHECK_TRUE(HasError(bad, "Получено значение string."));
    CHECK_TRUE(HasError(bad, "константу MAX нельзя переприсвоить"));
    CHECK_TRUE(HasError(bad, "Ожидалось значение integer."));
    CHECK_TRUE(HasError(bad, "нельзя вывести из nil"));

    // Обычный Lua без типов — без проверок: x меняет тип.
    CompileResult plain = Compile("plain.slua", "local x = 5\nx = 'text'\nreturn x\n");
    CHECK_TRUE(plain.Ok);
    // Выведенный тип держится: score := 0, потом строка — ошибка.
    CompileResult inferred = Compile("inf.slua", "local score := 0\nscore = 'many'\n");
    CHECK_TRUE(HasError(inferred, "Ожидалось значение number."));
}

// --- 3. Функции ---------------------------------------------------------------------
TEST(SageLua_functions_defaults_arrows_varargs_multiple_returns) {
    Lua lua;
    sol::object r = lua.Run("fn.slua", R"(
function add(a: number, b: number): number
    return a + b
end
function greet(name: string = "Player"): string
    return "Hello, " .. name
end
local square = (x: number): number => x * x
function getPosition(): (number, number)
    return 10, 20
end
function sum(...values: number[]): number
    local total = 0
    for _, value in ipairs(values) do
        total = total + value
    end
    return total
end
local x, y = getPosition()
return add(2, 3) .. "|" .. greet() .. "|" .. greet("Ann") .. "|" .. square(4) .. "|" .. (x + y) .. "|" .. sum(1, 2, 3, 4)
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "5|Hello, Player|Hello, Ann|16|30|10");

    CompileResult bad = Compile("fnbad.slua", R"(
function add(a: number, b: number): number
    return a + b
end
add("1", 2)
add(1)
add(1, 2, 3)
function nothing(): void
    return 5
end
function needs(): number
    return
end
function sum(...values: number[]): number
    return 0
end
sum(1, "two")
local f = (x: number): string => x * 2
)");
    CHECK_TRUE(HasError(bad, "Аргумент a у add()"));
    CHECK_TRUE(HasError(bad, "не хватает аргумента b"));
    CHECK_TRUE(HasError(bad, "принимает 2 аргумент(ов), а передано 3"));
    CHECK_TRUE(HasError(bad, "объявлена : void, а возвращает значение"));
    CHECK_TRUE(HasError(bad, "обязана вернуть number"));
    CHECK_TRUE(HasError(bad, "Аргумент ... у sum()"));
    CHECK_TRUE(HasError(bad, "Ожидалось значение string."));
}

// --- 4. Классы -------------------------------------------------------------------
TEST(SageLua_classes_inheritance_super_and_access) {
    Lua lua;
    sol::object r = lua.Run("classes.slua", R"(
class Entity
    public name: string = "Entity"
    public active: boolean = true
    public updated: number = 0
    function init(name: string)
        self.name = name
    end
    function update(dt: number): void
        self.updated = self.updated + 1
    end
    function destroy(): void
        self.active = false
    end
end

class Player extends Entity
    public health: number = 100
    public speed: number = 5
    override function update(dt: number): void
        super.update(dt)
        if self.health <= 0 then
            self:die()
        end
    end
    function takeDamage(amount: number): void
        self.health = math.max(0, self.health - amount)
    end
    private function die(): void
        self.active = false
    end
end

local player = Player("Hero")
player:takeDamage(25)
player:update(0.016)
local a = player.health
player:takeDamage(100)
player:update(0.016)
return player.name .. ":" .. a .. ":" .. tostring(player.active) .. ":" .. player.updated .. ":" .. tostring(player:is(Entity))
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "Hero:75:false:2:true");

    CompileResult bad = Compile("cls.slua", R"(
class A
    public x: number = 1
    private secret: number = 2
    protected guarded: number = 3
    function act(): void end
    final function locked(): void end
    private function hidden(): void end
end
class B extends A
    function act(): void end
    override function missing(): void end
    override function locked(): void end
    function read(): number
        return self.guarded + self.secret
    end
end
local a = A()
a.secret = 5
a:hidden()
print(a.guarded)
a.nope = 1
a:alsoNope()
local s: string = a.x
)");
    CHECK_TRUE(HasError(bad, "переопределяет A.act — напишите override"));
    CHECK_TRUE(HasError(bad, "override у метода B.missing, но у родителя такого метода нет"));
    CHECK_TRUE(HasError(bad, "A.locked объявлен final"));
    CHECK_TRUE(HasError(bad, "поле A.secret закрыто (private)"));
    CHECK_TRUE(HasError(bad, "метод A.hidden закрыто (private)"));
    CHECK_TRUE(HasError(bad, "поле A.guarded защищено (protected)"));
    CHECK_TRUE(HasError(bad, "у типа A нет поля 'nope'"));
    CHECK_TRUE(HasError(bad, "у типа A нет метода 'alsoNope'"));
    CHECK_TRUE(HasError(bad, "Поле: A.x") || HasError(bad, "Переменная: s"));
    // protected доступно наследнику: self.guarded в B — без ошибки про guarded.
    int guardedErrors = 0;
    for (const Diagnostic& d : bad.Diagnostics)
        if (d.Format().find("guarded") != std::string::npos) ++guardedErrors;
    CHECK_EQ(guardedErrors, 1);
}

TEST(SageLua_static_members_and_constructor_checks) {
    Lua lua;
    sol::object r = lua.Run("static.slua", R"(
class Counter
    static count: integer = 0
    public id: integer = 0
    function init()
        Counter.count = Counter.count + 1
        self.id = Counter.count
    end
    static function make(): Counter
        return Counter()
    end
end
Counter.make()
local c = Counter.make()
return Counter.count * 10 + c.id
)");
    CHECK_TRUE(r.is<int>() && r.as<int>() == 22);
    CompileResult bad = Compile("ctor.slua", R"(
class P
    function init(name: string) end
end
local p = P(5)
local q = P()
class Q end
local w = Q(1)
)");
    CHECK_TRUE(HasError(bad, "Аргумент name у P()"));
    CHECK_TRUE(HasError(bad, "у P() не хватает аргумента name"));
    CHECK_TRUE(HasError(bad, "у класса Q нет конструктора init"));
}

// --- 5. Аннотации ------------------------------------------------------------------
TEST(SageLua_annotations_bind_to_the_next_declaration_and_are_validated) {
    const std::string src = R"(
@serializable
class PlayerController extends Component
    @field(
        editable = true,
        name = "Movement Speed",
        tooltip = "Movement speed of the player",
        range = { min = 0, max = 20, step = 0.5 }
    )
    public speed: number = 5
    @field(
        editable = true,
        name = "Health",
        tooltip = "Current player health",
        range = { min = 0, max = 100 }
    )
    public health: number = 100
    @field(
        editable = true,
        name = "Follow Camera",
        tooltip = "Enable automatic camera following"
    )
    public followCamera: boolean = true
    @field(
        editable = true,
        name = "Score",
        replicated = true
    )
    public score: integer = 0
    public hiddenValue: number = 3
    @method(
        deprecated = "Use moveTo() instead"
    )
    function oldMove(): void
        self:moveTo(0, 0)
    end
    function moveTo(x: number, y: number): void end
    function onUpdate(dt: number): void
        self:oldMove()
    end
end
)";
    CompileResult r = Compile("Scripts/PlayerController.slua", src);
    Dump(r);
    CHECK_TRUE(r.Ok);
    CHECK_TRUE(r.MainClass == "PlayerController");
    const ClassInfo* c = r.Program.Find("PlayerController");
    CHECK_TRUE(c && c->Serializable && c->IsComponent());
    if (!c) return;
    const FieldInfo* speed = c->OwnField("speed");
    CHECK_TRUE(speed && speed->Meta.Editable && speed->Meta.Label == "Movement Speed" && speed->Meta.HasRange);
    CHECK_NEAR(speed->Meta.Step, 0.5, 1e-9);
    // Аннотация не расползается на соседей.
    CHECK_FALSE(c->OwnField("hiddenValue")->Meta.Editable);
    CHECK_TRUE(c->OwnField("score")->Meta.Replicated);
    CHECK_FALSE(c->OwnField("health")->Meta.Replicated);
    CHECK_TRUE(c->OwnMethod("oldMove")->Deprecated == "Use moveTo() instead");
    CHECK_TRUE(HasWarning(r, "Use moveTo() instead"));   // вызов устаревшего метода

    // Инспектор: только editable-поля, с подписями и границами.
    sage::vars::Table fields = FieldsOf(r);
    CHECK_EQ((int)fields.Size(), 4);
    const sage::vars::Var* v = fields.Find("speed");
    CHECK_TRUE(v && v->Label == "Movement Speed" && v->Tooltip == "Movement speed of the player");
    CHECK_NEAR(v ? v->Max : 0.0f, 20.0f, 1e-6f);
    CHECK_TRUE(v && v->Data.Type() == sage::vars::Kind::Float);
    CHECK_TRUE(fields.Find("score")->Data.Type() == sage::vars::Kind::Int);
    CHECK_TRUE(fields.Find("followCamera")->Data.Type() == sage::vars::Kind::Bool);
    CHECK_TRUE(fields.Find("hiddenValue") == nullptr);
    // Описание для редактора.
    const std::string desc = r.DescribeClasses();
    CHECK_TRUE(desc.find("field public speed: number editable name=\"Movement Speed\" range=[0, 20, 0.5]") != std::string::npos);

    CompileResult bad = Compile("ann.slua", R"(
class X extends Component
    @field(editable = true, range = { min = 10, max = 0 })
    public a: number = 5
    @field(editable = true, range = { min = 0, max = 10 })
    public b: number = 50
    @field(editable = true, range = { min = 0, max = 1 })
    public c: string = "x"
    @field(editbale = true)
    public d: number = 1
    @field(editable = "yes")
    public e: number = 1
    @method(deprecated = "x")
    public f: number = 1
    @unknown
    function g(): void end
    @field(editable = true)
    public h: table = {}
end
)");
    CHECK_TRUE(HasError(bad, "min (10) больше max (0)"));
    CHECK_TRUE(HasError(bad, "значение по умолчанию 50 вне диапазона [0, 10]"));
    CHECK_TRUE(HasError(bad, "range применим к числовому полю"));
    CHECK_TRUE(HasError(bad, "у @field нет параметра 'editbale'"));
    CHECK_TRUE(HasError(bad, "@field(editable) ждёт true или false"));
    CHECK_TRUE(HasError(bad, "@method неприменима к полю"));
    CHECK_TRUE(HasError(bad, "неизвестная аннотация @unknown"));
    CHECK_TRUE(HasError(bad, "тип table не показывается в инспекторе"));
}

TEST(SageLua_annotation_before_a_non_declaration_is_a_syntax_error) {
    CompileResult bad = Compile("misplaced.slua", "@field(editable = true)\nlocal z = 1\n");
    CHECK_FALSE(bad.Ok);
    CHECK_TRUE(HasError(bad, "аннотация должна стоять перед объявлением"));
}

// --- 6. Интерфейсы, перечисления, структуры, коллекции ------------------------------------
TEST(SageLua_interfaces_enums_structs_and_collections) {
    Lua lua;
    sol::object r = lua.Run("data.slua", R"(
interface IDamageable
    function takeDamage(amount: number): void
end
class Enemy implements IDamageable
    public health: number = 50
    function takeDamage(amount: number): void
        self.health = math.max(0, self.health - amount)
    end
end
enum GameState
    Loading,
    MainMenu,
    Playing,
    Paused,
    GameOver
end
struct Vector2D
    x: number = 0
    y: number = 0
end
local state: GameState = GameState.MainMenu
local e = Enemy()
e:takeDamage(20)
local position = Vector2D(10, 20)
position.x = position.x + 5
local copy = position:copy()
copy.y = 0
local inventory: string[] = { "Sword", "Potion", "Key" }
local stats: map<string, number> = { damage = 25, armor = 10 }
local d: IDamageable = e
return e.health .. "|" .. state .. "|" .. GameState.name(state) .. "|" .. position.x .. "," .. position.y ..
       "|" .. copy.y .. "|" .. #inventory .. "|" .. stats.damage .. "|" .. tostring(e:is(IDamageable)) ..
       "|" .. tostring(position == Vector2D(15, 20))
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "30|1|MainMenu|15,20|0|3|25|true|true");
    // Несуществующее значение перечисления на runtime — ошибка, а не nil.
    sol::object ok = lua.Run("enum2.slua", "enum E\n A\nend\nlocal key = 'B'\nlocal ok = pcall(function() return E[key] end)\nreturn ok", true);
    CHECK_TRUE(ok.is<bool>() && !ok.as<bool>());

    CompileResult bad = Compile("data_bad.slua", R"(
interface IDamageable
    function takeDamage(amount: number): void
end
class Wall implements IDamageable
end
class Enemy implements IDamageable
    function takeDamage(amount: string): void end
end
enum GameState
    Loading,
    Playing
end
local s = GameState.Plaing
struct V
    x: number = 0
end
local v = V(1, 2)
local inventory: string[] = { "Sword", 5 }
local stats: map<string, number> = { damage = "high" }
local i = IDamageable()
)");
    CHECK_TRUE(HasError(bad, "класс Wall не реализует IDamageable.takeDamage"));
    CHECK_TRUE(HasError(bad, "несовместима с IDamageable.takeDamage"));
    CHECK_TRUE(HasError(bad, "в перечислении GameState нет значения 'Plaing'"));
    CHECK_TRUE(HasError(bad, "у структуры V 1 поля, а передано 2"));
    CHECK_TRUE(HasError(bad, "Ожидалось значение string.") && HasError(bad, "Получено значение integer."));
    CHECK_TRUE(HasError(bad, "Ожидалось значение number.") && HasError(bad, "Получено значение string."));
    CHECK_TRUE(HasError(bad, "интерфейс IDamageable нельзя создать"));
}

// --- 8. События --------------------------------------------------------------------
TEST(SageLua_typed_events_on_once_emit_and_unsubscribe) {
    Lua lua;
    sol::object r = lua.Run("events.slua", R"(
class Player
    public name: string = "Hero"
end
event PlayerDied(player: Player)
event ScoreChanged(newScore: integer)
local log = {}
local h = Events.on(PlayerDied, function(player)
    log[#log + 1] = player.name .. " died"
end)
Events.on(ScoreChanged, function(score) log[#log + 1] = "score " .. score end)
Events.once("GameStarted", function() log[#log + 1] = "started" end)
Events.emit("GameStarted")
Events.emit("GameStarted")
local player = Player()
Events.emit(PlayerDied, player)
Events.emit(ScoreChanged, 100)
h:disconnect()
Events.emit(PlayerDied, player)
-- Ошибка в обработчике не валит отправку остальным.
Events.on("Boom", function() error("handler failed") end)
Events.on("Boom", function() log[#log + 1] = "second handler ran" end)
Events.emit("Boom")
return table.concat(log, ";")
)");
    CHECK_TRUE(r.is<std::string>() &&
               r.as<std::string>() == "started;Hero died;score 100;second handler ran");
    CHECK_EQ((int)lua.errors.size(), 1);

    CompileResult bad = Compile("ev.slua", R"(
event PlayerDied(player: string)
event ScoreChanged(newScore: integer)
Events.emit(PlayerDied)
Events.emit(ScoreChanged, "high")
Events.on(ScoreChanged, function(a, b) end)
)");
    CHECK_TRUE(HasError(bad, "событие PlayerDied объявлено с 1 параметр(ами), а отправлено 0"));
    CHECK_TRUE(HasError(bad, "Аргумент newScore события ScoreChanged"));
    CHECK_TRUE(HasError(bad, "обработчик ждёт 2 параметр(ов), а событие ScoreChanged даёт 1"));
}

// --- 10. Асинхронность, таймеры, корутины -------------------------------------------------
TEST(SageLua_async_await_timers_and_coroutines) {
    Lua lua;
    lua.Run("async.slua", R"(
log = {}
async function spawnEnemy(): void
    log[#log + 1] = "wait"
    await Time.wait(2.0)
    log[#log + 1] = "spawned"
    await Time.nextFrame()
    log[#log + 1] = "next frame"
end
async function loadValue(): number
    await Time.wait(0.5)
    return 42
end
async function chain(): void
    local v = await loadValue()
    log[#log + 1] = "got " .. v
end
task = spawnEnemy()
chain()
Timer.after(3.0, function() log[#log + 1] = "three seconds" end)
ticks = 0
every = Timer.every(1.0, function() ticks = ticks + 1 end)
local routine = coroutine.create(function()
    log[#log + 1] = "co started"
    coroutine.yield()
    log[#log + 1] = "co resumed"
end)
coroutine.resume(routine)
coroutine.resume(routine)
)");
    CHECK_TRUE(lua.Get<std::string>("table.concat(log, ';')") == "wait;co started;co resumed");
    lua.Tick(0.25, 2);   // 0.5 с
    CHECK_TRUE(lua.Get<std::string>("table.concat(log, ';')") == "wait;co started;co resumed;got 42");
    lua.Tick(0.5, 3);    // 2.0 с
    CHECK_TRUE(lua.Get<std::string>("table.concat(log, ';')").find("spawned") != std::string::npos);
    CHECK_FALSE(lua.Get<bool>("task:isDone()"));
    lua.Tick(0.016, 1);
    CHECK_TRUE(lua.Get<bool>("task:isDone()"));
    lua.Tick(0.5, 2);    // 3.0 с
    CHECK_TRUE(lua.Get<std::string>("table.concat(log, ';')").find("three seconds") != std::string::npos);
    CHECK_EQ(lua.Get<int>("ticks"), 3);
    lua.L.script("every:cancel()");
    lua.Tick(1.0, 3);
    CHECK_EQ(lua.Get<int>("ticks"), 3);

    // Ошибка в задаче — в журнал, остальное идёт.
    lua.Run("asyncerr.slua", R"(
async function broken(): void
    await Time.wait(0.1)
    error("task failed")
end
t2 = broken()
)");
    lua.Tick(0.2);
    CHECK_TRUE(lua.Get<bool>("t2:isDone()"));
    CHECK_FALSE(lua.errors.empty());

    // await вне async — ошибка компиляции.
    CompileResult bad = Compile("aw.slua", "function f()\n    await Time.wait(1)\nend\n");
    CHECK_TRUE(HasError(bad, "await вне async"));
}

// --- 14. Result -------------------------------------------------------------------
TEST(SageLua_result_type) {
    Lua lua;
    sol::object r = lua.Run("result.slua", R"(
function loadLevel(name: string): Result<string, string>
    if name == "" then
        return Result.err("Level name cannot be empty")
    end
    return Result.ok("scene:" .. name)
end
local a = loadLevel("Levels/Forest")
local b = loadLevel("")
local out = {}
if a:isOk() then out[#out + 1] = a:value() end
if b:isErr() then out[#out + 1] = b:error() end
out[#out + 1] = b:unwrapOr("fallback")
return table.concat(out, "|")
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "scene:Levels/Forest|Level name cannot be empty|fallback");
    CompileResult bad = Compile("rbad.slua", R"(
function f(): Result<string, string>
    return Result.ok(5)
end
local r = f()
r:nope()
)");
    CHECK_TRUE(HasError(bad, "Результат функции f"));
    CHECK_TRUE(HasError(bad, "у типа Result нет метода 'nope'"));
}

// --- 15. Обобщения, псевдонимы, объединения ------------------------------------------------
TEST(SageLua_generics_aliases_and_unions) {
    Lua lua;
    sol::object r = lua.Run("gen.slua", R"(
class Enemy
    public hp: number = 10
end
class Pool<T>
    private items: T[] = {}
    function add(item: T): void
        table.insert(self.items, item)
    end
    function get(index: integer): T?
        return self.items[index]
    end
    function size(): integer
        return #self.items
    end
end
type EntityId = integer
type DamageInfo = {
    amount: number,
    source: Entity?,
    critical: boolean
}
type Callback<T> = function(value: T): void
type Weapon = string | number
local enemies = Pool<Enemy>()
enemies:add(Enemy())
enemies:add(Enemy())
local other = Pool<Enemy>()
local id: EntityId = 7
local info: DamageInfo = { amount = 5, critical = true }
local w: Weapon = "Sword"
w = 3
local cb: Callback<number> = function(v) end
return enemies:size() .. "|" .. other:size() .. "|" .. enemies:get(1).hp .. "|" .. id .. "|" .. info.amount
)");
    CHECK_TRUE(r.is<std::string>() && r.as<std::string>() == "2|0|10|7|5");

    CompileResult bad = Compile("genbad.slua", R"(
class Enemy end
class Pool<T>
    function add(item: T): void end
end
local p = Pool<Enemy>()
p:add("not an enemy")
type DamageInfo = { amount: number, critical: boolean }
local info: DamageInfo = { amount = "x", critical = true }
local info2: DamageInfo = { amount = 1 }
type Weapon = string | number
local w: Weapon = true
local u: Unknown = 1
)");
    CHECK_TRUE(HasError(bad, "Аргумент item у Pool:add()"));
    CHECK_TRUE(HasError(bad, "поле amount"));
    CHECK_TRUE(HasError(bad, "не хватает поля 'critical'"));
    CHECK_TRUE(HasError(bad, "Ожидалось значение string | number."));
    CHECK_TRUE(HasError(bad, "неизвестный тип Unknown"));
}

// --- 14. Диагностика: формат и точная позиция ----------------------------------------------
TEST(SageLua_diagnostic_format_matches_the_spec) {
    CompileResult r = Compile("Scripts/Player.slua", R"(
class PlayerController extends Component
    public speed: number = 5
    function onStart(): void
        self.speed = "fast"
    end
end
)");
    CHECK_FALSE(r.Ok);
    const Diagnostic* d = r.FirstError();
    CHECK_TRUE(d != nullptr);
    if (!d) return;
    const std::string text = d->Format();
    CHECK_TRUE(text.rfind("Scripts/Player.slua:5:22\nОшибка типа:\nОжидалось значение number.\nПолучено значение string.\nПоле: PlayerController.speed\nПолучено: \"fast\"", 0) == 0);
    if (text.rfind("Scripts/Player.slua:5:22", 0) != 0) std::printf("       %s\n", text.c_str());
    // Синтаксическая ошибка — с позицией.
    CompileResult syn = Compile("s.slua", "local x = (1 +\n");
    CHECK_TRUE(HasError(syn, "Синтаксическая ошибка"));
}

// --- Генератор: строки Lua совпадают со строками .slua ---------------------------------------
TEST(SageLua_runtime_errors_point_at_the_sage_line) {
    Lua lua;
    lua.Run("lines.slua", R"(
class A
    function boom(): void
        local t = nil
        local x = t.field
    end
end
A():boom()
)", false);
    CHECK_TRUE(lua.lastError.find("lines.slua:5:") != std::string::npos);
    if (lua.lastError.find("lines.slua:5:") == std::string::npos) std::printf("       %s\n", lua.lastError.c_str());
}

// --- 13. Модули: import / export, цикл, отсутствие -------------------------------------------
TEST(SageLua_modules_import_export_and_cycles) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_lang_modules";
    fs::remove_all(dir);
    fs::create_directories(dir / "Game");
    auto write = [&](const fs::path& p, const std::string& text) {
        std::ofstream f(p, std::ios::binary);
        f << text;
    };
    write(dir / "Game/Player.slua", R"(
export class Player
    public health: number = 100
    function hit(n: number): void self.health = self.health - n end
end
export function createPlayer(): Player
    return Player()
end
export const MAX_PLAYERS: integer = 4
)");
    write(dir / "Game/Util.lua", "return { twice = function(x) return x * 2 end }\n");
    write(dir / "Game/A.slua", "import { B } from \"Game/B\"\nexport class A end\n");
    write(dir / "Game/B.slua", "import { A } from \"Game/A\"\nexport class B end\n");
    CompileOptions opt;
    opt.Loader = [&](const std::string& spec, const std::string& from, std::string& resolved, std::string& text) {
        if (!ResolveModulePath(spec, from, {dir.string()}, resolved)) return false;
        std::ifstream f(resolved, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    const std::string main = (dir / "main.slua").string();
    CompileResult ok = Compile(main, R"(
import { Player, createPlayer, MAX_PLAYERS } from "Game/Player"
import { Vector3 } from "SAGE/Math"
import { Input, Time } from "SAGE/Core"
local p: Player = createPlayer()
p:hit(10)
local n: integer = MAX_PLAYERS
)", opt);
    Dump(ok);
    CHECK_TRUE(ok.Ok);
    CompileResult typed = Compile(main, R"(
import { Player } from "Game/Player"
local p = Player()
p:hit("ten")
)", opt);
    CHECK_TRUE(HasError(typed, "Аргумент n у Player:hit()"));
    CompileResult missing = Compile(main, "import { Nope } from \"Game/Player\"\nimport { X } from \"Game/Absent\"\n", opt);
    CHECK_TRUE(HasError(missing, "не экспортирует 'Nope'"));
    CHECK_TRUE(HasError(missing, "модуль 'Game/Absent' не найден"));
    CompileResult cycle = Compile((dir / "Game/A.slua").string(), "import { B } from \"Game/B\"\nexport class A end\n", opt);
    CHECK_TRUE(HasError(cycle, "циклический импорт"));
    CompileResult engine = Compile(main, "import { Teleporter } from \"SAGE/Core\"\n", opt);
    CHECK_TRUE(HasError(engine, "в модуле SAGE/Core нет 'Teleporter'"));

    // Runtime: import исполняет модуль через загрузчик движка.
    Lua lua;
    lua.L.set_function("__sage_native_load", [&](const std::string& spec, const std::string& from) {
        std::string resolved, text;
        sol::state_view L = lua.L;
        if (!opt.Loader(spec, from, resolved, text)) return std::make_tuple(sol::object(sol::lua_nil), sol::make_object(L, "нет модуля " + spec));
        if (resolved.size() > 5 && resolved.compare(resolved.size() - 5, 5, ".slua") == 0) {
            CompileResult c = Compile(resolved, text, opt);
            if (!c.Ok) return std::make_tuple(sol::object(sol::lua_nil), sol::make_object(L, c.FirstError()->Format()));
            text = c.Lua;
        }
        return std::make_tuple(sol::make_object(L, text), sol::make_object(L, resolved));
    });
    CompileResult runnable = Compile(main, R"(
import { createPlayer, MAX_PLAYERS } from "Game/Player"
import { twice } from "Game/Util"
local p = createPlayer()
p:hit(30)
return p.health + MAX_PLAYERS + twice(5)
)", opt);
    CHECK_TRUE(runnable.Ok);
    sol::protected_function fn = lua.L.load(runnable.Lua, "@main.slua");
    sol::protected_function_result res = fn();
    if (!res.valid()) {
        sol::error e = res;
        std::printf("       %s\n", e.what());
    }
    CHECK_TRUE(res.valid() && res.get<int>() == 84);
    fs::remove_all(dir);
}
