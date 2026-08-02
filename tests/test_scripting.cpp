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
#include "sage/core/Log.h"

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

// --- Твины из Lua: TweenMove ведёт позицию к цели, тикая в UpdateAll ---
TEST(Scripting_tween_moves_entity_over_time) {
    ScriptEngine se;
    Scene scene("T");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Mover");
    o.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    se.Lua()["e"] = o;

    se.Lua().script("TweenMove(e, Vec3.new(10, 0, 0), 1.0, Ease.Linear)");
    int active = se.Lua().script("return ActiveTweens()");
    CHECK_EQ(active, 1);

    se.UpdateAll(0.5f); // линейно, половина -> x≈5
    CHECK_NEAR(o.GetTransform().Position.x, 5.0f, 1e-2);

    se.UpdateAll(0.6f); // перелёт за конец -> x=10, твин завершён
    CHECK_NEAR(o.GetTransform().Position.x, 10.0f, 1e-2);
    int activeAfter = se.Lua().script("return ActiveTweens()");
    CHECK_EQ(activeAfter, 0);
}

// --- TweenCancelAll останавливает всё ---
TEST(Scripting_tween_cancel_all) {
    ScriptEngine se;
    Scene scene("T");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Mover");
    se.Lua()["e"] = o;
    se.Lua().script("TweenScale(e, Vec3.new(2,2,2), 2.0); TweenColor(e, Vec3.new(1,0,0), 2.0)");
    CHECK_EQ((int)se.Lua().script("return ActiveTweens()"), 2);
    se.Lua().script("TweenCancelAll()");
    se.UpdateAll(0.1f);
    CHECK_EQ((int)se.Lua().script("return ActiveTweens()"), 0);
}

// ===========================================================================
//  Модули (require), параметры запуска и объявление раскладки из Lua
//
//  Всё это появилось, когда на движке начали делать игру целиком на скриптах:
//  без модулей игра размером больше одного экрана кода не раскладывается,
//  без параметров запуска её нельзя прогнать в CI, а без BindAction скрипт не
//  может завести ни одной своей клавиши.
// ===========================================================================

// require подтягивает соседний .lua из добавленной папки поиска, и модуль
// видит API движка (Vec3 и т.п.), потому что выполняется в глобальной среде.
TEST(Scripting_require_loads_module_from_search_path) {
    WriteTempScript("mod_geometry", R"LUA(
local M = {}
function M.Double(v) return Vec3.new(v.x * 2, v.y * 2, v.z * 2) end
M.NAME = "geometry"
return M
)LUA");

    ScriptEngine se;
    se.AddScriptSearchPath("."); // временные скрипты пишутся в текущую папку
    std::string name = se.Lua().script("local m = require 'sage_test_mod_geometry' return m.NAME");
    CHECK_EQ(name, std::string("geometry"));
    float doubled = se.Lua().script(
        "local m = require 'sage_test_mod_geometry' return m.Double(Vec3.new(1.5, 0, 0)).x");
    CHECK_NEAR(doubled, 3.0f, 1e-4);

    std::remove("sage_test_mod_geometry.lua");
}

// Модуль, загруженный через require, ОДИН на весь ScriptEngine: два скрипта
// сущностей видят одно и то же состояние. На этом держится общий мир у игры,
// разложенной по файлам, — иначе у каждого скрипта был бы свой «мир».
TEST(Scripting_require_shares_module_state_between_scripts) {
    WriteTempScript("mod_shared", "local M = {count = 0}\nreturn M\n");
    std::string a = WriteTempScript("uses_shared_a", R"LUA(
local S = require 'sage_test_mod_shared'
function OnStart(entity) S.count = S.count + 10 end
)LUA");
    std::string b = WriteTempScript("uses_shared_b", R"LUA(
local S = require 'sage_test_mod_shared'
function OnStart(entity) S.count = S.count + 5 end
)LUA");

    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.AddScriptSearchPath(".");
    se.AttachScript(scene.CreateObject("A"), a);
    se.AttachScript(scene.CreateObject("B"), b);

    int count = se.Lua().script("return require('sage_test_mod_shared').count");
    CHECK_EQ(count, 15);

    std::remove("sage_test_mod_shared.lua");
    std::remove(a.c_str());
    std::remove(b.c_str());
}

