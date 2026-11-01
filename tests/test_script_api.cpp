#include "TestFramework.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sage/lang/ApiRegistry.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptApi.h"
#include "sage/scripting/ScriptEngine.h"

// ---------------------------------------------------------------------------
// КАТАЛОГ API: одна регистрация — описание с точными типами, вызов для
// любого языка, привязка к Lua.
// ---------------------------------------------------------------------------
using sage::lang::ApiEntry;
using sage::lang::ApiRegistry;
using sage::scripting::ApiCall;
using sage::vars::Value;

namespace {
ApiEntry Entry(const std::string& name) {
    ApiEntry e;
    ApiRegistry::Instance().Find(name, e);
    return e;
}
} // namespace

// Типы — от компилятора, а не от регулярного выражения: прежде у половины
// функций возврат значился «nil» (Clamp «ничего не возвращал»).
TEST(ScriptApi_bind_records_exact_signatures) {
    ScriptEngine se;
    const ApiEntry clamp = Entry("sage.math.Clamp");
    CHECK_TRUE(clamp.Qualified == "sage.math.Clamp");
    CHECK_EQ((int)clamp.Params.size(), 3);
    CHECK_TRUE(clamp.Params[0].Type == "number");
    CHECK_TRUE(clamp.Returns == std::vector<std::string>{"number"});
    CHECK_TRUE(clamp.Portable);
    // Псевдонимы: короткое имя модуля и прежнее глобальное — та же запись.
    CHECK_TRUE(Entry("Clamp").Qualified == "sage.math.Clamp");
    CHECK_TRUE(Entry("noise.Perlin").Qualified == "sage.noise.Perlin");
    // math.X — всегда стандартная библиотека Lua: короткого имени у модуля math нет.
    CHECK_FALSE(ApiRegistry::Instance().Has("math.Clamp"));

    const ApiEntry cross = Entry("sage.math.Cross");
    CHECK_TRUE(cross.Returns == std::vector<std::string>{"Vec3"});
    CHECK_TRUE(cross.Params[1].Type == "Vec3");

    // Необязательный аргумент — из sol::optional.
    const ApiEntry perlin = Entry("sage.noise.Perlin");
    CHECK_EQ((int)perlin.Params.size(), 3);
    CHECK_TRUE(perlin.Params[2].Optional);

    // Функция с таблицей Lua описана, но универсального вызова у неё нет.
    bool sawLuaOnly = false;
    for (const ApiEntry& e : ApiRegistry::Instance().All())
        if (!e.Portable && !e.Unknown) sawLuaOnly = true;
    CHECK_TRUE(sawLuaOnly);
    CHECK_TRUE(ApiRegistry::Instance().All().size() > 300);
}

// Универсальный вызов: значения движка туда и обратно, ошибки — текстом.
TEST(ScriptApi_portable_call_converts_values_and_reports_errors) {
    ScriptEngine se;
    ApiCall call;
    call.Args = {Value(5.0f), Value(0), Value(2.0f)};          // целое приводится к числу
    CHECK_TRUE(se.Api().Call("sage.math.Clamp", call));
    CHECK_TRUE(call.Results.size() == 1 && std::abs(call.Results[0].AsFloat() - 2.0f) < 1e-6f);

    ApiCall cross;
    cross.Args = {Value(glm::vec3(1, 0, 0)), Value(glm::vec3(0, 1, 0))};
    CHECK_TRUE(se.Api().Call("Cross", cross));                    // по прежнему имени тоже
    CHECK_TRUE(cross.Results.size() == 1 && std::abs(cross.Results[0].AsVec3().z - 1.0f) < 1e-6f);

    ApiCall few;
    few.Args = {Value(1.0f)};
    CHECK_FALSE(se.Api().Call("sage.math.Clamp", few));
    CHECK_TRUE(few.Error.find("ждёт 3") != std::string::npos);

    ApiCall wrong;
    wrong.Args = {Value(std::string("x")), Value(0.0f), Value(1.0f)};
    CHECK_FALSE(se.Api().Call("sage.math.Clamp", wrong));
    CHECK_TRUE(wrong.Error.find("аргумент 1") != std::string::npos);

    ApiCall none;
    CHECK_FALSE(se.Api().Call("sage.nope.Nothing", none));
    CHECK_TRUE(se.Api().Size() > 150);

    // Ссылка на объект — номер объекта сцены.
    Scene scene("S");
    GameObject box = scene.CreateObject("Box");
    se.BindScene(scene);
    ApiCall named;
    named.ScenePtr = &scene;
    named.Args = {Value(sage::vars::EntityRef{box.Id()})};
    sage::lang::ApiEntry e;
    // Любая переносимая функция, принимающая объект и отдающая строку или
    // число, проверяет перевод ссылки; конкретная — неважна.
    for (const ApiEntry& x : ApiRegistry::Instance().All()) {
        if (x.Portable && x.Params.size() == 1 && x.Params[0].Type == "Entity" && !x.Returns.empty()) {
            e = x;
            break;
        }
    }
    CHECK_FALSE(e.Qualified.empty());
    CHECK_TRUE(se.Api().Call(e.Qualified, named));
}

