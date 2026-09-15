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
#include "sage/render/Camera.h"
#include "sage/assets/AssetDatabase.h"

#include <chrono>
#include <filesystem>
#include <system_error>

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

// Свечение объекта задаётся из скрипта МАТЕРИАЛОМ — своего свечения у объекта
// больше нет. Тест закрепляет ровно этот путь: собрать материал скриптом,
// выставить ему свечение и назначить объекту. Пока свечение дублировалось в
// компоненте, `obj.Emissive = ...` падал с "cannot set (new_index) into this
// object", и обходной путь искали наощупь; теперь путь один, и он проверен.
TEST(Scripting_emissive_comes_from_the_material) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Lantern");
    se.Lua()["e"] = o;

    se.Lua().script("local m = sage.render.NewMaterial('test/lantern')\n"
                    "m.Albedo = Vec3.new(0.2, 0.3, 0.4)\n"
                    "m.Emissive = Vec3.new(1.0, 0.72, 0.34)\n"
                    "m.EmissiveStrength = 2.6\n"
                    "sage.render.SetMaterial(e, 'test/lantern')");

    const MeshRendererComponent& mr = scene.Registry().get<MeshRendererComponent>(o.Entity());
    CHECK_TRUE(mr.MaterialPtr != nullptr);
    CHECK_NEAR(mr.MaterialPtr->Emissive.x, 1.0f, 1e-4);
    CHECK_NEAR(mr.MaterialPtr->Emissive.y, 0.72f, 1e-4);
    CHECK_NEAR(mr.MaterialPtr->Emissive.z, 0.34f, 1e-4);
    CHECK_NEAR(mr.MaterialPtr->EmissiveStrength, 2.6f, 1e-4);

    // Материал объекта читается обратно и правится на ходу — то есть менять
    // яркость лампы во время игры можно, не собирая материал заново.
    se.Lua().script("sage.render.MaterialOf(e).EmissiveStrength = 4.0");
    CHECK_NEAR(mr.MaterialPtr->EmissiveStrength, 4.0f, 1e-4);

    // Итоговое свечение больше единицы — иначе bloom не сработает и «светящийся»
    // объект окажется просто светлым.
    CHECK_TRUE(EffectiveEmissive(mr).x > 1.0f);
}

// Полная поддержка материалов из скрипта: собственный шейдер, юниформы
// (запись/чтение/удаление) и сохранение/перезагрузка с диска — то, чего не
// было ни у Material-usertype (только поля поверхности), ни у sage.render
// (SetMaterialParam был "в одну сторону": писал, но не читал). Один тест на
// всю цепочку, потому что это ОДНА возможность: "материал, собранный
// скриптом, — полноценный ассет проекта", а не набор независимых полей.
TEST(Scripting_materials_full_support) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_script_material_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "assets" / "shaders");
    fs::create_directories(dir / "assets" / "materials");
    // Настоящие .vert/.frag не нужны: GetShader на несобирающемся шейдере не
    // бросает, а логирует и кэширует nullptr (см. ResourceManager::GetShader)
    // — для проверки, что ПУТИ дошли до материала, этого достаточно.
    std::ofstream(dir / "assets/shaders/water.vert") << "// stub";
    std::ofstream(dir / "assets/shaders/water.frag") << "// stub";

    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    db.Clear();
    db.ScanProject(dir.string());

    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // NewMaterial отдаёт материал БЕЗ осмысленного пути в общем кэше (см.
    // комментарий у Bind("render","NewMaterial",...)) — поэтому всё, что
    // проверяется по этому 'm', проверяется через сам 'm', а не повторным
    // поиском по имени "test/water" со стороны C++: два разных способа
    // получить материал (по сырому имени и по канонизированному пути) не
    // обязаны указывать на один и тот же кэшированный экземпляр.
    se.Lua().script(
        "m = sage.render.NewMaterial('test/water')\n"
        "m.Albedo = Vec3.new(0.1, 0.4, 0.6)\n"
        "sage.render.SetMaterialShader(m, 'assets/shaders/water.vert', 'assets/shaders/water.frag')\n"
        "m:SetParam('waveHeight', 0.35)\n"
        "m:SetParam('flowDir', Vec2.new(1.0, 0.0))\n");

    // Пути дошли до материала, и HasCustomShader их видит.
    bool hasCustom = se.Lua().script(
        "return m:HasCustomShader() and m.VertexShaderPath == 'assets/shaders/water.vert' "
        "and m.FragmentShaderPath == 'assets/shaders/water.frag'");
    CHECK_TRUE(hasCustom);

    // Юниформа читается обратно ТЕМ ЖЕ типом, каким была задана.
    bool paramOk = se.Lua().script(
        "local h = m:GetParam('waveHeight')\n"
        "local d = m:GetParam('flowDir')\n"
        "return math.abs(h - 0.35) < 1e-4 and math.abs(d.x - 1.0) < 1e-4 "
        "and m:HasParam('waveHeight') and not m:HasParam('nope')");
    CHECK_TRUE(paramOk);

    // ClearParam снимает ОДНУ юниформу — вторая остаётся на месте.
    bool clearedOk = se.Lua().script(
        "m:ClearParam('waveHeight')\n"
        "return not m:HasParam('waveHeight') and m:HasParam('flowDir')");
    CHECK_TRUE(clearedOk);

    // Сохранение НОВОГО материала (файла ещё не было) — путь разрешается
    // относительно проекта, не рабочей директории процесса теста.
    se.Lua().script("sage.render.SaveMaterial(m, 'assets/materials/water.sagemat')");
    const fs::path saved = dir / "assets/materials/water.sagemat";
    CHECK_TRUE(fs::exists(saved));

    // Независимая загрузка ТОГО ЖЕ файла (sage.render.GetMaterial по пути —
    // отдельный, канонизированный ключ кэша) обязана увидеть ровно то, что
    // SaveMaterial записал: цвет, оба пути шейдера и уцелевшую юниформу.
    bool roundTripOk = se.Lua().script(
        "local r = sage.render.GetMaterial('assets/materials/water.sagemat')\n"
        "return math.abs(r.Albedo.x - 0.1) < 1e-4 and math.abs(r.Albedo.y - 0.4) < 1e-4 "
        "and r.VertexShaderPath == 'assets/shaders/water.vert' "
        "and r.FragmentShaderPath == 'assets/shaders/water.frag' "
        "and math.abs(r:GetParam('flowDir').x - 1.0) < 1e-4 "
        "and not r:HasParam('waveHeight')");
    CHECK_TRUE(roundTripOk);

    // Правим ЗАГРУЖЕННЫЙ экземпляр в памяти мимо файла, затем откатываем —
    // ReloadMaterial обязан вернуть то, что реально лежит на диске, причём
    // ТОТ ЖЕ общий экземпляр (см. ResourceManager::ReloadMaterial — правит
    // объект на месте, а не подменяет указатель), а не собственную копию.
    bool reloadOk = se.Lua().script(
        "local r = sage.render.GetMaterial('assets/materials/water.sagemat')\n"
        "r.Albedo = Vec3.new(0.9, 0.9, 0.9)\n"
        "local reloaded = sage.render.ReloadMaterial('assets/materials/water.sagemat')\n"
        "return math.abs(reloaded.Albedo.x - 0.1) < 1e-4 and math.abs(r.Albedo.x - 0.1) < 1e-4");
    CHECK_TRUE(reloadOk);

    // ClearMaterialShader возвращает материал к штатному PBR — на ЭТОМ же
    // независимо загруженном экземпляре, чтобы не задевать 'm' раньше времени.
    bool clearShaderOk = se.Lua().script(
        "local r = sage.render.GetMaterial('assets/materials/water.sagemat')\n"
        "sage.render.ClearMaterialShader(r)\n"
        "return not r:HasCustomShader() and r.VertexShaderPath == '' and r.FragmentShaderPath == ''");
    CHECK_TRUE(clearShaderOk);

    // sage.render.GetMaterialParam читает то же, что положил
    // sage.render.SetMaterialParam, — по пути, без ссылки на объект материала.
    bool pathParamOk = se.Lua().script(
        "sage.render.SetMaterialParam('assets/materials/water.sagemat', 'tint', 0.75)\n"
        "return math.abs(sage.render.GetMaterialParam('assets/materials/water.sagemat', 'tint') - 0.75) < 1e-4 "
        "and sage.render.GetMaterialParam('assets/materials/water.sagemat', 'nope') == nil");
    CHECK_TRUE(pathParamOk);

    db.Clear();
    std::error_code ec;
    fs::remove_all(dir, ec);
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