// Нативные модули скриптам игры недоступны намеренно: package.cpath пуст.
TEST(Scripting_native_module_loading_is_disabled) {
    ScriptEngine se;
    std::string cpath = se.Lua().script("return package.cpath");
    CHECK_TRUE(cpath.empty());
}

// Параметры запуска: строка «ключ=значение» из командной строки/окружения
// доходит до скрипта как LaunchArg/LaunchFlag.
TEST(Scripting_launch_args_reach_lua) {
    ScriptEngine se;
    se.SetLaunchArgsFromString("--autopilot=1 seed=42 --debug bare=value");

    CHECK_TRUE((bool)se.Lua().script("return LaunchFlag('autopilot')"));
    CHECK_TRUE((bool)se.Lua().script("return LaunchFlag('debug')")); // голый ключ = включён
    CHECK_FALSE((bool)se.Lua().script("return LaunchFlag('missing')"));

    int seed = se.Lua().script("return tonumber(LaunchArg('seed'))");
    CHECK_EQ(seed, 42);
    std::string bare = se.Lua().script("return LaunchArg('bare')");
    CHECK_EQ(bare, std::string("value"));
    CHECK_TRUE((bool)se.Lua().script("return LaunchArg('nope') == nil"));
}

// Раскладка объявляется из игры: BindAction заводит действие в карте ввода
// движка, и дальше его читает тот же IsActionDown, что и код на C++.
TEST(Scripting_bind_action_declares_actions) {
    ScriptEngine se;
    InputMap input;
    se.BindInput(input);

    int bound = se.Lua().script("return BindAction('Jump', 'SPACE')");
    CHECK_EQ(bound, 1);
    CHECK_TRUE(input.Has("Jump"));

    // Список клавиш: несколько привязок на одно действие (WASD и стрелки).
    int many = se.Lua().script("return BindAction('Move Forward', {'W', 'UP'})");
    CHECK_EQ(many, 2);
    CHECK_EQ((int)input.Get("Move Forward").Bindings().size(), 2);

    // Кнопки мыши — такие же привязки, как клавиши.
    CHECK_EQ((int)se.Lua().script("return BindAction('Break', 'MOUSE_LEFT')"), 1);

    // Нераспознанное имя не роняет игру: действие заводится, привязок 0.
    int bad = se.Lua().script("return BindAction('Nonsense', 'NOT_A_KEY')");
    CHECK_EQ(bad, 0);

    CHECK_FALSE(input.IsDown("Jump")); // никто ничего не нажимал
    CHECK_TRUE((bool)se.Lua().script("return HasAction('Jump')"));
    CHECK_FALSE((bool)se.Lua().script("return HasAction('Never Declared')"));
}

// «Сырой» ввод: скрипт получает смещение мыши и управляет захватом курсора —
// без этого вид от первого лица из Lua написать нельзя.
TEST(Scripting_raw_input_gives_mouse_and_capture) {
    struct FakeInput : sage::RawInputSource {
        glm::vec2 Delta{3.0f, -2.0f};
        bool Captured = false;
        glm::vec2 MouseDelta() const override { return Delta; }
        glm::vec2 MousePosition() const override { return {100.0f, 50.0f}; }
        int ScrollDelta() const override { return -1; }
        void SetMouseCaptured(bool c) override { Captured = c; }
        bool MouseCaptured() const override { return Captured; }
    } fake;

    ScriptEngine se;
    se.BindRawInput(fake);

    CHECK_NEAR((float)se.Lua().script("return GetMouseDelta().x"), 3.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetMouseDelta().y"), -2.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetMousePosition().x"), 100.0f, 1e-4);
    CHECK_EQ((int)se.Lua().script("return GetScrollDelta()"), -1);

    CHECK_FALSE((bool)se.Lua().script("return IsMouseCaptured()"));
    se.Lua().script("SetMouseCaptured(true)");
    CHECK_TRUE(fake.Captured);
    CHECK_TRUE((bool)se.Lua().script("return IsMouseCaptured()"));
}

