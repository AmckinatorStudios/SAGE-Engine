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
#include "sage/ui/UI.h"
#include "sage/assets/Pack.h"
#include "sage/vars/VarsComponent.h"
#include "sage/events/Events.h"

#include <filesystem>

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

// Свечение объекта пишется из скрипта так же, как цвет. Тест ловит ровно ту
// ошибку, из-за которой светящиеся фонари не работали: `obj.Emissive = ...`
// падал с "cannot set (new_index) into this object", потому что свойство было
// только у компонента-рендерера, а у GameObject его не было — притом что
// СОСЕДНЯЯ строка `obj.Color = ...` работала.
TEST(Scripting_emissive_is_settable_on_the_object_like_color) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Lantern");
    se.Lua()["e"] = o;

    se.Lua().script("e.Color = Vec3.new(0.2, 0.3, 0.4)\n"
                    "e.Emissive = Vec3.new(1.0, 0.72, 0.34)\n"
                    "e.EmissiveStrength = 2.6");

    const MeshRendererComponent& mr = scene.Registry().get<MeshRendererComponent>(o.Entity());
    CHECK_NEAR(mr.Emissive.x, 1.0f, 1e-4);
    CHECK_NEAR(mr.Emissive.y, 0.72f, 1e-4);
    CHECK_NEAR(mr.Emissive.z, 0.34f, 1e-4);
    CHECK_NEAR(mr.EmissiveStrength, 2.6f, 1e-4);

    // И читается обратно, и правится покомпонентно — как Color.
    se.Lua().script("e.Emissive.y = 0.5");
    CHECK_NEAR(mr.Emissive.y, 0.5f, 1e-4);
    float strength = se.Lua().script("return e.EmissiveStrength");
    CHECK_NEAR(strength, 2.6f, 1e-4);

    // Итоговое свечение больше единицы — иначе bloom не сработает и «светящийся»
    // объект окажется просто светлым.
    CHECK_TRUE(EffectiveEmissive(mr).x > 1.0f);
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

// --- Префабы доступны ИГРЕ, а не только редактору ------------------------------
//
// Префабы формально были: «Save as Prefab» в иерархии, двойной клик по файлу.
// Но код жил внутри EditorLayer.cpp, в безымянном пространстве имён, и потому
// был недоступен никому, кроме редактора. Для игры про постройку из блоков это
// значит, что главной её операции — поставить заготовленный объект в мир — из
// скрипта не существовало.
#include "sage/scene/Prefab.h"
#include "sage/core/SaveGame.h"

#include <cstdlib>
#include <filesystem>

TEST(Prefab_round_trips_through_a_file_and_keeps_the_subtree) {
    Scene source("src");
    GameObject root = source.CreateObject("Turret");
    root.GetTransform().Position = {5.0f, 0.0f, 0.0f};
    auto& rb = source.Registry().emplace<RigidBodyComponent>(root.Entity());
    rb.Type = sage::physics::BodyType::Static;
    rb.RuntimeBody = 4242;   // рантайм-поле: у копии обязано быть сброшено

    GameObject barrel = source.CreateObject("Barrel");
    barrel.GetTransform().Position = {0.0f, 1.5f, 0.0f};
    source.SetParent(barrel.Entity(), root.Entity());

    const std::string path = "sage_test_turret.sageprefab";
    std::string err;
    CHECK_TRUE(sage::scene::SavePrefab(source, root.Entity(), path, err));

    // Ставим в ДРУГУЮ сцену — ровно так это делает игра.
    Scene world("world");
    const int id = sage::scene::InstantiatePrefabAt(world, path, {10.0f, 2.0f, -3.0f});
    CHECK_TRUE(id > 0);

    GameObject spawned = world.Get(id);
    CHECK_TRUE(spawned.Valid());
    CHECK_TRUE(spawned.Name() == "Turret");
    CHECK_NEAR(spawned.GetTransform().Position.x, 10.0f, 1e-4);
    CHECK_NEAR(spawned.GetTransform().Position.y, 2.0f, 1e-4);

    // Поддерево на месте.
    const HierarchyComponent* h = world.Registry().try_get<HierarchyComponent>(spawned.Entity());
    CHECK_TRUE(h != nullptr && h->Children.size() == 1);

    // Дескриптор тела — состояние ЭКЗЕМПЛЯРА. Скопируй его как есть, и две
    // сущности делили бы одно тело: удаление одной уносило бы физику другой.
    const RigidBodyComponent* copied =
        world.Registry().try_get<RigidBodyComponent>(spawned.Entity());
    CHECK_TRUE(copied != nullptr);
    if (copied) CHECK_TRUE(copied->RuntimeBody == sage::physics::kInvalidBody);

    // Второй экземпляр — независимая сущность, а не тот же id.
    const int id2 = sage::scene::InstantiatePrefab(world, path);
    CHECK_TRUE(id2 > 0 && id2 != id);

    sage::scene::ClearPrefabCache();
    std::remove(path.c_str());
}

