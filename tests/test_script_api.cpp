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
    CHECK_TRUE(Entry("math.Clamp").Qualified == "sage.math.Clamp");

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