// Своя функция для всех языков: без sol, с проверкой переносимости типов на
// этапе компиляции. Видна в Lua (под полным и коротким именем), в
// универсальном вызове и в каталоге с выведенными типами.
TEST(ScriptApi_register_api_function_reaches_every_consumer) {
    sage::scripting::RegisterApiFunction(
        "testweather", "rain", [](float k, std::optional<int> drops) { return k * (float)drops.value_or(10); },
        "дождь силой k", "test:weather");
    ScriptEngine se;
    se.Lua().script(R"LUA(
        a = sage.testweather.rain(0.5)
        b = testweather.rain(0.5, 4)
    )LUA");
    CHECK_NEAR(se.Lua()["a"].get<float>(), 5.0f, 1e-6f);
    CHECK_NEAR(se.Lua()["b"].get<float>(), 2.0f, 1e-6f);

    ApiCall call;
    call.Args = {Value(0.25f), Value(8)};
    const bool called = se.Api().Call("sage.testweather.rain", call);
    if (!called) std::printf("    ошибка вызова: %s\n", call.Error.c_str());
    CHECK_TRUE(called);
    CHECK_TRUE(!call.Results.empty() && std::abs(call.Results[0].AsFloat() - 2.0f) < 1e-6f);

    const ApiEntry e = Entry("sage.testweather.rain");
    CHECK_TRUE(e.Doc == "дождь силой k");
    CHECK_TRUE(e.Origin == "test:weather");
    CHECK_TRUE(e.Params[1].Optional && e.Params[1].Type == "integer");
    CHECK_TRUE(e.Returns == std::vector<std::string>{"number"});
    CHECK_TRUE(ApiRegistry::Instance().ToLuaHints().find("function sage.testweather.rain(arg1, arg2)") !=
               std::string::npos);
    CHECK_TRUE(ApiRegistry::Instance().ToJson().find("\"sage.testweather.rain\"") != std::string::npos);

    // Снятие источника — функции нет ни в каталоге, ни в новых состояниях.
    CHECK_EQ(sage::scripting::RemovePortableFunctions("test:weather"), 1);
    CHECK_FALSE(ApiRegistry::Instance().Has("sage.testweather.rain"));
    ScriptEngine fresh;
    CHECK_FALSE(fresh.Lua()["sage"]["testweather"].valid());
}

// Имена API не конфликтуют: одно имя — одна функция. Повтор регистрации той
// же функции (новое состояние скриптов) конфликтом не считается.
TEST(ScriptApi_engine_api_has_no_name_conflicts) {
    ScriptEngine a;
    ScriptEngine b;
    const std::vector<std::string> conflicts = ApiRegistry::Instance().Conflicts();
    for (const std::string& c : conflicts) std::printf("    %s\n", c.c_str());
    CHECK_TRUE(conflicts.empty());
}

