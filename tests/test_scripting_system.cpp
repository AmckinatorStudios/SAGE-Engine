// Модульные тесты НОВОЙ системы скриптинга: архитектура
// ScriptingSystem → ScriptRuntime → LanguageBackend → LuaBackend.
//
// Что здесь проверяется и почему именно это:
//   • объявление публичных переменных читается ТЕКСТОМ (инспектор обязан
//     показывать их, не запуская чужой файл);
//   • значения из сцены доезжают до скрипта, а правка файла их не сбрасывает;
//   • хуки зовутся только те, что объявлены (пустых методов писать не надо);
//   • ошибка выполнения несёт ФАЙЛ И СТРОКУ и не роняет ни движок, ни соседние
//     скрипты;
//   • горячая перезагрузка не заменяет рабочий скрипт сломанным;
//   • контроллер персонажа падает сам, без единой строки тяготения в скрипте.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "sage/input/InputSystem.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace {

using namespace sage::scripting;

std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = "sage_scriptsys_" + name + ".lua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    f.close();
    return path;
}

// Готовая система с Lua-бэкендом на СВОЁМ состоянии (без старого движка):
// ровно так же в неё встанет любой второй язык.
std::unique_ptr<ScriptingSystem> MakeSystem(Scene& scene) {
    auto sys = std::make_unique<ScriptingSystem>();
    sys->AddBackend(MakeLuaBackend({}));
    ScriptServices services;
    services.ScenePtr = &scene;
    sys->Bind(services);
    return sys;
}

} // namespace

// --- Объявление публичных переменных -----------------------------------------

TEST(ScriptSystem_fields_parsed_from_text) {
    const std::string source = R"LUA(
local Player = {}

Player.public = {
    MoveSpeed = field.number(5.0, 0.0, 20.0),
    Jumps     = field.integer(2, 0, 5),
    CanJump   = field.boolean(true),
    Title     = field.string("hero"),
    Camera    = field.entity(),
    Animator  = field.component("Animation"),
    Tint      = field.color(),
    Offset    = field.vector3(),
    Step      = field.asset("Audio"),
    Health    = 100,
    Ready     = false,
}

function Player:Update(dt) end

return Player
)LUA";
    const sage::vars::Table fields = ParseFields("player.lua", source);
    CHECK_EQ((int)fields.Size(), 11);

    const sage::vars::Var* speed = fields.Find("MoveSpeed");
    CHECK_TRUE(speed != nullptr);
    if (speed) {
        CHECK_TRUE(speed->Data.Type() == sage::vars::Kind::Float);
        CHECK_NEAR(speed->Data.AsFloat(), 5.0f, 1e-5f);
        CHECK_NEAR(speed->Min, 0.0f, 1e-5f);
        CHECK_NEAR(speed->Max, 20.0f, 1e-5f);
        CHECK_TRUE(speed->Declared);
    }
    const sage::vars::Var* jumps = fields.Find("Jumps");
    CHECK_TRUE(jumps && jumps->Data.Type() == sage::vars::Kind::Int);
    const sage::vars::Var* canJump = fields.Find("CanJump");
    CHECK_TRUE(canJump && canJump->Data.AsBool());
    const sage::vars::Var* title = fields.Find("Title");
    CHECK_TRUE(title && title->Data.AsString() == "hero");
    // Ссылка на компонент — это ссылка на объект С УТОЧНЕНИЕМ: слот инспектора
    // по нему фильтрует, а скрипт получает сразу компонент.
    const sage::vars::Var* animator = fields.Find("Animator");
    CHECK_TRUE(animator && animator->Data.Type() == sage::vars::Kind::Entity);
    if (animator) CHECK_TRUE(animator->Hint == "Animation");
    const sage::vars::Var* camera = fields.Find("Camera");
    CHECK_TRUE(camera && camera->Data.Type() == sage::vars::Kind::Entity);
    if (camera) CHECK_TRUE(camera->Hint.empty());
    const sage::vars::Var* clip = fields.Find("Step");
    CHECK_TRUE(clip && clip->Data.Type() == sage::vars::Kind::Asset);
    if (clip) CHECK_TRUE(clip->Hint == "Audio");
    const sage::vars::Var* tint = fields.Find("Tint");
    CHECK_TRUE(tint && tint->Data.Type() == sage::vars::Kind::Color);
    const sage::vars::Var* offset = fields.Find("Offset");
    CHECK_TRUE(offset && offset->Data.Type() == sage::vars::Kind::Vec3);
    // Голый литерал — тоже объявление: обязательного field.* быть не должно.
    const sage::vars::Var* health = fields.Find("Health");
    CHECK_TRUE(health && health->Data.Type() == sage::vars::Kind::Int);
    if (health) CHECK_EQ(health->Data.AsInt(), 100);
}