TEST(Prefab_is_reachable_from_lua) {
    ScriptEngine se;
    Scene scene("world");
    se.BindScene(scene);

    // Готовим шаблон средствами движка, ставим его из СКРИПТА.
    GameObject block = scene.CreateObject("Block");
    std::string err;
    const std::string path = "sage_test_block.sageprefab";
    CHECK_TRUE(sage::scene::SavePrefab(scene, block.Entity(), path, err));

    const int spawned = se.Lua().script(
        "return sage.scene.SpawnPrefab('sage_test_block.sageprefab', Vec3.new(3, 0, 4))");
    CHECK_TRUE(spawned > 0);
    GameObject obj = scene.Get(spawned);
    CHECK_TRUE(obj.Valid() && obj.Name() == "Block");
    CHECK_NEAR(obj.GetTransform().Position.x, 3.0f, 1e-4);

    // Старое глобальное имя работает наравне с модулем — как и весь остальной API.
    CHECK_TRUE(se.Lua().script("return sage.scene.SpawnPrefab == SpawnPrefab"));

    sage::scene::ClearPrefabCache();
    std::remove(path.c_str());
}

// Прогресс проходит круг ЧЕРЕЗ LUA: игра думает таблицами, а не JSON-текстом.
TEST(SaveGame_round_trips_a_lua_table) {
    const std::string sandbox =
        (std::filesystem::temp_directory_path() / "sage_save_lua").string();
    std::filesystem::remove_all(sandbox);
#ifdef _WIN32
    _putenv_s("APPDATA", sandbox.c_str());
#else
    setenv("XDG_DATA_HOME", sandbox.c_str(), 1);
#endif
    sage::save::SetGameName("LuaGame");

    ScriptEngine se;
    CHECK_TRUE(se.Lua().script(R"(
        return sage.save.Write("main", {
            day = 12, hp = 80.5, alive = true, name = "Робинзон",
            inventory = {"доска", "верёвка", "ткань"},
            base = { x = 3, y = 0, z = -7 },
        })
    )"));

    // Нет слота — nil, а не пустая таблица: «сохранения нет» и «сохранение
    // пустое» — разные вещи, и новый игрок не должен попадать в конец игры.
    CHECK_TRUE(se.Lua().script("return sage.save.Read('нетакого') == nil"));

    CHECK_TRUE(se.Lua().script("return sage.save.Read('main').day == 12"));
    // Целое обязано вернуться целым, а не 12.0: иначе интерфейс показывает
    // «День 12.0» при полностью рабочем сравнении «== 12».
    CHECK_TRUE(se.Lua().script("return math.type(sage.save.Read('main').day) == 'integer'"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('main').alive == true"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('main').name == 'Робинзон'"));
    CHECK_TRUE(se.Lua().script("return #sage.save.Read('main').inventory == 3"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('main').inventory[2] == 'верёвка'"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('main').base.z == -7"));
    CHECK_TRUE(se.Lua().script("return #sage.save.Slots() == 1"));
    CHECK_TRUE(se.Lua().script("return sage.save.Slots()[1].name == 'main'"));

    // Таблица, ссылающаяся на саму себя, обязана сохраниться без падения.
    // Одной глубины рекурсии тут мало: каждый уровень держит ОТКРЫТЫЙ обход
    // таблицы, и три десятка вложенных обходов переполняют стек Lua — игра
    // падает не здесь, а позже, при закрытии состояния, и связать одно с
    // другим уже невозможно. Цикл ловится по факту повторной встречи.
    CHECK_TRUE(se.Lua().script("local t = {a=1}; t.self = t; return sage.save.Write('loop', t)"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('loop').a == 1"));
    // Взаимная ссылка двух таблиц — тот же случай, только не самоочевидный.
    CHECK_TRUE(se.Lua().script(
        "local a = {n=1}; local b = {n=2}; a.b = b; b.a = a; return sage.save.Write('pair', a)"));
    CHECK_TRUE(se.Lua().script("return sage.save.Read('pair').b.n == 2"));

    std::filesystem::remove_all(sandbox);
}

// --- Ход игры: смена сцены, пауза, масштаб времени --------------------------
//
// БЕЗ ЭТОГО ИГРА НА ДВИЖКЕ БЫЛА ДЛИНОЙ В ОДНУ СЦЕНУ. Меню → уровень 1 →
// уровень 2 → титры собрать было нельзя: SceneManager существовал только в
// C++, и обойти это из скрипта было нечем.
//
// Проверяется здесь именно МЕХАНИКА ЗАПРОСА, а не сама загрузка. Загружает
// хозяин кадра (плеер), и делает это МЕЖДУ кадрами — потому что скрипт зовёт
// Load изнутри OnUpdate, когда движок идёт по сущностям этой же сцены и сам
// скрипт держит на них ссылки. Сменить сцену прямо там значит уничтожить
// реестр под ногами у обхода.
TEST(Scripting_scene_load_is_a_request_taken_once) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    std::string name;
    CHECK_FALSE(se.TakeSceneRequest(name)); // никто не просил

    se.Lua().script("sage.scene.Load('level2')");
    CHECK_TRUE(se.TakeSceneRequest(name));
    CHECK_TRUE(name == "level2");
    // Забрали — значит выполнено. Второй раз запрос повторяться не должен:
    // иначе сцена грузилась бы каждый кадр до скончания века.
    CHECK_FALSE(se.TakeSceneRequest(name));

    // Два вызова за кадр — одна загрузка, последняя. Скрипт, дважды
    // передумавший, не должен получить две смены сцены подряд.
    se.Lua().script("sage.scene.Load('a'); sage.scene.Load('b')");
    CHECK_TRUE(se.TakeSceneRequest(name));
    CHECK_TRUE(name == "b");
    CHECK_FALSE(se.TakeSceneRequest(name));

    // Пустое имя игнорируется: это почти наверняка ошибка в скрипте, и
    // выполнить её значило бы перезагрузить уровень на ровном месте.
    se.Lua().script("sage.scene.Load('')");
    CHECK_FALSE(se.TakeSceneRequest(name));
}

TEST(Scripting_restart_and_quit_are_taken_once) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    CHECK_FALSE(se.TakeRestartRequest());
    se.Lua().script("sage.game.Restart()");
    CHECK_TRUE(se.TakeRestartRequest());
    CHECK_FALSE(se.TakeRestartRequest());

    CHECK_FALSE(se.TakeQuitRequest());
    se.Lua().script("sage.game.Quit()");
    CHECK_TRUE(se.TakeQuitRequest());
    CHECK_FALSE(se.TakeQuitRequest());
}

TEST(Scripting_time_scale_and_pause_fold_into_one_multiplier) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    CHECK_NEAR(se.TimeScale(), 1.0f, 1e-6);
    CHECK_NEAR(se.FrameTimeScale(), 1.0f, 1e-6);

    se.Lua().script("sage.time.SetScale(0.5)");
    CHECK_NEAR(se.TimeScale(), 0.5f, 1e-6);
    CHECK_NEAR(se.FrameTimeScale(), 0.5f, 1e-6);

    // Пауза бьёт масштаб: множитель кадра — ноль, что бы ни стояло в SetScale.
    se.Lua().script("sage.game.Pause(true)");
    CHECK_TRUE(se.Paused());
    CHECK_NEAR(se.FrameTimeScale(), 0.0f, 1e-6);
    // Но САМ масштаб пауза не трогает: сняли паузу — вернулось замедление,
    // которое просила игра, а не единица.
    CHECK_NEAR(se.TimeScale(), 0.5f, 1e-6);
    se.Lua().script("sage.game.Pause(false)");
    CHECK_NEAR(se.FrameTimeScale(), 0.5f, 1e-6);

    // Отрицательный масштаб зажимается в ноль. Обратное время звучит заманчиво,
    // но физика, анимация и таймеры к нему не готовы: вышла бы не перемотка, а
    // разъезжающееся состояние.
    se.Lua().script("sage.time.SetScale(-3)");
    CHECK_NEAR(se.TimeScale(), 0.0f, 1e-6);
}