// --- Реестр языков -------------------------------------------------------------

#include "sage/scripting/Languages.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"

namespace {
// Язык-заглушка: исполнение есть, но ничего не делает. Для проверки того, что
// хост подключает любой зарегистрированный язык, не зная его заранее.
class NullBackend : public sage::scripting::LanguageBackend {
public:
    const char* Name() const override { return "Null"; }
    bool Handles(const std::string& ext) const override { return ext == ".nul"; }
    void Bind(const sage::scripting::ScriptServices&) override {}
    sage::vars::Table ParseFields(const std::string&, const std::string&) const override { return {}; }
    sage::scripting::InstanceId Create(const sage::scripting::ScriptSource&, GameObject,
                                       sage::scripting::ScriptError&) override { return 1; }
    void Destroy(sage::scripting::InstanceId) override {}
    bool Has(sage::scripting::InstanceId, sage::scripting::Hook) const override { return false; }
    bool Call(sage::scripting::InstanceId, sage::scripting::Hook, float, sage::scripting::ScriptError&) override {
        return true;
    }
    bool CallWith(sage::scripting::InstanceId, sage::scripting::Hook, GameObject,
                  sage::scripting::ScriptError&) override { return true; }
    bool CallNamed(sage::scripting::InstanceId, sage::scripting::Hook, const std::string&,
                   sage::scripting::ScriptError&) override { return true; }
    using LanguageBackend::Invoke;
    bool Invoke(sage::scripting::InstanceId, const std::string&, const std::vector<Value>&, std::vector<Value>&,
                sage::scripting::ScriptError&) override { return true; }
    bool HasMethod(sage::scripting::InstanceId, const std::string&) const override { return false; }
    bool InvokeEvent(sage::scripting::InstanceId, const std::string&, const sage::events::Event&,
                     sage::scripting::ScriptError&) override { return true; }
    void ApplyFields(sage::scripting::InstanceId, const sage::vars::Table&) override {}
    void Reset() override {}
};
} // namespace

// Lua и SAGE Lua — обычные записи реестра; инструменты разбора (поля,
// методы) идут через реестр, а не через выбор по расширению.
TEST(Languages_builtins_are_registry_entries) {
    using namespace sage::scripting;
    Language lua, slua;
    CHECK_TRUE(LanguageForPath("assets/scripts/player.LUA", lua));
    CHECK_TRUE(lua.Id == "lua" && lua.CreateBackend);
    CHECK_TRUE(LanguageForPath("a/b.slua", slua) && slua.RunsOn == "lua" && !slua.CreateBackend);
    CHECK_TRUE(IsScriptPath("x.lua") && !IsScriptPath("x.png") && !IsScriptPath("lua"));
    const sage::vars::Table fields = ParseFields("p.lua", "speed = 5\nfunction Update(dt) end\n");
    CHECK_TRUE(fields.Find("speed") != nullptr);
    const std::vector<std::string> methods = ParseMethods("p.lua", "function Menu:start_game() end\n");
    CHECK_TRUE(methods.size() == 1 && methods[0] == "start_game");
}