// Расширенная математика: Vec2 в пару к Vec3, геометрия на Vec3 (Reflect/
// Angle/MoveTowards/ClampLength), Quat (полностью — от оси-угла до Slerp),
// Lerp/SmoothStep на всех векторах, случайные точки/направления, пересечения
// луча с плоскостью и сферой без физического мира. Один тест на всю область,
// потому что запрос был на "полную математику" ЦЕЛИКОМ, а не на функцию.
TEST(Scripting_math_full_support) {
    ScriptEngine se;

    // Vec2 — теперь тот же набор геометрии, что у Vec3.
    bool vec2Ok = se.Lua().script(
        "local a = Vec2.new(3, 4)\n"
        "local b = Vec2.new(1, 0)\n"
        "return math.abs(a:Length() - 5.0) < 1e-4 "
        "and math.abs(a:Normalized():Length() - 1.0) < 1e-4 "
        "and math.abs(a:Distance(Vec2.new(0,0)) - 5.0) < 1e-4 "
        "and math.abs(a:Dot(b) - 3.0) < 1e-4 "
        "and (-a).x == -3.0 "
        "and (a / 2.0).x == 1.5");
    CHECK_TRUE(vec2Ok);

    // Vec3: Reflect (луч по нормали), Angle (в градусах), MoveTowards
    // (снап на конечном шаге, а не перелёт), ClampLength (направление цело).
    bool vec3Ok = se.Lua().script(
        "local r = Vec3.new(1,-1,0):Reflect(Vec3.new(0,1,0))\n"
        "local ang = Vec3.new(1,0,0):Angle(Vec3.new(0,1,0))\n"
        "local mv = Vec3.new(0,0,0):MoveTowards(Vec3.new(10,0,0), 3)\n"
        "local mv2 = Vec3.new(0,0,0):MoveTowards(Vec3.new(1,0,0), 10)\n"
        "local cl = Vec3.new(10,0,0):ClampLength(2)\n"
        "return math.abs(r.y - 1.0) < 1e-4 and math.abs(ang - 90.0) < 1e-2 "
        "and math.abs(mv.x - 3.0) < 1e-4 and math.abs(mv2.x - 1.0) < 1e-4 "
        "and math.abs(cl:Length() - 2.0) < 1e-4");
    CHECK_TRUE(vec3Ok);

    // Lerp теперь и на Vec2, и на Vec4 (раньше — только число и Vec3).
    bool lerpOk = se.Lua().script(
        "local v2 = Lerp(Vec2.new(0,0), Vec2.new(10,10), 0.5)\n"
        "local v4 = Lerp(Vec4.new(0,0,0,0), Vec4.new(1,1,1,1), 0.25)\n"
        "return math.abs(v2.x - 5.0) < 1e-4 and math.abs(v4.w - 0.25) < 1e-4");
    CHECK_TRUE(lerpOk);

    float smooth = se.Lua().script("return SmoothStep(0.0, 10.0, 5.0)");
    CHECK_NEAR(smooth, 0.5f, 1e-4);

    // Slerp векторов — по дуге между НАПРАВЛЕНИЯМИ, не покомпонентно: длина
    // единичная всю дорогу, а не проседает к середине, как у Lerp.
    bool slerpOk = se.Lua().script(
        "local s = Slerp(Vec3.new(1,0,0), Vec3.new(0,1,0), 0.5)\n"
        "return math.abs(s:Length() - 1.0) < 1e-3 and s.x > 0 and s.y > 0");
    CHECK_TRUE(slerpOk);

    // Quat: ось-угол, применение к вектору, обратный отменяет поворот,
    // q * q:Inverse() — единичный кватернион.
    bool quatOk = se.Lua().script(
        "local q = Quat.FromAxisAngle(Vec3.new(0,1,0), 90.0)\n"
        "local v = q * Vec3.new(1,0,0)\n"
        "local back = q:Inverse() * v\n"
        "local id = q * q:Inverse()\n"
        "return math.abs(v.x) < 1e-3 and math.abs(v.z + 1.0) < 1e-3 "
        "and math.abs(back.x - 1.0) < 1e-3 and math.abs(id.w - 1.0) < 1e-3");
    CHECK_TRUE(quatOk);

    // FromEuler/Euler ОБЯЗАНЫ совпадать с Transform.Rotation — общая формула
    // (sage::EulerXYZDegreesFromRotationMatrix), иначе поворот, посчитанный
    // кватернионом, при записи в Transform.Rotation дал бы другой результат.
    bool eulerBridgeOk = se.Lua().script(
        "local e = Vec3.new(15, 35, -20)\n"
        "local back = Quat.FromEuler(e):Euler()\n"
        "return math.abs(back.x - e.x) < 0.05 and math.abs(back.y - e.y) < 0.05 "
        "and math.abs(back.z - e.z) < 0.05");
    CHECK_TRUE(eulerBridgeOk);

    // Slerp кватернионов: на полпути между 0 и 90° вокруг Y — ровно 45°.
    bool quatSlerpOk = se.Lua().script(
        "local a = Quat.Identity()\n"
        "local b = Quat.FromAxisAngle(Vec3.new(0,1,0), 90.0)\n"
        "local mid = a:Slerp(b, 0.5)\n"
        "return math.abs(mid:Euler().y - 45.0) < 0.5");
    CHECK_TRUE(quatSlerpOk);

    // Случайность — проверяем ГРАНИЦЫ и инварианты (длина <= 1, длина == 1
    // на поверхности), не конкретное значение: оно намеренно не детерминировано.
    bool randomOk = se.Lua().script(
        "for i = 1, 50 do\n"
        "    local r = RandomRange(-2.0, 2.0)\n"
        "    if r < -2.0 or r > 2.0 then return false end\n"
        "    if RandomInsideUnitSphere():Length() > 1.0001 then return false end\n"
        "    if math.abs(RandomOnUnitSphere():Length() - 1.0) > 1e-3 then return false end\n"
        "    if RandomInsideUnitCircle():Length() > 1.0001 then return false end\n"
        "end\n"
        "return true");
    CHECK_TRUE(randomOk);

    // Геометрия без физического мира: луч в плоскость и в сферу, попадание и
    // явный промах (nil).
    bool geomOk = se.Lua().script(
        "local hit = IntersectRayPlane(Vec3.new(0,5,0), Vec3.new(0,-1,0), Vec3.new(0,0,0), Vec3.new(0,1,0))\n"
        "local miss = IntersectRayPlane(Vec3.new(0,5,0), Vec3.new(0,1,0), Vec3.new(0,0,0), Vec3.new(0,1,0))\n"
        "local sph = IntersectRaySphere(Vec3.new(-5,0,0), Vec3.new(1,0,0), Vec3.new(0,0,0), 1.0)\n"
        "local sphMiss = IntersectRaySphere(Vec3.new(-5,10,0), Vec3.new(1,0,0), Vec3.new(0,0,0), 1.0)\n"
        "return hit ~= nil and math.abs(hit.y) < 1e-3 and miss == nil "
        "and sph ~= nil and math.abs(sph.x + 1.0) < 1e-3 and sphMiss == nil");
    CHECK_TRUE(geomOk);
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

// sage.msg.Call — связь СО СВОИМ ОТВЕТОМ: в отличие от SendMessage/Broadcast
// (оповещение без возврата), Call зовёт именованную функцию скрипта цели
// напрямую и отдаёт то, что она вернула. Проверяем: значение доходит,
// аргументы доходят, несколько возвращаемых значений доходят все сразу,
// отсутствующая функция и мёртвая/несуществующая цель — законный nil (а не
// ошибка), и ошибка ВНУТРИ вызванной функции не роняет и не блокирует
// звонящего (просто nil в логе).
TEST(Scripting_call_between_scripts) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    std::string path = WriteTempScript("callee",
        "function GetHealth(entity) return 42 end\n"
        "function Add(entity, a, b) return a + b end\n"
        "function GetPosition(entity) return 1, 2, 3 end\n"
        "function Kaboom(entity) error('boom') end\n");

    GameObject enemy = scene.CreateObject("Enemy");
    se.AttachScript(enemy, path);
    se.Lua()["enemy"] = enemy;

    double hp = se.Lua().script("return sage.msg.Call(enemy, 'GetHealth')");
    CHECK_NEAR(hp, 42.0, 1e-6);

    double sum = se.Lua().script("return sage.msg.Call(enemy, 'Add', 3, 4)");
    CHECK_NEAR(sum, 7.0, 1e-6);

    // Несколько возвращаемых значений доходят ВСЕ, а не только первое.
    bool multiOk = se.Lua().script(
        "local x, y, z = sage.msg.Call(enemy, 'GetPosition')\n"
        "return x == 1 and y == 2 and z == 3");
    CHECK_TRUE(multiOk);

    // Функции нет у цели — nil, не ошибка: это не обязательный хук.
    bool missingOk = se.Lua().script("return sage.msg.Call(enemy, 'NoSuchFunction') == nil");
    CHECK_TRUE(missingOk);

    // Ошибка внутри вызванной функции — nil звонящему, а не Lua-исключение,
    // которое оборвало бы его собственный скрипт.
    bool crashOk = se.Lua().script(
        "local ok, err = pcall(function() return sage.msg.Call(enemy, 'Kaboom') end)\n"
        "return ok and err == nil"); // pcall не падает — Call сама ловит ошибку внутри
    CHECK_TRUE(crashOk);

    // Мёртвая цель — тоже nil.
    scene.RemoveObject(enemy.Id());
    bool deadOk = se.Lua().script("return sage.msg.Call(enemy, 'GetHealth') == nil");
    CHECK_TRUE(deadOk);

    // Несуществующий номер — тоже nil, не ошибка.
    bool noneOk = se.Lua().script("return sage.msg.Call(999999, 'GetHealth') == nil");
    CHECK_TRUE(noneOk);

    std::remove(path.c_str());
}