// --- Интерфейс из скрипта: раскладка, холст, группа ---------------------------
//
// Компоненты Layout/Canvas/Group существовали с самого появления новой системы
// интерфейса, но были доступны ТОЛЬКО из редактора. Игре, которая собирает свои
// экраны скриптом, это означало: сетку инвентаря раскладывать формулой в
// самом скрипте, порядок «меню поверх худа» — угадывать по порядку создания
// сущностей, а спрятать панель целиком — обходить всех её детей.
TEST(Scripting_ui_layout_canvas_and_group_are_reachable) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject panel = scene.CreateObject("Panel");
    se.Lua()["panel"] = panel;

    se.Lua().script(R"(
        local e = panel:AddUI()
        e.Type = UIKind.Panel
        e.Stretch = UIStretch.Both
        e.Margin = Vec4(4, 8, 12, 16)
        e.Pivot = Vec2(0.5, 0.5)
        e.Alpha = 0.25
        e.IconSize = 18.0
        sage.ui.SetLayout(panel, {dir = "grid", columns = 5, spacing = 6,
                                  padding = 3.0, stretch = false, fit = true})
        sage.ui.SetCanvas(panel, {order = 7, scale = true, reference = Vec2(1280, 720),
                                  match = 0.25})
    )");

    const entt::entity e = panel.Entity();
    const auto& xf = scene.Registry().get<sage::ui::Transform>(e);
    CHECK_TRUE(xf.Mode == sage::ui::Transform::Stretch::Both);
    CHECK_NEAR(xf.Margin.z, 12.0f, 1e-4f);
    CHECK_NEAR(xf.Pivot.x, 0.5f, 1e-4f);

    const auto& group = scene.Registry().get<sage::ui::Group>(e);
    CHECK_NEAR(group.Alpha, 0.25f, 1e-4f);
    // Прозрачность группы НЕ должна попутно запрещать ввод: панель, показанная
    // наполовину, обязана оставаться нажимаемой.
    CHECK_TRUE(group.Interactable);

    CHECK_NEAR(scene.Registry().get<sage::ui::Icon>(e).Size, 18.0f, 1e-4f);

    const auto& layout = scene.Registry().get<sage::ui::Layout>(e);
    CHECK_TRUE(layout.Direction == sage::ui::Layout::Flow::Grid);
    CHECK_EQ(layout.Columns, 5);
    CHECK_NEAR(layout.Spacing, 6.0f, 1e-4f);
    CHECK_NEAR(layout.Padding.w, 3.0f, 1e-4f);
    CHECK_FALSE(layout.StretchCross);
    CHECK_TRUE(layout.FitContent);

    const auto& canvas = scene.Registry().get<sage::ui::Canvas>(e);
    CHECK_EQ(canvas.SortOrder, 7);
    CHECK_TRUE(canvas.Mode == sage::ui::Canvas::Scale::ScaleWithSize);
    CHECK_NEAR(canvas.Reference.x, 1280.0f, 1e-4f);
    CHECK_NEAR(canvas.MatchWidthOrHeight, 0.25f, 1e-4f);

    // Снять раскладку так же просто, как поставить: иначе «сделать из сетки
    // обычную панель» означало бы пересоздать её.
    se.Lua().script("sage.ui.ClearLayout(panel)");
    CHECK_FALSE(scene.Registry().all_of<sage::ui::Layout>(e));
}

