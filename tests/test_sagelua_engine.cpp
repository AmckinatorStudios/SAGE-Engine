// SAGE Lua В ДВИЖКЕ: .slua на объекте сцены, через ScriptingSystem — тот же
// путь, что у Play в редакторе и у собранной игры.
//
// Модульные тесты компилятора (test_sagelua.cpp) гоняют код на голом Lua.
// Здесь — то, чего там нет: хуки движка доходят до onAwake/onUpdate, поля
// инспектора берутся из @field, ошибка компиляции не запускает скрипт,
// таймеры и await идут кадром движка, подписки уходят вместе с объектом.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "sage/input/InputSystem.h"
#include "sage/lang/SageCompiler.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

namespace {

using namespace sage::scripting;

std::string WriteSage(const std::string& name, const std::string& body) {
    const std::string path = name + ".slua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    return path;
}

std::unique_ptr<ScriptingSystem> MakeSystem(Scene& scene) {
    auto sys = std::make_unique<ScriptingSystem>();
    sys->AddBackend(MakeLuaBackend({}));
    ScriptServices services;
    services.ScenePtr = &scene;
    sys->Bind(services);
    return sys;
}

} // namespace

// Хуки движка -> хуки SAGE, поле инспектора -> значение экземпляра.
TEST(SageEngine_component_lifecycle_and_inspector_fields) {
    sage::lang::ClearCompileCache();
    Scene scene("test");
    GameObject obj = scene.CreateObject("Hero");
    const std::string path = WriteSage("SageEngineHero", R"SAGE(
class SageEngineHero extends Component
    @field(editable = true, name = "Скорость", range = { min = 0, max = 20, step = 0.5 })
    public speed: number = 1

    @field(editable = true)
    public title: string = "hero"

    private awake: integer = 0

    function onAwake()
        self.awake = self.awake + 1
    end

    function onStart()
        self.transform:SetPosition(0, 0, 0)
    end

    function onUpdate(dt: number)
        self.transform:Translate(Vector3(self.speed * dt, self.awake, 0))
    end
end
)SAGE");

    // Объявление полей — из класса, с подписью и диапазоном.
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const sage::vars::Table declared = ParseFields(path, text);
    CHECK_EQ((int)declared.Size(), 2);
    const sage::vars::Var* speed = declared.Find("speed");
    CHECK_TRUE(speed != nullptr);
    if (speed) {
        CHECK_TRUE(speed->Data.Type() == sage::vars::Kind::Float);
        CHECK_NEAR(speed->Data.AsFloat(), 1.0f, 1e-5f);
        CHECK_NEAR(speed->Max, 20.0f, 1e-5f);
        CHECK_TRUE(speed->Label == "Скорость");
    }
    CHECK_TRUE(declared.Find("awake") == nullptr);   // private — не в инспекторе

    ScriptComponent sc;
    sc.Path = path;
    sage::vars::Var mine;
    mine.Name = "speed";
    mine.Data = sage::vars::Value(4.0f);
    mine.Declared = true;
    sc.Fields.Put(mine);
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), sc);

    auto sys = MakeSystem(scene);
    int errors = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        ++errors;
        std::printf("  %s\n", e.Format().c_str());
    });
    CHECK_EQ(sys->AttachScene(scene), 1);
    sys->Update(0.5f);
    // Скорость — из сцены (4), не умолчание (1); onAwake отработал ровно раз.
    CHECK_NEAR(obj.GetTransform().Position.x, 2.0f, 1e-4f);
    CHECK_NEAR(obj.GetTransform().Position.y, 1.0f, 1e-4f);
    sys->Update(0.5f);
    CHECK_NEAR(obj.GetTransform().Position.y, 2.0f, 1e-4f);
    CHECK_EQ(errors, 0);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Ошибка типа — скрипт НЕ запускается, а консоль получает файл и строку .slua.