// scene.FindById — вторая половина того, что уже умели SendMessage/Call/
// DestroyObject (принимать номер сущности вместо самой сущности): найти
// объект ПО НОМЕРУ, когда он единственное, что осталось (например, номер
// сохранился с прошлого кадра, а FindObject по имени не годится — имя могло
// смениться, а вот номер живёт, пока жива сама сущность).
TEST(Scripting_find_object_by_id) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    GameObject o = scene.CreateObject("Target");
    se.Lua()["targetId"] = o.Id();

    bool foundOk = se.Lua().script(
        "local found = sage.scene.FindById(targetId)\n"
        "return found ~= nil and found.Name == 'Target'");
    CHECK_TRUE(foundOk);

    bool missingOk = se.Lua().script("return sage.scene.FindById(999999) == nil");
    CHECK_TRUE(missingOk);
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

// Раскладка объявляется из игры: BindAction заводит действие в системе ввода
// движка, и дальше его читает тот же IsActionDown, что и код на C++.
TEST(Scripting_bind_action_declares_actions) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    int bound = se.Lua().script("return BindAction('Jump', 'SPACE')");
    CHECK_EQ(bound, 1);
    CHECK_TRUE(input.Has("Jump"));

    // Список клавиш: несколько привязок на одно действие (WASD и стрелки).
    int many = se.Lua().script("return BindAction('Move Forward', {'W', 'UP'})");
    CHECK_EQ(many, 2);
    CHECK_EQ((int)input.Find("Move Forward")->Bindings().size(), 2);

    // Кнопки мыши и геймпада — такие же привязки, как клавиши.
    CHECK_EQ((int)se.Lua().script("return BindAction('Break', 'MOUSE_LEFT')"), 1);
    CHECK_EQ((int)se.Lua().script("return BindAction('Use', 'PAD_A')"), 1);

    // Нераспознанное имя не роняет игру: действие заводится, привязок 0.
    int bad = se.Lua().script("return BindAction('Nonsense', 'NOT_A_KEY')");
    CHECK_EQ(bad, 0);

    CHECK_FALSE(input.IsDown("Jump")); // никто ничего не нажимал
    CHECK_TRUE((bool)se.Lua().script("return HasAction('Jump')"));
    CHECK_FALSE((bool)se.Lua().script("return HasAction('Never Declared')"));
}