// Без привязок «сырого» ввода GetMouseDelta обязан внятно ругаться, а не
// молча отдавать нули: молчаливый ноль выглядит как «мышь не двигают».
TEST(Scripting_raw_input_without_binding_errors) {
    ScriptEngine se;
    auto result = se.Lua().safe_script("return GetMouseDelta()", sol::script_pass_on_error);
    CHECK_FALSE(result.valid());
    // Скролл — исключение: он опционален, и ноль для него честный ответ.
    CHECK_EQ((int)se.Lua().script("return GetScrollDelta()"), 0);
}

// --- Пространства имён Lua-API ------------------------------------------------
//
// API был ПЛОСКОЙ КУЧЕЙ: 126 глобальных имён, где SetIKFootLock,
// SetWaterReflection и BorrowAnimations лежали рядом и ничем не отличались от
// функций самой игры. Теперь каждая функция живёт в модуле sage.<область> и
// одновременно доступна под прежним глобальным именем.
TEST(Scripting_api_is_grouped_into_modules) {
    ScriptEngine se;

    // Модули существуют и являются таблицами.
    const char* modules[] = {"anim", "ik", "physics", "render", "reflect", "ui",
                             "input", "audio", "fx", "tween", "time", "msg",
                             "math", "scene", "camera", "light", "app", "core"};
    for (const char* m : modules) {
        const bool isTable = se.Lua().script(std::string("return type(sage.") + m + ") == 'table'");
        if (!isTable) LOG_ERROR("Test") << "модуль sage." << m << " не таблица";
        CHECK_TRUE(isTable);
    }

    // Имя внутри модуля короче и осмысленнее: sage.ik.SetFootLock, а не
    // SetIKFootLock в общей куче.
    CHECK_TRUE(se.Lua().script("return type(sage.ik.SetFootLock) == 'function'"));
    CHECK_TRUE(se.Lua().script("return type(sage.anim.Borrow) == 'function'"));
    CHECK_TRUE(se.Lua().script("return type(sage.physics.Raycast) == 'function'"));
    CHECK_TRUE(se.Lua().script("return type(sage.reflect.SetWater) == 'function'"));
}

// Псевдоним — ТА ЖЕ функция, а не вторая регистрация. Это главное свойство:
// две регистрации одного поведения — та же болезнь, от которой уходим, только в
// новой форме, и разойтись они могут молча.
TEST(Scripting_legacy_names_are_the_same_function_object) {
    ScriptEngine se;
    CHECK_TRUE(se.Lua().script("return sage.ik.SetFootLock == SetIKFootLock"));
    CHECK_TRUE(se.Lua().script("return sage.anim.Borrow == BorrowAnimations"));
    CHECK_TRUE(se.Lua().script("return sage.physics.Raycast == Raycast"));
    CHECK_TRUE(se.Lua().script("return sage.tween.Move == TweenMove"));
    CHECK_TRUE(se.Lua().script("return sage.math.Clamp == Clamp"));
    CHECK_TRUE(se.Lua().script("return sage.core.log == log"));
}

// Старые имена не помечены устаревшими и удалять их не планируется: все
// существующие игры написаны на них. Тест закрепляет это обещание — иначе
// «наведение порядка» однажды тихо сломает работающие скрипты.
TEST(Scripting_every_legacy_global_still_answers) {
    ScriptEngine se;
    const char* legacy[] = {"SpawnObject", "FindObject", "DestroyObject", "SetMeshCube",
                            "SetMaterial", "IsActionDown", "GetCamera",   "EmitParticles",
                            "PlaySound",   "Schedule",     "SendMessage", "LaunchArg",
                            "Cross",       "GetLighting",  "SetVelocity", "SetUIValue",
                            "TweenColor",  "PlayAnimation", "AddIKGoal",  "MoveCharacter"};
    for (const char* name : legacy) {
        const bool ok = se.Lua().script(std::string("return type(") + name + ") == 'function'");
        if (!ok) LOG_ERROR("Test") << "потеряно глобальное имя " << name;
        CHECK_TRUE(ok);
    }
}