TEST(ScriptSystem_fields_absent_is_not_an_error) {
    const sage::vars::Table fields = ParseFields("plain.lua", "local T = {}\nreturn T\n");
    CHECK_TRUE(fields.Empty());
}

// Правка скрипта НЕ ИМЕЕТ ПРАВА сбрасывать настройки объектов уровня: иначе
// «подобрать скорость» означало бы расставлять её заново у каждого объекта
// после каждой правки файла.
TEST(ScriptSystem_fields_keep_scene_values_on_merge) {
    sage::vars::Table scene;
    sage::vars::Var mine;
    mine.Name = "MoveSpeed";
    mine.Data = sage::vars::Value(9.0f);
    scene.Put(mine);

    const sage::vars::Table declaration =
        ParseFields("p.lua", "P.public = { MoveSpeed = field.number(5.0, 0.0, 20.0), Extra = 1 }");
    scene.MergeDeclaration(declaration);

    const sage::vars::Var* speed = scene.Find("MoveSpeed");
    CHECK_TRUE(speed != nullptr);
    if (speed) {
        CHECK_NEAR(speed->Data.AsFloat(), 9.0f, 1e-5f); // значение объекта
        CHECK_NEAR(speed->Max, 20.0f, 1e-5f);           // описание из скрипта
    }
    const sage::vars::Var* extra = scene.Find("Extra");
    CHECK_TRUE(extra != nullptr); // новая переменная пришла со своим умолчанием
}

// Переменная, исчезнувшая из скрипта (опечатка в имени, переименование),
// остаётся видна и помечается необъявленной — молча стереть настройку хуже.
TEST(ScriptSystem_fields_renamed_variable_survives) {
    sage::vars::Table scene;
    sage::vars::Var mine;
    mine.Name = "Speed";
    mine.Data = sage::vars::Value(7.0f);
    mine.Declared = true;
    scene.Put(mine);

    scene.MergeDeclaration(ParseFields("p.lua", "P.public = { MoveSpeed = 5.0 }"));

    const sage::vars::Var* old = scene.Find("Speed");
    CHECK_TRUE(old != nullptr);
    if (old) {
        CHECK_FALSE(old->Declared);
        CHECK_NEAR(old->Data.AsFloat(), 7.0f, 1e-5f);
    }
    CHECK_TRUE(scene.Find("MoveSpeed") != nullptr);
}

// --- Жизненный цикл ----------------------------------------------------------

TEST(ScriptSystem_lifecycle_and_public_values) {
    Scene scene("test");
    GameObject obj = scene.CreateObject("Hero");

    const std::string path = WriteScript("life", R"LUA(
local Hero = {}
Hero.public = { Speed = field.number(1.0, 0.0, 10.0) }

function Hero:Start()
    self.started = (self.started or 0) + 1
    self.transform:SetPosition(0, 0, 0)
end

function Hero:Update(dt)
    self.transform:Translate(Vector3(self.Speed * dt, 0, 0))
end

return Hero
)LUA");

    ScriptComponent sc;
    sc.Path = path;
    sc.Fields = ParseFields(path, "");
    sage::vars::Var speed;
    speed.Name = "Speed";
    speed.Data = sage::vars::Value(4.0f);
    speed.Declared = true;
    sc.Fields.Put(speed);
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), sc);

    auto sys = MakeSystem(scene);
    CHECK_EQ(sys->AttachScene(scene), 1);

    // Start уже случился — позиция сброшена в ноль самим скриптом.
    CHECK_NEAR(obj.GetTransform().Position.x, 0.0f, 1e-5f);

    sys->Update(0.5f);
    // Значение публичной переменной пришло ИЗ СЦЕНЫ (4), а не из умолчания (1).
    CHECK_NEAR(obj.GetTransform().Position.x, 2.0f, 1e-4f);

    sys->Shutdown();
    CHECK_EQ((int)sys->Runtime().Count(), 0);
    std::filesystem::remove(path);
}