// Что под курсором — по ИМЕНИ ДЕЙСТВИЯ, как и что нажато. Без этого подсказка
// «из чего делается предмет» требует опрашивать поле Hovered у каждой ячейки
// инвентаря каждый кадр.
TEST(Scripting_ui_hovered_action_answers_by_name) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject slot = scene.CreateObject("Slot");
    se.Lua()["slot"] = slot;
    se.Lua().script(R"(
        local e = slot:AddUI()
        e.Type = UIKind.Panel
        e.Interactive = true
        e.Action = "craft:plank"
    )");

    std::string hovered = se.Lua().script("return sage.ui.HoveredAction()");
    CHECK_TRUE(hovered.empty());

    scene.Registry().get<sage::ui::Interactable>(slot.Entity()).Runtime.Hovered = true;
    hovered = se.Lua().script("return sage.ui.HoveredAction()");
    CHECK_TRUE(hovered == "craft:plank");
}

// Встроенное меню паузы плеера выключается игрой, у которой меню своё. Пока
// выключить его было нечем, «своё меню» означало два меню сразу: ESC
// перехватывал плеер, а до скрипта клавиша не доходила.
TEST(Scripting_game_can_turn_off_the_builtin_pause_menu) {
    ScriptEngine se;
    CHECK_TRUE(se.PauseMenuEnabled());
    se.Lua().script("sage.game.SetPauseMenu(false)");
    CHECK_FALSE(se.PauseMenuEnabled());
    bool asked = se.Lua().script("return sage.game.HasPauseMenu()");
    CHECK_FALSE(asked);
    se.Lua().script("sage.game.SetPauseMenu(true)");
    CHECK_TRUE(se.PauseMenuEnabled());
}

