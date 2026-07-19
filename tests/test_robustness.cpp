// Регрессионные тесты на устойчивость движка к битым данным и злоупотреблениям
// из скриптов — по итогам аудита: дубликаты id в файле сцены, битые/отсутствующие
// модели, циклы OnMessage-обработчиков, таймеры из колбэков таймеров. Всё на
// CPU, БЕЗ GL (пути подобраны так, чтобы не создавать GPU-ресурсы).
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/render/ResourceManager.h"
#include "sage/scripting/ScriptEngine.h"

namespace {
std::string WriteTempScript(const std::string& name, const std::string& body) {
    std::string path = std::string("sage_robust_") + name + ".lua";
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}
} // namespace

TEST(Robust_duplicate_id_gets_fresh_id) {
    Scene scene("R");
    GameObject a = scene.CreateObjectWithId("A", 7);
    GameObject b = scene.CreateObjectWithId("B", 7); // дубликат — обязан получить свежий id
    CHECK_EQ(a.Id(), 7);
    CHECK_TRUE(b.Id() != 7);
    CHECK_EQ((int)scene.Count(), 2);
    // Обе достижимы по своим id (раньше первая становилась недостижимой).
    CHECK_TRUE(scene.Get(a.Id()).Valid());
    CHECK_TRUE(scene.Get(b.Id()).Valid());
    // Удаление по id работает для обеих — утечки в registry нет.
    scene.RemoveObject(a.Id());
    scene.RemoveObject(b.Id());
    CHECK_EQ((int)scene.Count(), 0);
}

TEST(Robust_scene_load_with_duplicate_ids) {
    const char* json = R"({
        "sage_scene_version": 1, "name": "Dup",
        "objects": [
            {"id": 3, "name": "First",  "mesh": {"type": "none", "path": ""}},
            {"id": 3, "name": "Second", "mesh": {"type": "none", "path": ""}}
        ]
    })";
    auto scene = SceneSerializer::LoadFromString(json);
    CHECK_EQ((int)scene->Count(), 2);
    // Обе сущности живы и имена не потеряны.
    CHECK_TRUE(scene->FindByName("First").Valid());
    CHECK_TRUE(scene->FindByName("Second").Valid());
    // Новые объекты не конфликтуют id с загруженными.
    GameObject fresh = scene->CreateObject("Fresh");
    CHECK_TRUE(scene->Get(fresh.Id()).Valid());
}

TEST(Robust_missing_model_does_not_abort_scene_load) {
    // Сцена ссылается на несуществующий .obj — раньше исключение лоадера
    // обрывало загрузку ВСЕЙ сцены. Теперь сущность просто без меша.
    const char* json = R"({
        "sage_scene_version": 1, "name": "BadModel",
        "objects": [
            {"id": 1, "name": "Broken", "mesh": {"type": "model", "path": "no_such_model_12345.obj"}},
            {"id": 2, "name": "Ok",     "mesh": {"type": "none",  "path": ""}}
        ]
    })";
    auto scene = SceneSerializer::LoadFromString(json);
    CHECK_EQ((int)scene->Count(), 2);
    GameObject broken = scene->FindByName("Broken");
    CHECK_TRUE(broken.Valid());
    CHECK_TRUE(broken.Renderer().MeshPtr == nullptr); // битая модель -> без меша, не краш
    CHECK_TRUE(scene->FindByName("Ok").Valid());
}

TEST(Robust_get_model_negative_cache_no_throw) {
    auto& rm = ResourceManager::Instance();
    // Не бросает и возвращает nullptr; повторный запрос — из негативного кэша.
    auto m1 = rm.GetModel("no_such_model_12345.obj");
    auto m2 = rm.GetModel("no_such_model_12345.obj");
    CHECK_TRUE(m1 == nullptr);
    CHECK_TRUE(m2 == nullptr);
}

TEST(Robust_message_cycle_is_bounded) {
    ScriptEngine se;
    Scene scene("R");
    se.BindScene(scene);

    int calls = 0;
    se.Lua().set_function("TestTick", [&] { ++calls; });

    // OnMessage безусловно рассылает дальше — без ограничения глубины это
    // бесконечная рекурсия и переполнение C++-стека.
    std::string path = WriteTempScript("cycle",
        "function OnMessage(entity, name, data)\n"
        "    TestTick()\n"
        "    Broadcast('ping')\n"
        "end\n");
    GameObject a = scene.CreateObject("A");
    se.AttachScript(a, path);

    se.Lua().script("Broadcast('ping')"); // должен ЗАВЕРШИТЬСЯ, а не упасть
    CHECK_TRUE(calls >= 1);
    CHECK_TRUE(calls <= 32); // глубина ограничена kMaxMessageDepth (16)

    std::remove(path.c_str());
}

TEST(Robust_timer_scheduled_in_callback_waits_full_interval) {
    ScriptEngine se;
    // Вложенный Schedule из колбэка таймера НЕ должен тикать в том же кадре —
    // иначе он теряет один dt и срабатывает на кадр раньше срока.
    se.Lua().script(
        "inner_fired = 0\n"
        "Schedule(0.05, function()\n"
        "    Schedule(0.05, function() inner_fired = 1 end)\n"
        "end)\n");

    se.UpdateAll(0.06f); // внешний сработал, внутренний только создан
    int afterFirst = se.Lua()["inner_fired"];
    CHECK_EQ(afterFirst, 0);

    se.UpdateAll(0.06f); // теперь срок внутреннего
    int afterSecond = se.Lua()["inner_fired"];
    CHECK_EQ(afterSecond, 1);
}