// Пустых методов писать не надо: зовётся только то, что скрипт объявил.
TEST(ScriptSystem_missing_hooks_are_not_called) {
    Scene scene("test");
    GameObject obj = scene.CreateObject("Quiet");
    const std::string path = WriteScript("quiet", "local Q = {}\nreturn Q\n");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    CHECK_EQ(sys->AttachScene(scene), 1);
    sys->Update(0.016f);       // не должно ни упасть, ни пожаловаться
    sys->FixedUpdate(0.016f);
    sys->LateUpdate(0.016f);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Ошибка выполнения: файл, строка, текст — и живой движок.
TEST(ScriptSystem_runtime_error_has_file_and_line) {
    Scene scene("test");
    GameObject obj = scene.CreateObject("Broken");
    const std::string path = WriteScript("broken", R"LUA(
local B = {}

function B:Update(dt)
    self.missing:Move()
end

return B
)LUA");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    ScriptError captured;
    int reports = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        captured = e;
        ++reports;
    });
    CHECK_EQ(sys->AttachScene(scene), 1);

    sys->Update(0.016f);
    CHECK_EQ(reports, 1);
    CHECK_EQ(captured.Line, 5);
    CHECK_TRUE(captured.File.find("broken") != std::string::npos);
    CHECK_TRUE(!captured.Message.empty());

    // Поток одинаковых сообщений гасится: шестьдесят строк в секунду делают
    // консоль бесполезной.
    for (int i = 0; i < 10; ++i) sys->Update(0.016f);
    CHECK_TRUE(reports <= 3);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Скрипт с опечаткой НЕ заменяет работающий: недописанная строка — обычное
// состояние файла в середине правки.
TEST(ScriptSystem_hot_reload_keeps_working_script_on_error) {
    Scene scene("test");
    GameObject obj = scene.CreateObject("Hot");
    const std::string path = WriteScript("hot", R"LUA(
local H = {}
function H:Update(dt) self.transform:Translate(Vector3(1, 0, 0)) end
return H
)LUA");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    sys->Runtime().SetErrorSink([](const ScriptError&) {}); // ошибку ждём, в лог не сыплем
    CHECK_EQ(sys->AttachScene(scene), 1);
    sys->Update(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.x, 1.0f, 1e-4f);

    // Штамп времени правки должен отличаться от прежнего.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::ofstream f(path, std::ios::binary); f << "local H = {\n"; }
    CHECK_EQ(sys->ReloadChanged(), 0);
    sys->Update(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.x, 2.0f, 1e-4f); // прежний экземпляр жив

    // Исправили — перечитался.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream f(path, std::ios::binary);
        f << "local H = {}\nfunction H:Update(dt) self.transform:Translate(Vector3(0, 1, 0)) end\nreturn H\n";
    }
    CHECK_EQ(sys->ReloadChanged(), 1);
    sys->Update(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.y, 1.0f, 1e-4f);

    sys->Shutdown();
    std::filesystem::remove(path);
}