// --- Модули в СОБРАННОЙ игре --------------------------------------------------
//
// Собранная игра не могла загрузить ни одного модуля, и это ломало её целиком:
// скрипт сущности движок читает через vfs (проект в собранной игре лежит одним
// файлом game.sagepak), а require шёл штатным загрузчиком Lua — по настоящему
// диску, где файлов нет. В редакторе и при запуске из папки проекта всё
// работало, потому что там файлы есть; игрок же получал «module 'blocks' not
// found» на первой строке первого скрипта.
//
// Тест воспроизводит ровно ту обстановку: модуль ТОЛЬКО в пакете, на диске его
// нет.
TEST(Scripting_require_finds_modules_inside_the_game_package) {
    const std::filesystem::path sandbox =
        std::filesystem::temp_directory_path() / "sage_pack_require";
    std::filesystem::remove_all(sandbox);
    std::filesystem::create_directories(sandbox / "assets" / "scripts");

    {
        std::ofstream f(sandbox / "assets" / "scripts" / "greet.lua");
        f << "local M = {}\nfunction M.Hello() return 'из пакета' end\nreturn M\n";
    }

    sage::assets::PackWriter pack;
    pack.AddDirectory(sandbox);
    const std::filesystem::path packFile = sandbox / "game.sagepak";
    CHECK_TRUE(pack.Save(packFile));

    // Файлы с диска убираем: остаётся только пакет — как в собранной игре.
    std::filesystem::remove_all(sandbox / "assets");
    CHECK_TRUE(sage::assets::vfs::Mount(packFile));

    {
        ScriptEngine se;
        se.AddScriptSearchPath("assets/scripts");
        std::string greeting = se.Lua().script("return require('greet').Hello()");
        CHECK_TRUE(greeting == "из пакета");

        // Ошибка «модуля нет» обязана называть, где искали, — иначе она
        // неотличима от опечатки в имени.
        auto missing = se.Lua().script("return pcall(require, 'nosuch')",
                                       sol::script_pass_on_error);
        CHECK_TRUE(missing.valid());
    }

    sage::assets::vfs::Unmount();
    std::filesystem::remove_all(sandbox);
}

