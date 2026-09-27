// Раннер тестов на Lua (sage/scripting/ScriptTests.h, lua/LuaApi_Test.cpp) и
// то, что через него нашлось в API скриптинга.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace {

std::string Write(const std::string& name, const std::string& body) {
    const std::string path = "sage_luatest_" + name + ".lua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    return path;
}

std::unique_ptr<sage::scripting::ScriptingSystem> MakeSystem(Scene& scene) {
    auto sys = std::make_unique<sage::scripting::ScriptingSystem>();
    sys->AddBackend(sage::scripting::MakeLuaBackend({}));
    sage::scripting::ScriptServices services;
    services.ScenePtr = &scene;
    sys->Bind(services);
    return sys;
}

// Прогнать тесты файла до конца (не больше maxFrames кадров по dt).
std::vector<sage::scripting::TestResult> Run(const std::string& body, int maxFrames = 600,
                                             float dt = 1.0f / 60.0f) {
    Scene scene("t");
    const std::string path = Write("run", body);
    GameObject o = scene.CreateObject("Runner");
    scene.Registry().emplace<ScriptComponent>(o.Entity()).Path = path;
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    sys->RunTests(o);
    for (int i = 0; i < maxFrames && sys->PendingTests() > 0; ++i) sys->Update(dt);
    std::vector<sage::scripting::TestResult> out = sys->TestResults();
    sys->Shutdown();
    std::remove(path.c_str());
    return out;
}

} // namespace

TEST(LuaRunner_reports_pass_fail_with_the_test_line) {
    const auto r = Run(R"(
local T = {}
function T:test_ok() Test.eq(1 + 1, 2) end
function T:test_bad()
    Test.eq(2 + 2, 5, "арифметика")
end
return T
)");
    CHECK_EQ((int)r.size(), 2);
    if (r.size() != 2) return;
    CHECK_EQ(r[0].Name, std::string("test_ok"));   // порядок — как в файле
    CHECK_TRUE(r[0].Passed);
    CHECK_FALSE(r[1].Passed);
    CHECK_EQ(r[1].Line, 5);                        // строка ТЕСТА, а не библиотеки
    CHECK_TRUE(r[1].Message.find("арифметика: expected 5, got 4") != std::string::npos);
}

TEST(LuaRunner_waits_real_frames_and_times_out) {
    const auto r = Run(R"(
local T = {}
function T:Update(dt) self.frames = (self.frames or 0) + 1 end
function T:test_wait_frames()
    local f = self.frames or 0
    Test.frames(5)
    Test.eq((self.frames or 0) - f, 5)
end
function T:test_wait_seconds()
    local f = self.frames or 0
    Test.wait(0.5)
    Test.expect((self.frames or 0) - f >= 29, "прошло кадров: " .. ((self.frames or 0) - f))
end
function T:test_timeout()
    Test.timeout(0.2)
    while true do Test.frames(1) end
end
return T
)");
    CHECK_EQ((int)r.size(), 3);
    if (r.size() != 3) return;
    CHECK_TRUE(r[0].Passed);
    CHECK_TRUE(r[1].Passed);
    CHECK_FALSE(r[2].Passed);
    CHECK_TRUE(r[2].Message.find("тайм-аут") != std::string::npos);
}

// before_each/after_each — вокруг КАЖДОГО теста и тоже могут ждать кадры.
TEST(LuaRunner_before_and_after_each_wrap_every_test_and_may_wait) {
    const auto r = Run(R"(
local T = {}
function T:before_each() self.log = (self.log or "") .. "[" end
function T:after_each() Test.frames(1); self.log = self.log .. "]" end
function T:test_a() self.log = self.log .. "a" end
function T:test_b() Test.eq(self.log, "[a][b", "журнал") end
function T:test_c() error("намеренно") end
return T
)");
    CHECK_EQ((int)r.size(), 3);
    if (r.size() != 3) return;
    CHECK_TRUE(r[0].Passed);
    CHECK_FALSE(r[1].Passed);   // журнал "[a][" — b ещё не дописана: проверка честная
    CHECK_FALSE(r[2].Passed);
    CHECK_TRUE(r[2].Message.find("намеренно") != std::string::npos);
}

// ВТОРОЙ ЭКЗЕМПЛЯР ТОГО ЖЕ ФАЙЛА НЕ ДЕЛИТ ГЛОБАЛЬНЫЕ ПЕРВОГО. Скомпилированный
// файл кэшировался одной функцией, и окружение (_ENV) подменялось у неё при
// каждом создании экземпляра — а _ENV общий у всех замыканий этой функции.
// Итог: глобальные переменные скрипта старого стиля у ПЕРВОГО объекта молча
// начинали указывать в окружение последнего.
TEST(LuaRunner_two_instances_of_one_file_keep_their_own_globals) {
    const std::string helper = Write("globals", R"(
counter = 0
function Bump() counter = counter + 1; return counter end
function Count() return counter end
)");
    const auto r = Run(R"(
local T = {}
function T:test_globals_are_per_instance()
    local a = Scene.Create("A")
    a:AddComponent("Script", {path = ")" + helper + R"("})
    a:Call("Bump"); a:Call("Bump")
    local b = Scene.Create("B")
    b:AddComponent("Script", {path = ")" + helper + R"("})
    b:Call("Bump")
    Test.eq(a:Call("Count"), 2, "глобальные первого объекта")
    Test.eq(b:Call("Count"), 1, "глобальные второго объекта")
end
return T
)");
    std::remove(helper.c_str());
    CHECK_EQ((int)r.size(), 1);
    if (!r.empty()) {
        if (!r[0].Passed) std::printf("       %s\n", r[0].Message.c_str());
        CHECK_TRUE(r[0].Passed);
    }
}

// Подписки скрипта, поставленного кодом, принадлежат ЕМУ: объект уничтожили —
// они молчат и уходят; второй такой же объект слышит событие один раз.
TEST(LuaRunner_script_added_by_code_owns_its_subscriptions) {
    const std::string helper = Write("counter", R"(
local C = {}
function C:Start()
    self.count = 0
    Events.on("tick", function() self.count = self.count + 1 end)
end
return C
)");
    const auto r = Run(R"(
local T = {}
local function make()
    local o = Scene.Create("Counter")
    return o, o:AddComponent("Script", {path = ")" + helper + R"("})
end
function T:test_one()
    local o = make(); o:Destroy()
end
function T:test_two()
    local o, s = make()
    Events.emit("tick")
    Test.eq(s.count, 1, "второй объект услышал событие")
    Test.eq(Events.count("tick"), 1, "подписка первого объекта пережила Destroy в том же кадре")
    Test.frames(2)
    o:Destroy()
    Test.eq(Events.count("tick"), 0, "подписки ушли не сразу, а только к следующему кадру")
    Test.frames(2)
    Test.eq(Events.count("tick"), 0, "подписки пережили объекты")
end
return T
)");
    std::remove(helper.c_str());
    CHECK_EQ((int)r.size(), 2);
    for (const auto& t : r) {
        if (!t.Passed) std::printf("       %s: %s\n", t.Name.c_str(), t.Message.c_str());
        CHECK_TRUE(t.Passed);
    }
}
