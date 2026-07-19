// Модульные тесты расширенного Lua-API скриптинга: доступ к компонентам любой
// сущности (Has/Get/Add/Remove), иерархия из скрипта, математические хелперы,
// доступ к освещению сцены и — главное — обмен сообщениями между скриптами
// (SendMessage/Broadcast + OnMessage). Всё на CPU, БЕЗ GL: тесты не трогают
// меши/текстуры (те требуют контекст), только логику API поверх ECS и Lua.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "sage/scripting/ScriptEngine.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

namespace {
// Пишет временный .lua во временную папку, отдаёт путь. Тела скриптов короткие,
// поэтому держать их прямо в тесте нагляднее, чем заводить файлы-фикстуры.
std::string WriteTempScript(const std::string& name, const std::string& body) {
    std::string path = std::string("sage_test_") + name + ".lua";
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}
} // namespace

TEST(Scripting_component_add_get_has_remove) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Hero");
    se.Lua()["e"] = o;

    // AddLight создаёт компонент и отдаёт ссылкой — правка идёт прямо в ECS.
    bool hadBefore = se.Lua().script("return e:HasLight()");
    CHECK_FALSE(hadBefore);
    se.Lua().script("e:AddLight().Intensity = 3.5");
    CHECK_TRUE(scene.Registry().all_of<LightComponent>(o.Entity()));
    CHECK_NEAR(scene.Registry().get<LightComponent>(o.Entity()).Intensity, 3.5f, 1e-4);

    // GetLight отдаёт тот же компонент (не nil) — читаем записанное значение.
    float readBack = se.Lua().script("return e:GetLight().Intensity");
    CHECK_NEAR(readBack, 3.5f, 1e-4);

    // RemoveLight снимает компонент, HasLight -> false, GetLight -> nil.
    se.Lua().script("e:RemoveLight()");
    CHECK_FALSE(scene.Registry().all_of<LightComponent>(o.Entity()));
    bool getNil = se.Lua().script("return e:GetLight() == nil");
    CHECK_TRUE(getNil);
}

TEST(Scripting_enum_values_bound) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Lamp");
    se.Lua()["e"] = o;

    // Именованное значение enum пишется в поле компонента и читается обратно.
    se.Lua().script("local l = e:AddLight(); l.Kind = LightType.Spot");
    CHECK_TRUE(scene.Registry().get<LightComponent>(o.Entity()).Kind == LightComponent::Type::Spot);

    se.Lua().script("e:AddRigidBody().Type = BodyType.Kinematic");
    CHECK_TRUE(scene.Registry().get<RigidBodyComponent>(o.Entity()).Type
               == sage::physics::BodyType::Kinematic);
}

TEST(Scripting_hierarchy_from_lua) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject parent = scene.CreateObject("Parent");
    parent.GetTransform().Position = {10.0f, 0.0f, 0.0f};
    GameObject child = scene.CreateObject("Child");
    child.GetTransform().Position = {0.0f, 5.0f, 0.0f};
    se.Lua()["p"] = parent;
    se.Lua()["c"] = child;

    se.Lua().script("c:SetParent(p)");
    CHECK_TRUE(scene.ParentOf(child.Entity()) == parent.Entity());

    // WorldPosition из Lua = композиция матрицы родителя и локальной ребёнка.
    glm::vec3 wp = se.Lua().script("return c:WorldPosition()");
    CHECK_NEAR(wp.x, 10.0f, 1e-4);
    CHECK_NEAR(wp.y, 5.0f, 1e-4);

    // Parent() и Children() согласованы.
    int childCount = se.Lua().script("return #p:Children()");
    CHECK_EQ(childCount, 1);
    bool parentMatches = se.Lua().script("return c:Parent().Id == p.Id");
    CHECK_TRUE(parentMatches);

    // Unparent возвращает в корень.
    se.Lua().script("c:Unparent()");
    CHECK_TRUE(scene.ParentOf(child.Entity()) == entt::null);
}

TEST(Scripting_destroy_from_lua) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Doomed");
    int id = o.Id();
    se.Lua()["e"] = o;
    se.Lua().script("e:Destroy()");
    CHECK_FALSE(scene.Get(id).Valid());
}

TEST(Scripting_math_helpers) {
    ScriptEngine se;

    // Cross(X, Y) == Z
    glm::vec3 c = se.Lua().script("return Cross(Vec3.new(1,0,0), Vec3.new(0,1,0))");
    CHECK_NEAR(c.z, 1.0f, 1e-4);

    float clamped = se.Lua().script("return Clamp(5.0, 0.0, 1.0)");
    CHECK_NEAR(clamped, 1.0f, 1e-4);

    float lerpF = se.Lua().script("return Lerp(0.0, 10.0, 0.5)");
    CHECK_NEAR(lerpF, 5.0f, 1e-4);

    glm::vec3 lerpV = se.Lua().script("return Lerp(Vec3.new(0,0,0), Vec3.new(2,4,6), 0.5)");
    CHECK_NEAR(lerpV.y, 2.0f, 1e-4);

    float rad = se.Lua().script("return Radians(180.0)");
    CHECK_NEAR(rad, 3.14159265f, 1e-3);
    float deg = se.Lua().script("return Degrees(3.14159265)");
    CHECK_NEAR(deg, 180.0f, 1e-2);
}

TEST(Scripting_lighting_access) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    se.Lua().script("GetLighting().Sun.Intensity = 0.25");
    CHECK_NEAR(scene.Lighting.Sun.Intensity, 0.25f, 1e-4);

    se.Lua().script("GetLighting().Fog.Enabled = true; GetLighting().Fog.Start = 7.0");
    CHECK_TRUE(scene.Lighting.Fog.Enabled);
    CHECK_NEAR(scene.Lighting.Fog.Start, 7.0f, 1e-4);
}

TEST(Scripting_messages_between_scripts) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Наблюдаем доставку через C++-колбэк, доступный скриптам как глобальная
    // функция TestRecord (окружение скрипта строится из globals).
    int count = 0;
    std::string last;
    se.Lua().set_function("TestRecord", [&](const std::string& n) { ++count; last = n; });

    std::string path = WriteTempScript("recv",
        "function OnMessage(entity, name, data)\n"
        "    TestRecord(name)\n"
        "end\n");

    GameObject a = scene.CreateObject("A");
    GameObject b = scene.CreateObject("B");
    se.AttachScript(a, path);
    se.AttachScript(b, path);
    se.Lua()["a"] = a;

    // Адресное сообщение — только скрипту сущности A.
    se.Lua().script("SendMessage(a, 'hit')");
    CHECK_EQ(count, 1);
    CHECK_EQ(last, std::string("hit"));

    // Широковещательное — обоим.
    se.Lua().script("Broadcast('ping')");
    CHECK_EQ(count, 3);

    // Адресное по числовому Id тоже работает.
    int aId = a.Id();
    se.Lua().script("SendMessage(" + std::to_string(aId) + ", 'byid')");
    CHECK_EQ(count, 4);

    std::remove(path.c_str());
}

TEST(Scripting_message_payload_delivered) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    double sum = 0.0;
    se.Lua().set_function("TestNumber", [&](double v) { sum += v; });

    std::string path = WriteTempScript("payload",
        "function OnMessage(entity, name, data)\n"
        "    if type(data) == 'number' then TestNumber(data) end\n"
        "end\n");

    GameObject a = scene.CreateObject("A");
    se.AttachScript(a, path);
    se.Lua()["a"] = a;
    se.Lua().script("SendMessage(a, 'add', 42)");
    CHECK_NEAR(sum, 42.0, 1e-6);

    std::remove(path.c_str());
}