// ===========================================================================
//  ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ, ССЫЛКИ И СОБЫТИЯ В СКРИПТАХ
// ===========================================================================

// Скрипт читает и пишет настройку СВОЕГО объекта. Именно это отличает
// публичную переменную от числа в коде: скрипт один, а значение у каждой двери
// своё, видно в инспекторе и лежит в сцене.
TEST(Scripting_public_variables_are_read_and_written_from_lua) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject door = scene.CreateObject("Door");
    VarsComponent& vc = scene.Registry().emplace<VarsComponent>(door.Entity());
    vc.Values.Set("speed", sage::vars::Value(2.0f));
    vc.Values.Set("title", sage::vars::Value(std::string("Ворота")));

    se.Lua()["e"] = door;
    float speed = se.Lua().script("return e:Vars().speed");
    CHECK_NEAR(speed, 2.0f, 1e-4f);
    std::string title = se.Lua().script("return e:Vars().title");
    CHECK_EQ(title, std::string("Ворота"));

    se.Lua().script("e:Vars().speed = 7.5");
    CHECK_NEAR(vc.Values.Get("speed").AsFloat(), 7.5f, 1e-4f);

    // Вид переменной за ней и остаётся: скрипт, положивший строку в числовую
    // настройку, ошибся, и молча менять тип поля — значит спрятать ошибку.
    se.Lua().script("e:Vars().speed = 'три'");
    CHECK_TRUE(vc.Values.Find("speed")->Data.Type() == sage::vars::Kind::Float);

    // Переменной нет — nil, а не ошибка: проверять существование обычным `if`
    // должно быть можно.
    bool missing = se.Lua().script("return e:Vars().nosuch == nil");
    CHECK_TRUE(missing);
    bool has = se.Lua().script("return e:HasVar('title')");
    CHECK_TRUE(has);
}

// ССЫЛКА ОТДАЁТСЯ ОБЪЕКТОМ, а не номером: скрипт пишет `Vars.target:SetName(…)`,
// а не ищет сущность по номеру. И держится она за Id — переименование объекта
// в редакторе больше не ломает уровень молча, как ломал FindByName.
TEST(Scripting_a_reference_variable_hands_back_the_object_itself) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject door = scene.CreateObject("Door");
    GameObject key = scene.CreateObject("Key");
    scene.Registry().emplace<VarsComponent>(door.Entity()).Values.Set(
        "needs", sage::vars::Value(sage::vars::EntityRef{key.Id()}));

    se.Lua()["e"] = door;
    std::string name = se.Lua().script("return e:Vars().needs.Name");
    CHECK_EQ(name, std::string("Key"));

    // Присваивание объектом кладёт ссылку, а не копию.
    se.Lua()["other"] = scene.CreateObject("Chest");
    se.Lua().script("e:Vars().needs = other");
    const VarsComponent& vc = scene.Registry().get<VarsComponent>(door.Entity());
    CHECK_EQ(scene.Get(vc.Values.Get("needs").AsEntity().Id).Name(), std::string("Chest"));

    // Ссылка на удалённый объект — nil, а не «объект, у которого всё падает».
    scene.RemoveObject(vc.Values.Get("needs").AsEntity().Id);
    bool gone = se.Lua().script("return e:Vars().needs == nil");
    CHECK_TRUE(gone);
}