TEST(SageEngine_compile_error_blocks_the_script_and_points_at_the_line) {
    sage::lang::ClearCompileCache();
    Scene scene("test");
    GameObject obj = scene.CreateObject("Broken");
    const std::string path = WriteSage("SageEngineBroken", R"SAGE(
class SageEngineBroken extends Component
    public speed: number = 1

    function onStart()
        self.speed = "fast"
    end
end
)SAGE");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});
    auto sys = MakeSystem(scene);
    ScriptError captured;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) { captured = e; });
    CHECK_EQ(sys->AttachScene(scene), 0);
    CHECK_EQ(captured.Line, 6);
    CHECK_TRUE(captured.File.find("SageEngineBroken") != std::string::npos);
    CHECK_TRUE(captured.Message.find("Ошибка типа") != std::string::npos);
    CHECK_TRUE(captured.Message.find("Получено значение string") != std::string::npos);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Ошибка ВЫПОЛНЕНИЯ указывает в строку .slua, а не в сгенерированный Lua.
TEST(SageEngine_runtime_error_line_is_the_sage_line) {
    sage::lang::ClearCompileCache();
    Scene scene("test");
    GameObject obj = scene.CreateObject("Oops");
    const std::string path = WriteSage("SageEngineOops", R"SAGE(
class SageEngineOops extends Component
    function onUpdate(dt: number)
        local t = nil
        -- дальше — обращение к nil
        t.x = 1
    end
end
)SAGE");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});
    auto sys = MakeSystem(scene);
    ScriptError captured;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) { captured = e; });
    CHECK_EQ(sys->AttachScene(scene), 1);
    sys->Update(0.016f);
    CHECK_EQ(captured.Line, 6);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// Таймеры и await идут кадром движка; подписки уходят вместе с объектом.
TEST(SageEngine_timers_await_and_events_follow_the_object) {
    sage::lang::ClearCompileCache();
    Scene scene("test");
    GameObject obj = scene.CreateObject("Clock");
    GameObject probe = scene.CreateObject("Probe");
    const std::string path = WriteSage("SageEngineClock", R"SAGE(
event Ping(n: number)

class SageEngineClock extends Component
    async function onStart()
        Events.on(Ping, function(n: number)
            self.transform:Translate(Vector3(0, 0, n))
        end)
        Timer.after(0.1, function()
            self.transform:Translate(Vector3(1, 0, 0))
        end)
        await Time.wait(0.25)
        self.transform:Translate(Vector3(0, 10, 0))
    end
end
)SAGE");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});
    const std::string sender = WriteSage("SageEngineSender", R"SAGE(
event Ping(n: number)

class SageEngineSender extends Component
    function onUpdate(dt: number)
        Events.emit(Ping, 1)
    end
end
)SAGE");
    scene.Registry().emplace<ScriptComponent>(probe.Entity(), ScriptComponent{sender});

    auto sys = MakeSystem(scene);
    int errors = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        ++errors;
        std::printf("  %s\n", e.Format().c_str());
    });
    CHECK_EQ(sys->AttachScene(scene), 2);
    sys->Update(0.05f);
    CHECK_NEAR(obj.GetTransform().Position.x, 0.0f, 1e-4f);   // таймер ещё не пришёл
    sys->Update(0.06f);
    CHECK_NEAR(obj.GetTransform().Position.x, 1.0f, 1e-4f);   // 0.11 > 0.1
    CHECK_NEAR(obj.GetTransform().Position.y, 0.0f, 1e-4f);   // await ждёт 0.25
    sys->Update(0.2f);
    CHECK_NEAR(obj.GetTransform().Position.y, 10.0f, 1e-4f);
    const float z = obj.GetTransform().Position.z;
    CHECK_TRUE(z >= 2.0f);   // событие от соседа доходит каждый кадр

    // Объект уничтожен — его обработчик больше не зовётся (и не падает).
    scene.RemoveObject(obj.Id());
    sys->Update(0.016f);
    sys->Update(0.016f);
    CHECK_EQ(errors, 0);
    sys->Shutdown();
    std::filesystem::remove(path);
    std::filesystem::remove(sender);
}