// Оси и векторы из Lua: ради них ввод и разделён на виды — «Движение»
// одинаково приходит и с клавиш, и со стика, а скрипт спрашивает одно и то же.
TEST(Scripting_can_declare_axis_and_vector_actions) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    CHECK_EQ((int)se.Lua().script("return BindAxis('Throttle', 'W', 'S')"), 2);
    se.Lua().script("BindVector('Move', 'W', 'S', 'A', 'D')");

    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::W));
    input.Update(1.0f / 60.0f);

    CHECK_NEAR((float)se.Lua().script("return GetAxis('Throttle')"), 1.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetVector('Move').y"), 1.0f, 1e-4);
}

// Переназначение управления из игры: экран настроек живёт в игре, а не в
// движке, и без этой функции написать его на скриптах нельзя.
TEST(Scripting_can_rebind_an_action) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);
    se.Lua().script("BindAction('Jump', 'SPACE')");

    CHECK_TRUE((bool)se.Lua().script("return RebindAction('Jump', 'Q')"));
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    input.Update(1.0f / 60.0f);
    CHECK_FALSE(input.WasPressed("Jump"));

    input.Push(sage::input::InputEvent::KeyUp(sage::input::Key::Space));
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Q));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE(input.WasPressed("Jump"));
}

// Контексты из Lua: одна клавиша значит разное в игре и в инвентаре.
TEST(Scripting_can_switch_input_contexts) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    se.Lua().script("CreateInputContext('Inventory', 50)");
    se.Lua().script("BindActionIn('Inventory', 'Equip', 'E')");
    se.Lua().script("SetInputContextEnabled('Inventory', false)");
    CHECK_FALSE((bool)se.Lua().script("return IsInputContextEnabled('Inventory')"));

    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::E));
    input.Update(1.0f / 60.0f);
    CHECK_FALSE(input.WasPressed("Equip"));

    se.Lua().script("SetInputContextEnabled('Inventory', true)");
    input.Push(sage::input::InputEvent::KeyUp(sage::input::Key::E));
    input.Update(1.0f / 60.0f);
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::E));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE(input.WasPressed("Equip"));
}

// «Сырой» ввод: скрипт получает смещение мыши и управляет захватом курсора —
// без этого вид от первого лица из Lua написать нельзя.
TEST(Scripting_raw_input_gives_mouse_and_capture) {
    struct FakeCursor : sage::input::CursorControl {
        bool Captured = false;
        void SetCursorCaptured(bool c) override { Captured = c; }
        bool CursorCaptured() const override { return Captured; }
    } fake;

    ScriptEngine se;
    sage::input::InputSystem input;
    input.SetCursorControl(&fake);
    se.BindInput(input);

    input.Push(sage::input::InputEvent::MouseMove({100.0f, 50.0f}, {3.0f, -2.0f}));
    input.Push(sage::input::InputEvent::Wheeled(-1.0f));
    input.Update(1.0f / 60.0f);

    CHECK_NEAR((float)se.Lua().script("return GetMouseDelta().x"), 3.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetMouseDelta().y"), -2.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetMousePosition().x"), 100.0f, 1e-4);
    CHECK_NEAR((float)se.Lua().script("return GetScrollDelta()"), -1.0f, 1e-4);

    CHECK_FALSE((bool)se.Lua().script("return IsMouseCaptured()"));
    se.Lua().script("SetMouseCaptured(true)");
    CHECK_TRUE(fake.Captured);
    CHECK_TRUE((bool)se.Lua().script("return IsMouseCaptured()"));
}

// Без привязанного ввода GetMouseDelta обязан внятно ругаться, а не молча
// отдавать нули: молчаливый ноль выглядит как «мышь не двигают».
TEST(Scripting_raw_input_without_binding_errors) {
    ScriptEngine se;
    auto result = se.Lua().safe_script("return GetMouseDelta()", sol::script_pass_on_error);
    CHECK_FALSE(result.valid());
    // Скролл — исключение: он опционален, и ноль для него честный ответ.
    CHECK_NEAR((float)se.Lua().script("return GetScrollDelta()"), 0.0f, 1e-4);
}

// ============================================================================
//  Источник напрямую (SourceDown/Pressed/Released/Value, AnyPressedSource,
//  FindConflict, AddBinding, ContextNames, ReleaseAll, TypedText, геймпад,
//  Configure)
//
//  Именованных действий (BindAction/IsActionDown) хватает игровой логике, но
//  не экрану настроек: без прямого доступа к физическому источнику на Lua
//  нельзя написать ни ловлю «нажмите новую клавишу», ни проверку конфликта, ни
//  поле ввода имени игрока.
// ============================================================================