// Свой язык — одна регистрация: его файлы узнают инструменты, его поля видит
// инспектор, хост подключает его исполнение. Расширение чужого языка не отдаётся.
TEST(Languages_custom_language_plugs_in_everywhere) {
    using namespace sage::scripting;
    Language nul;
    nul.Id = "null-test";
    nul.Name = "Null";
    nul.Extensions = {".NUL"};
    nul.Origin = "test:null";
    nul.CreateBackend = [](const LanguageHost&) { return std::make_unique<NullBackend>(); };
    nul.Fields = [](const std::string&, const std::string& src) {
        sage::vars::Table t;
        t.Set("from_null", Value((int)src.size()));
        return t;
    };
    std::string err;
    CHECK_TRUE(RegisterLanguage(nul, &err));
    CHECK_TRUE(IsScriptPath("enemy.nul"));
    const std::vector<std::string> exts = ScriptExtensions();
    CHECK_TRUE(std::find(exts.begin(), exts.end(), ".nul") != exts.end());
    CHECK_TRUE(ParseFields("enemy.nul", "abc").Find("from_null") != nullptr);

    Language thief;
    thief.Id = "thief";
    thief.Name = "Thief";
    thief.Extensions = {".lua"};
    CHECK_FALSE(RegisterLanguage(thief, &err));
    CHECK_TRUE(err.find("Lua") != std::string::npos);

    ScriptEngine engine;
    ScriptingSystem sys;
    CHECK_EQ(AttachLanguages(sys, {&engine, {}}), 2);   // Lua и Null; SAGE Lua идёт в Lua
    CHECK_TRUE(sys.Runtime().BackendFor("enemy.nul") != nullptr);
    CHECK_TRUE(std::string(sys.Runtime().BackendFor("enemy.nul")->Name()) == "Null");
    CHECK_TRUE(std::string(sys.Runtime().BackendFor("p.slua")->Name()) == "Lua");

    CHECK_EQ(UnregisterLanguages("test:null"), 1);
    CHECK_FALSE(IsScriptPath("enemy.nul"));
}

// --- Язык во внешнем процессе ----------------------------------------------------

#include <filesystem>
#include <fstream>
#include "sage/scene/ScriptComponent.h"
#include "sage/scripting/ProcessLanguage.h"
#include "sage/core/ChildProcess.h"
#include <chrono>

namespace {
namespace fs = std::filesystem;

void WriteText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Проект с языком Python (эталонный адаптер из editor/assets/languages).
fs::path MakePythonProject(const std::string& name, const std::string& extra = {}) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    const fs::path runner = fs::path(SAGE_TEST_DATA_DIR) / ".." / ".." / "editor" / "assets" / "languages" /
                            "python" / "sage_python.py";
    WriteText(dir / "scripting" / "languages.json",
              R"({"languages": [{"id": "python", "name": "Python", "extensions": [".py"],
                  "command": ["python3", ")" + fs::absolute(runner).generic_string() + R"("]})" + extra + "]}");
    return dir;
}

bool HavePython() {
    sage::ChildProcess p;
    std::string err;
    return p.Start({"python3", "-c", "print(1)"}, "", err);
}
} // namespace

// Python — без строки C++: описание в файле проекта. Поля видит инспектор,
// скрипт зовёт API движка по каталогу, Lua зовёт метод Python и получает ответ.
TEST(ProcessLanguage_python_script_lives_in_the_scene) {
    if (!HavePython()) {
        std::printf("    python3 не найден — проверка пропущена\n");
        return;
    }
    using namespace sage::scripting;
    const fs::path project = MakePythonProject("sage_proc_lang");
    WriteText(project / "assets" / "enemy.py", R"PY(
max_hp = 10
speed = 2.5

def start(self):
    self.hp = self.max_hp
    self.clamped = sage.math.Clamp(self.speed * 4, 0, 7)

def update(self, dt):
    self.ticks = getattr(self, "ticks", 0) + 1

def take_damage(self, n):
    self.hp -= n
    return self.hp

def report(self):
    return self.hp, self.clamped, self.ticks
)PY");
    WriteText(project / "assets" / "caller.lua", R"LUA(
local Caller = {}
function Caller:Start()
    local enemy = Scene.find("Enemy")
    self.got = enemy:Call("take_damage", 3)
    Events.emit("lua_got", self.got)
end
return Caller
)LUA");

    CHECK_EQ(LoadProjectLanguages(project), 1);
    // Поля из текста — без запуска файла.
    const sage::vars::Table fields = ParseFields("assets/enemy.py", "max_hp = 10\nspeed = 2.5\n");
    CHECK_TRUE(fields.Find("max_hp") && fields.Find("max_hp")->Data.AsInt() == 10);
    CHECK_TRUE(fields.Find("speed") != nullptr);

    ScriptEngine engine;
    Scene scene("S");
    engine.BindScene(scene);
    GameObject enemy = scene.CreateObject("Enemy");
    sage::vars::Table overrides;
    overrides.Set("max_hp", Value(20));                    // значение из инспектора
    scene.Registry().emplace<ScriptComponent>(enemy.Entity(), ScriptComponent{"assets/enemy.py", overrides});
    GameObject caller = scene.CreateObject("Caller");
    scene.Registry().emplace<ScriptComponent>(caller.Entity(), ScriptComponent{"assets/caller.lua", {}});

    ScriptingSystem sys;
    sys.SetProjectDir(project);
    CHECK_EQ(AttachLanguages(sys, {&engine, project}), 2);  // Lua и Python
    ScriptServices services;
    services.ScenePtr = &scene;
    sys.Bind(services);
    float luaGot = -1.0f;
    scene.Events.Connect(0, "lua_got", [&](const sage::events::Event& e) { luaGot = e.Arg.AsFloat(); });
    CHECK_EQ(sys.AttachScene(scene), 2);
    sys.Update(1.0f / 60.0f);

    std::vector<Value> out;
    CHECK_TRUE(sys.Runtime().Invoke(enemy.Entity(), "report", {}, out));
    CHECK_EQ((int)out.size(), 3);
    if (out.size() == 3) {
        CHECK_EQ(out[0].AsInt(), 17);                       // 20 из инспектора − 3 от Lua
        CHECK_NEAR(out[1].AsFloat(), 7.0f, 1e-5f);          // Clamp(10, 0, 7) — API движка
        CHECK_TRUE(out[2].AsInt() >= 1);                    // Update пришёл
    }
    CHECK_NEAR(luaGot, 17.0f, 1e-5f);                       // Lua получил ответ Python
    UnregisterLanguages("project");
    fs::remove_all(project);
}