// entity:addComponent(Класс SAGE): второй компонент объекта ведёт runtime SAGE.
TEST(SageEngine_add_and_get_component_of_a_sage_class) {
    sage::lang::ClearCompileCache();
    Scene scene("test");
    GameObject obj = scene.CreateObject("Host");
    const std::string path = WriteSage("SageEngineHost", R"SAGE(
class Spinner extends Component
    public turns: integer = 0
    function onUpdate(dt: number)
        self.turns = self.turns + 1
        self.transform:Translate(Vector3(0, 1, 0))
    end
end

class SageEngineHost extends Component
    function onStart()
        local s := self.entity:addComponent(Spinner)
        s.turns = 100
    end

    function onUpdate(dt: number)
        local s = self.entity:getComponent(Spinner)
        if s ~= nil and s.turns >= 101 then
            self.transform:Translate(Vector3(1, 0, 0))
        end
    end
end
)SAGE");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});
    auto sys = MakeSystem(scene);
    int errors = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        ++errors;
        std::printf("  %s\n", e.Format().c_str());
    });
    CHECK_EQ(sys->AttachScene(scene), 1);
    sys->Update(0.016f);
    sys->Update(0.016f);
    // Spinner шёл каждый кадр (y = 2), хозяин увидел его поле (x >= 1).
    CHECK_NEAR(obj.GetTransform().Position.y, 2.0f, 1e-4f);
    CHECK_TRUE(obj.GetTransform().Position.x >= 1.0f);
    CHECK_EQ(errors, 0);
    sys->Shutdown();
    std::filesystem::remove(path);
}

// import между файлами проекта — в работающей игре, через загрузчик движка.
TEST(SageEngine_import_from_another_file_at_runtime) {
    sage::lang::ClearCompileCache();
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_engine_import";
    fs::remove_all(dir);
    fs::create_directories(dir / "Game");
    {
        std::ofstream f(dir / "Game" / "Tuning.slua", std::ios::binary);
        f << "export const STEP: number = 3\nexport function twice(x: number): number\n    return x * 2\nend\n";
    }
    const std::string path = (dir / "SageEngineUser.slua").generic_string();
    {
        std::ofstream f(path, std::ios::binary);
        f << R"SAGE(import { STEP, twice } from "Game/Tuning"

class SageEngineUser extends Component
    function onStart()
        self.transform:Translate(Vector3(twice(STEP), 0, 0))
    end
end
)SAGE";
    }
    Scene scene("test");
    GameObject obj = scene.CreateObject("User");
    scene.Registry().emplace<ScriptComponent>(obj.Entity(), ScriptComponent{path});
    auto sys = MakeSystem(scene);
    int errors = 0;
    sys->Runtime().SetErrorSink([&](const ScriptError& e) {
        ++errors;
        std::printf("  %s\n", e.Format().c_str());
    });
    CHECK_EQ(sys->AttachScene(scene), 1);
    CHECK_NEAR(obj.GetTransform().Position.x, 6.0f, 1e-4f);
    CHECK_EQ(errors, 0);
    sys->Shutdown();
    fs::remove_all(dir);
}

// Пример из поставки ЗАПУСКАЕТСЯ, а не только читается: по нему пишут первый
// скрипт, и пример с ошибкой выглядит как сломанный движок.
TEST(SageEngine_shipped_example_compiles_and_runs) {
    sage::lang::ClearCompileCache();
    std::filesystem::path script;
    std::filesystem::path dir = std::filesystem::current_path();
    for (int i = 0; i < 4; ++i) {
        const std::filesystem::path candidate = dir / "editor/assets/scripts/examples/PlayerController.slua";
        if (std::filesystem::exists(candidate)) { script = candidate; break; }
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    CHECK_TRUE(!script.empty());
    if (script.empty()) return;

    Scene scene("test");
    GameObject hero = scene.CreateObject("Hero");
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity());
    scene.Registry().emplace<ScriptComponent>(hero.Entity(), ScriptComponent{script.generic_string()});
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
        std::printf("  %s\n", e.Format().c_str());
    });
    CHECK_EQ(sys->AttachScene(scene), 1);
    for (int i = 0; i < 3; ++i) {
        input.BeginFrame();
        input.UpdateActions(1.0f / 60.0f);
        sys->Update(1.0f / 60.0f);
    }
    CHECK_EQ(errors, 0);
    sys->Shutdown();
}