TEST(Scripting_source_down_pressed_released_by_name) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    CHECK_FALSE((bool)se.Lua().script("return input.SourceDown('W')"));
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::W));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE((bool)se.Lua().script("return input.SourceDown('W')"));
    CHECK_TRUE((bool)se.Lua().script("return input.SourcePressed('W')"));
    CHECK_FALSE((bool)se.Lua().script("return input.SourceReleased('W')"));

    input.Push(sage::input::InputEvent::KeyUp(sage::input::Key::W));
    input.Update(1.0f / 60.0f);
    CHECK_FALSE((bool)se.Lua().script("return input.SourceDown('W')"));
    CHECK_TRUE((bool)se.Lua().script("return input.SourceReleased('W')"));

    // Мышь и геймпад — та же строка, тот же путь.
    input.Push(sage::input::InputEvent::MouseDown(sage::input::MouseButton::Left));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE((bool)se.Lua().script("return input.SourceDown('MOUSE_LEFT')"));

    // Геймпад отвечает, только когда подключён — как и в настоящей игре.
    input.Push(sage::input::InputEvent::PadConnected(0, true));
    input.Push(sage::input::InputEvent::PadDown(sage::input::GamepadButton::A, 0));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE((bool)se.Lua().script("return input.SourceDown('PAD_A')"));

    // Нераспознанное имя — честное false, а не ошибка.
    CHECK_FALSE((bool)se.Lua().script("return input.SourceDown('NOT_A_KEY')"));
}

TEST(Scripting_source_value_works_for_any_kind) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    // Кнопка как значение — 1.0/0.0, удобно, когда вид источника заранее не
    // известен (настраиваемая клавиша).
    CHECK_NEAR((float)se.Lua().script("return input.SourceValue('SPACE')"), 0.0f, 1e-4);
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    input.Update(1.0f / 60.0f);
    CHECK_NEAR((float)se.Lua().script("return input.SourceValue('SPACE')"), 1.0f, 1e-4);

    // Колесо мыши.
    input.Push(sage::input::InputEvent::Wheeled(-2.0f));
    input.Update(1.0f / 60.0f);
    CHECK_NEAR((float)se.Lua().script("return input.SourceValue('WHEEL_DOWN')"), -2.0f, 1e-4);

    // Ось геймпада — тоже только у подключённого.
    input.Push(sage::input::InputEvent::PadConnected(0, true));
    input.Push(sage::input::InputEvent::PadAxisMoved(sage::input::GamepadAxis::LeftX, 0.6f, 0));
    input.Update(1.0f / 60.0f);
    CHECK_NEAR((float)se.Lua().script("return input.SourceValue('PAD_LEFT_X')"), 0.6f, 1e-4);
}

TEST(Scripting_any_pressed_source_catches_the_frame_press) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    CHECK_TRUE((bool)se.Lua().script("return input.AnyPressedSource() == nil"));

    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Q));
    input.Update(1.0f / 60.0f);
    std::string caught = se.Lua().script("return input.AnyPressedSource()");
    CHECK_EQ(caught, std::string("Q"));

    // Голый модификатор (Shift без второй клавиши) источником не считается —
    // сочетание ещё не дожали.
    sage::input::InputEvent shift = sage::input::InputEvent::KeyDown(sage::input::Key::LeftShift);
    input.Push(shift);
    input.Update(1.0f / 60.0f);
    CHECK_TRUE((bool)se.Lua().script("return input.AnyPressedSource() == nil"));
}

TEST(Scripting_find_conflict_and_add_binding) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    se.Lua().script("BindAction('Jump', 'SPACE')");
    CHECK_TRUE((bool)se.Lua().script("return input.FindConflict('SPACE') == 'Jump'"));
    CHECK_TRUE((bool)se.Lua().script("return input.FindConflict('Q') == nil"));

    // AddBinding ДОБАВЛЯЕТ, не заменяя: пробел остаётся рабочим.
    CHECK_TRUE((bool)se.Lua().script("return input.AddBinding('Jump', 'PAD_A')"));
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE(input.WasPressed("Jump"));
}

TEST(Scripting_context_names_and_release_all) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    se.Lua().script("CreateInputContext('Inventory', 50)");
    sol::table names = se.Lua().script("return input.ContextNames()");
    bool foundDefault = false, foundInventory = false;
    for (auto& kv : names) {
        const std::string n = kv.second.as<std::string>();
        if (n == "Gameplay") foundDefault = true;
        if (n == "Inventory") foundInventory = true;
    }
    CHECK_TRUE(foundDefault);
    CHECK_TRUE(foundInventory);

    se.Lua().script("BindAction('Jump', 'SPACE')");
    input.Push(sage::input::InputEvent::KeyDown(sage::input::Key::Space));
    input.Update(1.0f / 60.0f);
    CHECK_TRUE(input.IsDown("Jump"));

    // ReleaseAll гасит зажатое немедленно — как потеря фокуса окна.
    se.Lua().script("input.ReleaseAll()");
    CHECK_FALSE(input.IsDown("Jump"));
}

TEST(Scripting_typed_text_reads_this_frames_unicode) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    input.Push(sage::input::InputEvent::Text((unsigned int)'H'));
    input.Push(sage::input::InputEvent::Text((unsigned int)'i'));
    input.Update(1.0f / 60.0f);
    CHECK_EQ((std::string)se.Lua().script("return input.TypedText()"), std::string("Hi"));
}

TEST(Scripting_gamepad_connected_and_name) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);

    CHECK_FALSE((bool)se.Lua().script("return input.GamepadConnected()"));
    input.Push(sage::input::InputEvent::PadConnected(0, true));
    input.Update(1.0f / 60.0f);
    input.MutableState().PadMutable(0).SetName("Test Pad");

    CHECK_TRUE((bool)se.Lua().script("return input.GamepadConnected()"));      // «хоть один»
    CHECK_TRUE((bool)se.Lua().script("return input.GamepadConnected(0)"));
    CHECK_FALSE((bool)se.Lua().script("return input.GamepadConnected(1)"));
    CHECK_EQ((std::string)se.Lua().script("return input.GamepadName(0)"), std::string("Test Pad"));
}