// Скрипты разговаривают друг с другом через объект, а не через глобальные
// переменные.
TEST(ScriptSystem_scripts_talk_to_each_other) {
    Scene scene("test");
    GameObject a = scene.CreateObject("Caller");
    GameObject b = scene.CreateObject("Target");

    const std::string targetPath = WriteScript("target", R"LUA(
local T = {}
function T:Start() self.damage = 0 end
function T:TakeDamage(amount) self.damage = self.damage + amount end
return T
)LUA");
    const std::string callerPath = WriteScript("caller", R"LUA(
local C = {}
function C:Update(dt)
    local other = scene:Find("Target")
    if other then other:Call("TakeDamage", 20) end
end
return C
)LUA");
    scene.Registry().emplace<ScriptComponent>(b.Entity(), ScriptComponent{targetPath});
    scene.Registry().emplace<ScriptComponent>(a.Entity(), ScriptComponent{callerPath});

    auto sys = MakeSystem(scene);
    CHECK_EQ(sys->AttachScene(scene), 2);
    sys->Update(0.016f);

    // Читаем результат тем же путём, каким его получил бы движок.
    const bool called = sys->Runtime().Invoke(b.Entity(), "TakeDamage", {sage::vars::Value(0)});
    CHECK_TRUE(called);
    sys->Shutdown();
    std::filesystem::remove(targetPath);
    std::filesystem::remove(callerPath);
}

// Тег — «чем объект является»; поиск по нему не ломается от переименования.
TEST(ScriptSystem_scene_find_by_tag) {
    Scene scene("test");
    GameObject enemy = scene.CreateObject("Goblin");
    scene.Registry().emplace<TagComponent>(enemy.Entity(), TagComponent{"Enemy"});
    GameObject probe = scene.CreateObject("Probe");

    const std::string path = WriteScript("tag", R"LUA(
local P = {}
function P:Update(dt)
    local found = scene:FindByTag("Enemy")
    if found then found.name = "Found" end
end
return P
)LUA");
    scene.Registry().emplace<ScriptComponent>(probe.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    sys->Update(0.016f);
    CHECK_TRUE(enemy.Name() == "Found");
    sys->Shutdown();
    std::filesystem::remove(path);
}

// --- Контроллер персонажа ----------------------------------------------------
//
// Главная проверка всей переработки: скрипт НЕ пишет тяготение, а персонаж
// падает. Пока это писала игра, каждая игра писала его заново и чуть-чуть
// иначе.
TEST(ScriptSystem_character_falls_without_script_gravity) {
    Scene scene("test");
    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = glm::vec3(0.0f, 10.0f, 0.0f);
    CharacterControllerComponent cc;
    cc.Gravity = -10.0f;
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity(), cc);

    PhysicsScene physics(sage::physics::PhysicsWorld::DefaultBackend(), scene);
    if (!physics.SupportsCharacters()) return; // бэкенд без контроллеров — проверять нечего

    const float before = hero.GetTransform().Position.y;
    for (int i = 0; i < 30; ++i) physics.Step(scene, 1.0f / 60.0f);
    CHECK_TRUE(hero.GetTransform().Position.y < before - 0.5f);

    const CharacterControllerComponent& after =
        scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    CHECK_TRUE(after.VerticalVelocity < 0.0f);
}

// Игра, которая двигает персонажа сама (старый sage.physics.MoveCharacter),
// НЕ получает второе тяготение сверху своего.
TEST(ScriptSystem_character_self_driven_keeps_old_behaviour) {
    Scene scene("test");
    GameObject hero = scene.CreateObject("Hero");
    hero.GetTransform().Position = glm::vec3(0.0f, 10.0f, 0.0f);
    CharacterControllerComponent cc;
    cc.Managed = false; // ровно то, что ставит прежний MoveCharacter
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity(), cc);

    PhysicsScene physics(sage::physics::PhysicsWorld::DefaultBackend(), scene);
    for (int i = 0; i < 30; ++i) physics.Step(scene, 1.0f / 60.0f);

    const CharacterControllerComponent& after =
        scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    CHECK_NEAR(after.VerticalVelocity, 0.0f, 1e-5f);
}