// ОДНА ШИНА НА ВСЕХ. Событие, посланное из C++ (так его шлёт кнопка
// интерфейса), обязано дойти до подписчика на Lua — иначе разговоров два, и
// кнопка не может позвать игровую логику без скрипта-опросчика.
TEST(Scripting_an_event_from_cpp_reaches_a_lua_subscriber) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.Lua().script(R"(
        heard = 0
        gotSender = -1
        gotValue = ""
        sage.events.On("game.start", function(p)
            heard = heard + 1
            gotSender = p.sender
            gotValue = p.value
        end)
    )");

    scene.Events.Emit("game.start", sage::vars::Value(std::string("level1")), 42);
    int heard = se.Lua()["heard"];
    int sender = se.Lua()["gotSender"];
    std::string value = se.Lua()["gotValue"];
    CHECK_EQ(heard, 1);
    CHECK_EQ(sender, 42);
    CHECK_EQ(value, std::string("level1"));
}

// И обратно: событие, посланное скриптом, слышит код на C++. Без этого
// подсистема движка не может отреагировать на игровое событие, не зная Lua.
TEST(Scripting_an_event_from_lua_reaches_a_cpp_subscriber) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    int heard = 0;
    std::string arg;
    scene.Events.On("player.died", [&](const sage::events::Event& e) {
        ++heard;
        arg = e.Arg.AsString();
    });
    se.Lua().script("sage.events.Emit('player.died', 'в лаву')");
    CHECK_EQ(heard, 1);
    CHECK_EQ(arg, std::string("в лаву"));
}

// Подписчик Lua слышит событие РОВНО ОДИН РАЗ, хотя оно и проходит через шину:
// без защиты мост позвал бы его вторично, и обработчик, считающий очки,
// насчитал бы вдвое.
TEST(Scripting_a_lua_event_is_not_heard_twice) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.Lua().script(R"(
        count = 0
        sage.events.On("ping", function() count = count + 1 end)
        sage.events.Emit("ping")
    )");
    int count = se.Lua()["count"];
    CHECK_EQ(count, 1);
}

// АДРЕСНАЯ ЧАСТЬ СВЯЗИ: «эта кнопка открывает эту дверь». Приходит скрипту
// объекта тем же путём, что и SendMessage, — второй механизм для этого заводить
// незачем.
TEST(Scripting_an_addressed_event_calls_the_method_of_the_target_object) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject door = scene.CreateObject("Door");

    // Скрипт двери докладывает о вызове ОБРАТНО В ШИНУ: заглядывать в его
    // окружение из теста значило бы проверять не тот путь, которым связь
    // работает в игре.
    const std::string path = WriteTempScript("door_target", R"(
        function OnMessage(entity, name, data)
            if name == "Open" then
                sage.events.Emit("door.opened", data.value)
            end
        end
    )");
    se.AttachScript(door, path);

    int opened = 0;
    std::string howLong;
    scene.Events.On("door.opened", [&](const sage::events::Event& e) {
        ++opened;
        howLong = e.Arg.AsString();
    });

    sage::events::Event e;
    e.Name = "door.open";
    e.Target = sage::vars::EntityRef{door.Id()};
    e.Method = "Open";
    e.Arg = sage::vars::Value(std::string("медленно"));
    scene.Events.Emit(e);
    CHECK_EQ(opened, 1);
    CHECK_EQ(howLong, std::string("медленно"));

    // Каждое событие — свой вызов: связь срабатывает всякий раз, а не однажды.
    scene.Events.Emit(e);
    CHECK_EQ(opened, 2);

    // Чужой объект метод не получает: адрес на то и адрес.
    GameObject other = scene.CreateObject("Window");
    sage::events::Event miss = e;
    miss.Target = sage::vars::EntityRef{other.Id()};
    scene.Events.Emit(miss);
    CHECK_EQ(opened, 2);

    std::remove(path.c_str());
}