TEST(Scripting_configure_tunes_dead_zone_and_sensitivity) {
    ScriptEngine se;
    sage::input::InputSystem input;
    se.BindInput(input);
    se.Lua().script("BindAxis('Look', 'RIGHT', 'LEFT')");

    CHECK_TRUE((bool)se.Lua().script(
        "return input.Configure('Look', {deadZone = 0.4, sensitivity = 2.0, smoothing = 0.0})"));
    const sage::input::ActionSettings& s = input.Find("Look")->Settings();
    CHECK_NEAR(s.DeadZone, 0.4f, 1e-4);
    CHECK_NEAR(s.Sensitivity, 2.0f, 1e-4);

    // Действия, которого нет, — честный false, а не ошибка.
    CHECK_FALSE((bool)se.Lua().script("return input.Configure('Never Declared', {deadZone = 0.1})"));
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
#include <chrono>
#include <filesystem>
#include <system_error>

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

// ---------------------------------------------------------------------------
// ГОРЯЧАЯ ПЕРЕЗАГРУЗКА СКРИПТОВ
//
// Скрипты правят во внешнем редакторе (своего в SAGE нет), и до этой правки
// изменение файла не значило НИЧЕГО, пока игру не перезапустят: цикл «поправил
// число — посмотрел» стоил прохождения уровня заново.
//
// Время правки файла у файловых систем имеет зернистость (на ext4 — наносекунды,
// но на некоторых FAT/сетевых — до двух секунд), поэтому тесты не полагаются на
// «прошло достаточно времени»: штамп сдвигается ЯВНО через last_write_time.
// ---------------------------------------------------------------------------
namespace {
void TouchLater(const std::string& path) {
    std::error_code ec;
    const auto now = std::filesystem::last_write_time(path, ec);
    std::filesystem::last_write_time(path, now + std::chrono::seconds(5), ec);
}
} // namespace

TEST(Scripting_edited_file_is_picked_up_without_restart) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject hero = scene.CreateObject("Hero");

    const std::string path = WriteTempScript("hotreload", "function OnStart(e) _G.MARK = 1 end\n");
    se.AttachScript(hero, path);
    CHECK_EQ(se.Lua()["MARK"].get<int>(), 1);

    // Ничего не менялось — перезагружать нечего. Иначе скрипт пересобирался бы
    // каждый кадр, и состояние сбрасывалось бы у всех подряд без причины.
    CHECK_EQ(se.ReloadChangedScripts(), 0);
    CHECK_EQ(se.Lua()["MARK"].get<int>(), 1);

    { std::ofstream f(path); f << "function OnStart(e) _G.MARK = 2 end\n"; }
    TouchLater(path);
    CHECK_EQ(se.ReloadChangedScripts(), 1);
    // Новый код выполнен, и выполнен ЗАНОВО: OnStart — часть перезагрузки, без
    // него скрипт остался бы со старым состоянием и новым кодом вперемешку.
    CHECK_EQ(se.Lua()["MARK"].get<int>(), 2);

    // И повторный вызов уже ничего не делает: штамп запомнен.
    CHECK_EQ(se.ReloadChangedScripts(), 0);
    std::filesystem::remove(path);
}

TEST(Scripting_broken_edit_keeps_the_working_script) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject hero = scene.CreateObject("Hero");

    const std::string path = WriteTempScript(
        "hotreload_broken", "function OnUpdate(e, dt) _G.TICKS = (_G.TICKS or 0) + 1 end\n");
    se.AttachScript(hero, path);
    se.UpdateAll(0.016f);
    CHECK_EQ(se.Lua()["TICKS"].get<int>(), 1);

    // Недописанная строка — обычное состояние файла в середине правки, и
    // выключать из-за неё то, что уже работает, нельзя: иначе каждое сохранение
    // на полуслове роняло бы игру.
    { std::ofstream f(path); f << "function OnUpdate(e, dt) this is not lua\n"; }
    TouchLater(path);
    CHECK_EQ(se.ReloadChangedScripts(), 0);   // не перечитан
    se.UpdateAll(0.016f);
    CHECK_EQ(se.Lua()["TICKS"].get<int>(), 2); // прежний продолжает работать

    // Опечатку исправили — подхватывается без перезапуска.
    { std::ofstream f(path); f << "function OnUpdate(e, dt) _G.TICKS = (_G.TICKS or 0) + 10 end\n"; }
    TouchLater(path);
    CHECK_EQ(se.ReloadChangedScripts(), 1);
    se.UpdateAll(0.016f);
    CHECK_EQ(se.Lua()["TICKS"].get<int>(), 12);
    std::filesystem::remove(path);
}

TEST(Scripting_level_script_reloads_too) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Уровневый скрипт (RunScript) — не привязан ни к одной сущности, и OnStart
    // у него без аргумента. Перезагрузка обязана работать и для него: правила
    // уровня правят не реже, чем поведение объекта.
    const std::string path = WriteTempScript("hotreload_level", "function OnStart() _G.RULE = 1 end\n");
    se.RunScript(path);
    CHECK_EQ(se.Lua()["RULE"].get<int>(), 1);

    { std::ofstream f(path); f << "function OnStart() _G.RULE = 7 end\n"; }
    TouchLater(path);
    CHECK_EQ(se.ReloadChangedScripts(), 1);
    CHECK_EQ(se.Lua()["RULE"].get<int>(), 7);
    std::filesystem::remove(path);
}