// Зависший или упавший язык не вешает и не роняет движок: ошибка вызова
// с причиной, а не замерший кадр.
TEST(ProcessLanguage_hung_or_broken_language_is_an_error_not_a_hang) {
    if (!HavePython()) return;
    using namespace sage::scripting;
    const fs::path project = fs::temp_directory_path() / "sage_proc_hang";
    fs::remove_all(project);
    WriteText(project / "scripting" / "languages.json",
              R"J({"languages": [{"id": "mute", "name": "Mute", "extensions": [".mute"], "start_timeout": 0.5,
                  "command": ["python3", "-c", "import sys, time\nfor l in sys.stdin: time.sleep(30)"]}]})J");
    WriteText(project / "assets" / "a.mute", "x");
    CHECK_EQ(LoadProjectLanguages(project), 1);
    ScriptEngine engine;
    Scene scene("S");
    GameObject obj = scene.CreateObject("A");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{"assets/a.mute", {}});
    ScriptingSystem sys;
    sys.SetProjectDir(project);
    AttachLanguages(sys, {&engine, project});
    ScriptServices services;
    services.ScenePtr = &scene;
    sys.Bind(services);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(sys.AttachScene(scene), 0);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_TRUE(waited < 5.0);
    UnregisterLanguages("project");

    // Ошибка в скрипте Python — с номером строки, следующий кадр жив.
    const fs::path py = MakePythonProject("sage_proc_broken");
    WriteText(py / "assets" / "bad.py", "def start(self):\n    x = 1\n    raise ValueError('сломано')\n");
    LoadProjectLanguages(py);
    Scene s2("S2");
    GameObject b = s2.CreateObject("B");
    s2.Registry().emplace<ScriptComponent>(b.Entity(), ScriptComponent{"assets/bad.py", {}});
    ScriptingSystem sys2;
    sys2.SetProjectDir(py);
    AttachLanguages(sys2, {&engine, py});
    ScriptServices s2services;
    s2services.ScenePtr = &s2;
    sys2.Bind(s2services);
    std::vector<ScriptError> errors;
    sys2.Runtime().SetErrorSink([&](const ScriptError& e) { errors.push_back(e); });
    sys2.AttachScene(s2);
    sys2.Update(1.0f / 60.0f);
    bool sawLine = false;
    for (const ScriptError& e : errors)
        if (e.Line == 3 && e.Message.find("сломано") != std::string::npos) sawLine = true;
    CHECK_TRUE(sawLine);
    UnregisterLanguages("project");
    fs::remove_all(project);
    fs::remove_all(py);
}