// Прыжок задаётся ВЫСОТОЙ: её автор видит в игре и может подобрать.
TEST(ScriptSystem_character_jump_from_script) {
    Scene scene("test");
    GameObject hero = scene.CreateObject("Hero");
    CharacterControllerComponent cc;
    cc.Gravity = -10.0f;
    cc.Grounded = true;
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity(), cc);

    const std::string path = WriteScript("jump", R"LUA(
local J = {}
function J:Update(dt)
    local c = self:GetCharacterController()
    if c and not self.jumped then
        c:Jump(2.0)
        self.jumped = true
    end
end
return J
)LUA");
    scene.Registry().emplace<ScriptComponent>(hero.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    sys->Update(0.016f);

    const CharacterControllerComponent& after =
        scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    // v = sqrt(2 * g * h) = sqrt(2 * 10 * 2) ≈ 6.32
    CHECK_NEAR(after.VerticalVelocity, 6.3245f, 0.01f);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Компонента нет — честный nil, а не падение: спрашивать у камня его
// контроллер персонажа обычное дело.
TEST(ScriptSystem_missing_component_is_nil) {
    Scene scene("test");
    GameObject obj = scene.CreateObject("Rock");
    const std::string path = WriteScript("nilcomp", R"LUA(
local R = {}
function R:Update(dt)
    if self:GetCharacterController() == nil then self.gameObject.name = "NoController" end
end
return R
)LUA");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});

    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    sys->Update(0.016f);
    CHECK_TRUE(obj.Name() == "NoController");
    sys->Shutdown();
    std::filesystem::remove(path);
}

// ПРИМЕР ИЗ ПОСТАВКИ ОБЯЗАН РАБОТАТЬ.
//
// Пример, который «примерно показывает идею», хуже отсутствия примера: по нему
// пишут первый скрипт, он не заводится, и виноватым выглядит движок. Поэтому
// player_fps.lua здесь не читается глазами, а ЗАПУСКАЕТСЯ — со всеми вызовами
// Input, CharacterController, Animation и Camera, которые в нём есть.
TEST(ScriptSystem_shipped_fps_example_runs) {
    // Тесты гоняют и из корня репозитория, и из каталога сборки — ищем файл
    // вверх по дереву, а не привязываемся к одному из вариантов.
    std::filesystem::path script;
    std::filesystem::path dir = std::filesystem::current_path();
    for (int i = 0; i < 4; ++i) {
        const std::filesystem::path candidate = dir / "editor/assets/scripts/player_fps.lua";
        if (std::filesystem::exists(candidate)) { script = candidate; break; }
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    CHECK_TRUE(!script.empty());
    if (script.empty()) return;

    Scene scene("test");
    GameObject hero = scene.CreateObject("Hero");
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    GameObject camera = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(camera.Entity());

    ScriptComponent sc;
    sc.Path = script.string();
    // Ссылка на камеру — как её расставили бы мышью в инспекторе.
    sage::vars::Var cameraField;
    cameraField.Name = "Camera";
    cameraField.Data = sage::vars::Value(sage::vars::EntityRef{camera.Id()});
    cameraField.Declared = true;
    sc.Fields.Put(cameraField);
    scene.Registry().emplace<ScriptComponent>(hero.Entity(), sc);

    sage::input::InputSystem input;
    auto sys = std::make_unique<ScriptingSystem>();
    sys->AddBackend(MakeLuaBackend({}));
    ScriptServices services;
    services.ScenePtr = &scene;
    services.Input = &input;
    sys->Bind(services);

    int errors = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        ++errors;
        std::printf("       [пример] %s\n", e.Format().c_str());
    });

    CHECK_EQ(sys->AttachScene(scene), 1);
    for (int i = 0; i < 5; ++i) {
        input.BeginFrame();
        input.UpdateActions(1.0f / 60.0f);
        sys->Update(1.0f / 60.0f);
    }
    CHECK_EQ(errors, 0);
    // Скрипт объявил свои действия сам — раскладка принадлежит игре.
    CHECK_TRUE(input.Has("Jump"));
    CHECK_TRUE(input.Has("Sprint"));
    sys->Shutdown();
}