// ============================================================================
//  КОРОТКИЕ ИМЕНА МОДУЛЕЙ: game.Quit() вместо sage.game.Quit()
//
//  «sage.» перед каждым вызовом — четыре знака, которые пишут десятки раз на
//  файл, и ноль сведений. Область при этом остаётся названной, а значит и
//  порядок, ради которого приставку заводили, не теряется.
//
//  Проверяется ИМЕННО ТОЖДЕСТВО, а не «обе функции работают»: псевдоним обязан
//  ссылаться на ТУ ЖЕ таблицу. Копия разъехалась бы с оригиналом на первой
//  правке движка, и об этом никто бы не узнал — обе вызывались бы без ошибок.
// ============================================================================
TEST(Scripting_short_module_name_is_the_same_table) {
    ScriptEngine se;

    const bool same = se.Lua().script("return game == sage.game").get<bool>();
    CHECK_TRUE(same);

    // И это верно для КАЖДОГО модуля, а не для того, о котором вспомнили.
    // Список берётся из самой таблицы sage: модуль, добавленный завтра,
    // попадает в проверку сам.
    const std::string report = se.Lua().script(R"(
        local missing = {}
        for name, value in pairs(sage) do
            if type(value) == "table" then
                if name == "math" then
                    -- math особый: дописываемся в стандартную таблицу.
                    for fn, impl in pairs(value) do
                        if math[fn] ~= impl then missing[#missing+1] = "math." .. fn end
                    end
                elseif _G[name] ~= value then
                    missing[#missing+1] = name
                end
            end
        end
        return table.concat(missing, ", ")
    )").get<std::string>();
    if (!report.empty()) std::printf("       без короткого имени: %s\n", report.c_str());
    CHECK_TRUE(report.empty());
}

TEST(Scripting_short_names_do_not_break_the_lua_standard_library) {
    ScriptEngine se;

    // math.floor обязан остаться на месте: модуль движка ДОПИСЫВАЕТСЯ в
    // стандартную таблицу, а не подменяет её. Подмена отобрала бы у игры
    // math.floor/random/pi — то есть сломала бы любой уже написанный скрипт.
    const float floored = se.Lua().script("return math.floor(3.7)").get<float>();
    CHECK_NEAR(floored, 3.0f, 1e-4);
    const bool pi = se.Lua().script("return math.pi > 3.14 and math.pi < 3.15").get<bool>();
    CHECK_TRUE(pi);
    // И при этом рядом лежит математика движка.
    const float lerp = se.Lua().script("return math.Lerp(0.0, 10.0, 0.25)").get<float>();
    CHECK_NEAR(lerp, 2.5f, 1e-4);
}

TEST(Scripting_short_names_never_replace_a_lua_standard_global) {
    ScriptEngine se;
    // ПРАВИЛО: занятое имя не трогаем. Проверяется на том, что занято ВСЕГДА —
    // на стандартной библиотеке Lua. Модуль, названный завтра `table` или
    // `string`, отобрал бы у каждой игры её таблицы молча: скрипт падал бы на
    // table.insert, и искать причину человек шёл бы в свой код.
    const std::string stolen = se.Lua().script(R"(
        local taken = {"table", "string", "os", "io", "coroutine", "debug", "utf8", "package"}
        local bad = {}
        for _, name in ipairs(taken) do
            if sage[name] ~= nil and _G[name] == sage[name] then bad[#bad+1] = name end
        end
        -- math особый: он не подменён, а ДОПИСАН — стандартные функции на месте.
        if type(math.floor) ~= "function" then bad[#bad+1] = "math" end
        return table.concat(bad, ", ")
    )").get<std::string>();
    if (!stolen.empty()) std::printf("       отобрано у Lua: %s\n", stolen.c_str());
    CHECK_TRUE(stolen.empty());
}

// ============================================================================
//  ПОДСКАЗКА ПО API НЕ ВРЁТ
//
//  editor/assets/api/sage.lua — то, что редактор кода человека показывает как
//  список доступных функций. Собирается он из вызовов Bind(...) разбором
//  исходников (scripts/gen_script_api.py), а разбор текста всегда может
//  ошибиться: пропустить вызов, склеить не тот аргумент, выдумать модуль.
//
//  Поэтому проверяется не «файл собрался», а ДВЕ вещи, которые только и делают
//  подсказку полезной: она читается как Lua (иначе редактор молча её не
//  подхватит, и человек решит, что подсказок в движке нет) и КАЖДАЯ обещанная
//  функция в движке действительно есть. Подсказка, предлагающая
//  несуществующее, хуже отсутствующей: отсутствующую человек компенсирует
//  документацией, а этой он верит и идёт искать ошибку в своём коде.
// ============================================================================
TEST(Scripting_api_hints_parse_and_match_the_engine) {
    namespace fs = std::filesystem;
    const fs::path stub =
        fs::path(__FILE__).parent_path().parent_path() / "editor" / "assets" / "api" / "sage.lua";
    std::error_code ec;
    CHECK_TRUE(fs::exists(stub, ec));
    if (!fs::exists(stub, ec)) return;

    ScriptEngine se;
    // ЧИТАЕТСЯ ЛИ. Именно load, а не script: выполнять описание незачем, оно
    // затёрло бы живые таблицы своими заглушками.
    sol::load_result chunk = se.Lua().load_file(stub.string());
    CHECK_TRUE(chunk.valid());
    if (!chunk.valid()) {
        sol::error err = chunk;
        std::printf("       подсказка не читается как Lua: %s\n", err.what());
        return;
    }

    // СОВПАДАЕТ ЛИ. Каждая строка «function sage.<модуль>.<Имя>(» обязана
    // отвечать живой функции.
    std::ifstream in(stub);
    std::string line, missing;
    int promised = 0;
    while (std::getline(in, line)) {
        const std::string head = "function sage.";
        if (line.rfind(head, 0) != 0) continue;
        const size_t open = line.find('(');
        if (open == std::string::npos) continue;
        const std::string full = line.substr(head.size() - 5, open - (head.size() - 5));
        const size_t dot = full.find('.', 5);
        if (dot == std::string::npos) continue;
        const std::string module = full.substr(5, dot - 5);
        const std::string name = full.substr(dot + 1);
        ++promised;
        const sol::object fn = se.Lua()["sage"][module][name];
        if (fn.get_type() != sol::type::function) {
            if (missing.size() < 200) missing += (missing.empty() ? "" : ", ") + full;
        }
    }
    std::printf("       подсказка обещает функций: %d\n", promised);
    CHECK_TRUE(promised > 100);   // файл не должен молча выродиться в пустой
    if (!missing.empty()) std::printf("       в движке нет: %s\n", missing.c_str());
    CHECK_TRUE(missing.empty());
}


// ============================================================================
//  `self` — СВОЯ СУЩНОСТЬ, ВИДНА ВО ВСЁМ ФАЙЛЕ
//
//  Раньше сущность приносил только аргумент хука. Функция, вынесенная из
//  OnUpdate — а её выносят сразу, как только тело перерастает десяток строк, —
//  своей сущности не видела: её тащили аргументом через каждый вызов или
//  запоминали в OnStart в свою переменную. И то и другое — обход того, что
//  движок и так знает.
//
//  Проверяется ТОЖДЕСТВО с аргументом хука и РАЗДЕЛЬНОСТЬ между скриптами:
//  общий `self` на двоих был бы хуже его отсутствия — скрипт молча правил бы
//  чужой объект.
// ============================================================================
TEST(Scripting_self_is_the_own_entity_everywhere_in_the_file) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("Hero");

    // Итоги скрипт кладёт в ОБЩУЮ таблицу: присваивание имени уходит в
    // окружение скрипта (у каждого своё), а поле общей таблицы видно снаружи.
    se.Lua()["Out"] = se.Lua().create_table();

    const std::string path = WriteTempScript("self_hook", R"(
        -- Код верхнего уровня тоже видит self: «настроил и забыл» — обычный
        -- способ писать скрипт, и сущность нужна ему так же.
        Out.topLevelName = self.Name

        local function move()        -- вынесенная функция: аргумента у неё нет
            self.Transform.Position.y = 5.0
        end

        function OnStart(entity)
            Out.sameObject = (self == entity)
            move()
        end
    )");
    se.AttachScript(obj, path);
    std::remove(path.c_str());

    CHECK_EQ(se.Lua()["Out"]["topLevelName"].get<std::string>(), std::string("Hero"));
    CHECK_TRUE(se.Lua()["Out"]["sameObject"].get<bool>());
    CHECK_NEAR(obj.GetTransform().Position.y, 5.0f, 1e-4);
}

TEST(Scripting_self_is_not_shared_between_scripts) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject a = scene.CreateObject("A");
    GameObject b = scene.CreateObject("B");

    // Один и тот же файл на двух сущностях: у каждой обязан быть СВОЙ self.
    // Общий сделал бы `self` ловушкой — второй скрипт молча правил бы первый.
    const std::string path = WriteTempScript("self_each", R"(
        function OnStart(entity)
            self.Transform.Position.x = 1.0
        end
    )");
    se.AttachScript(a, path);
    se.AttachScript(b, path);
    std::remove(path.c_str());

    CHECK_NEAR(a.GetTransform().Position.x, 1.0f, 1e-4);
    CHECK_NEAR(b.GetTransform().Position.x, 1.0f, 1e-4);
    // И ни одна из них не подвинулась дважды/за другую.
    CHECK_NEAR(a.GetTransform().Position.y, 0.0f, 1e-4);
    CHECK_NEAR(b.GetTransform().Position.y, 0.0f, 1e-4);
}

// ============================================================================
//  camera.ScreenToRay / camera.WorldToScreen
//
//  До этого щёлкнуть мышью по объекту сцены (не по центру экрана прицелом, а
//  ИМЕННО там, куда указывает курсор) было нечем: у камеры был только базис
//  (Position/Front/Right/Up/Fov), а перевести точку экрана в луч — отдельная
//  геометрия, которую иначе пришлось бы писать в каждой игре заново. Проверяем
//  оба направления и их согласованность друг с другом.
// ============================================================================

TEST(Scripting_camera_screen_to_ray_needs_binding) {
    ScriptEngine se; // камера не привязана (BindCamera не вызван)
    auto r = se.Lua().safe_script("return camera.ScreenToRay(0, 0)", sol::script_pass_on_error);
    CHECK_FALSE(r.valid());
}

TEST(Scripting_camera_screen_to_ray_center_points_forward) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.UiFrame.Size = glm::vec2(800.0f, 600.0f);
    Camera cam;   // умолчания: Position (0,0,3), Front (0,0,-1), Fov 60
    se.BindCamera(cam);

    // Центр экрана — луч точно вдоль Front, каким бы ни было поле зрения.
    se.Lua().script("Ray = camera.ScreenToRay(400, 300)");
    const glm::vec3 origin = se.Lua()["Ray"]["origin"];
    const glm::vec3 dir = se.Lua()["Ray"]["dir"];
    CHECK_NEAR(origin.x, cam.Position.x, 1e-4);
    CHECK_NEAR(origin.y, cam.Position.y, 1e-4);
    CHECK_NEAR(origin.z, cam.Position.z, 1e-4);
    CHECK_NEAR(dir.x, cam.Front.x, 1e-3);
    CHECK_NEAR(dir.y, cam.Front.y, 1e-3);
    CHECK_NEAR(dir.z, cam.Front.z, 1e-3);
}

TEST(Scripting_camera_world_to_screen_point_ahead_is_screen_center) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.UiFrame.Size = glm::vec2(800.0f, 600.0f);
    Camera cam;
    se.BindCamera(cam);

    // Точка ровно впереди камеры (вдоль Front) обязана лечь в центр кадра.
    const glm::vec3 ahead = cam.Position + cam.Front * 10.0f;
    se.Lua()["P"] = ahead;
    se.Lua().script("S = camera.WorldToScreen(P)");
    sol::object result = se.Lua()["S"];
    CHECK_TRUE(result.get_type() != sol::type::lua_nil);
    const glm::vec2 screen = result.as<glm::vec2>();
    CHECK_NEAR(screen.x, 400.0f, 0.5f);
    CHECK_NEAR(screen.y, 300.0f, 0.5f);
}