// --- Проверка типов и ресурсы ------------------------------------------------------

#include "sage/lang/SageApi.h"
#include "sage/scripting/lua/LuaBackend.h"

// Функция модуля игры видна проверке SAGE Lua: имя известно, тип возврата —
// из сигнатуры C++. Прежде проверка знала только сгенерированную таблицу.
TEST(ScriptApi_checker_sees_game_modules_from_the_live_catalog) {
    CHECK_TRUE(sage::lang::FindApiFunction("sage.testcheck.boost") == nullptr);
    sage::scripting::RegisterApiFunction("testcheck", "boost", [](float x) { return x * 2.0f; }, "", "test:check");
    const sage::lang::ApiFunction* f = sage::lang::FindApiFunction("sage.testcheck.boost");
    CHECK_TRUE(f != nullptr);
    if (f) {
        CHECK_TRUE(std::string(f->Ret) == "number");
        CHECK_TRUE(std::string(f->Params) == "arg1:number");
    }
    CHECK_TRUE(sage::lang::FindApiFunction("testcheck.boost") != nullptr);   // короткое имя модуля
    CHECK_TRUE(sage::lang::IsKnownGlobal("testcheck"));
    CHECK_TRUE(sage::lang::IsApiModule("sage.testcheck"));
    // Таблица по-прежнему первая ступень: функции движка находятся как раньше.
    CHECK_TRUE(sage::lang::FindApiFunction("sage.math.Clamp") != nullptr);
    sage::scripting::RemovePortableFunctions("test:check");
    CHECK_TRUE(sage::lang::FindApiFunction("sage.testcheck.boost") == nullptr);
}

// Предел памяти Lua: скрипт, раздувающий таблицу, получает ошибку «не хватает
// памяти», игра и соседние скрипты живы.
TEST(ScriptApi_lua_memory_limit_is_a_script_error) {
    using namespace sage::scripting;
    const fs::path dir = fs::temp_directory_path() / "sage_memlimit";
    fs::remove_all(dir);
    WriteText(dir / "hog.lua", R"LUA(
local Hog = {}
function Hog:Start()
    local t = {}
    for i = 1, 50000000 do t[i] = string.rep("x", 64) .. i end
end
return Hog
)LUA");
    WriteText(dir / "calm.lua", R"LUA(
local Calm = {}
function Calm:Start() self.ok = true; Events.emit("calm_ok", 1) end
return Calm
)LUA");
    Scene scene("S");
    GameObject hog = scene.CreateObject("Hog");
    scene.Registry().emplace<ScriptComponent>(hog.Entity(), ScriptComponent{"hog.lua", {}});
    GameObject calm = scene.CreateObject("Calm");
    scene.Registry().emplace<ScriptComponent>(calm.Entity(), ScriptComponent{"calm.lua", {}});
    ScriptingSystem sys;
    sys.SetProjectDir(dir);
    sys.AddBackend(MakeLuaBackend({}));
    sys.Runtime().SetMemoryLimit(64u * 1024 * 1024);
    ScriptServices services;
    services.ScenePtr = &scene;
    sys.Bind(services);
    bool calmOk = false;
    scene.Events.Connect(0, "calm_ok", [&](const sage::events::Event&) { calmOk = true; });
    std::vector<ScriptError> errors;
    sys.Runtime().SetErrorSink([&](const ScriptError& e) { errors.push_back(e); });
    sys.AttachScene(scene);
    bool sawMemory = false;
    for (const ScriptError& e : errors)
        if (e.Message.find("memory") != std::string::npos || e.Message.find("памят") != std::string::npos)
            sawMemory = true;
    CHECK_TRUE(sawMemory);
    CHECK_TRUE(calmOk);
    fs::remove_all(dir);
}