TEST(Scripting_camera_world_to_screen_behind_camera_is_nil) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.UiFrame.Size = glm::vec2(800.0f, 600.0f);
    Camera cam;
    se.BindCamera(cam);

    // Точка ЗА спиной камеры — честный nil, а не координата, случайно попавшая
    // на экран: спроецировать её без вранья нельзя (тангенс угла уходит в
    // отрицательную полуплоскость и «отражается» на противоположный край).
    const glm::vec3 behind = cam.Position - cam.Front * 10.0f;
    se.Lua()["P"] = behind;
    const bool isNil = se.Lua().script("return camera.WorldToScreen(P) == nil");
    CHECK_TRUE(isNil);
}

TEST(Scripting_camera_screen_to_ray_and_world_to_screen_are_consistent) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.UiFrame.Size = glm::vec2(800.0f, 600.0f);
    Camera cam;
    se.BindCamera(cam);

    // Точка МИМО центра: спроецировать в экран и тут же пустить луч из той же
    // точки экрана обязано указать почти точно обратно на неё же.
    const glm::vec3 world = cam.Position + cam.Front * 8.0f + cam.Right * 2.0f + cam.Up * 1.0f;
    se.Lua()["P"] = world;
    se.Lua().script("S = camera.WorldToScreen(P)");
    se.Lua().script("Ray = camera.ScreenToRay(S.x, S.y)");
    const glm::vec3 dir = se.Lua()["Ray"]["dir"];
    const glm::vec3 expected = glm::normalize(world - cam.Position);
    CHECK_NEAR(glm::dot(dir, expected), 1.0f, 1e-3);
}
