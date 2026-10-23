// РЕЕСТР КОМПОНЕНТОВ (sage/scene/ComponentRegistry.h): компонент игры
// сохраняется, читается, копируется и переписывает ссылки сам — без правки
// движка. И чужой компонент файла не теряется у программы, которая его не знает.
#include "TestFramework.h"

#include <nlohmann/json.hpp>

#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace {

struct TestHealth {
    float Value = 100.0f;
    sage::vars::EntityRef LastHitBy;
};

void RegisterHealth() {
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<TestHealth>(
        "test_health",
        [](const TestHealth& h) { return nlohmann::json{{"value", h.Value}, {"lastHitBy", h.LastHitBy.Id}}; },
        [](const nlohmann::json& j, TestHealth& h) {
            h.Value = j.value("value", h.Value);
            h.LastHitBy.Id = j.value("lastHitBy", 0);
        },
        [](TestHealth& h, const sage::scene::RefVisit& visit) { visit(h.LastHitBy); }));
}

} // namespace

TEST(ComponentRegistry_game_component_is_saved_loaded_and_copied) {
    RegisterHealth();
    Scene scene("S");
    GameObject enemy = scene.CreateObject("Enemy");
    GameObject sword = scene.CreateObject("Sword");
    scene.SetParent(sword.Entity(), enemy.Entity());
    TestHealth hp;
    hp.Value = 42.5f;
    hp.LastHitBy.Id = sword.Id();   // ссылка ВНУТРИ поддерева
    scene.Registry().emplace<TestHealth>(enemy.Entity(), hp);

    // Файл: свой ключ в objects[i].components.
    const std::string text = SceneSerializer::SaveToString(scene);
    const nlohmann::json j = nlohmann::json::parse(text);
    bool written = false;
    for (const auto& o : j["objects"])
        if (o.contains("components") && o["components"].contains("test_health")) written = true;
    CHECK_TRUE(written);
    auto loaded = SceneSerializer::LoadFromString(text);
    GameObject e2 = loaded->FindByName("Enemy");
    CHECK_TRUE(e2.Valid() && loaded->Registry().all_of<TestHealth>(e2.Entity()));
    if (e2.Valid() && loaded->Registry().all_of<TestHealth>(e2.Entity()))
        CHECK_NEAR(loaded->Registry().get<TestHealth>(e2.Entity()).Value, 42.5f, 1e-5f);

    // Копия (Ctrl+D, префаб): компонент едет, ссылка ведёт в КОПИЮ меча.
    GameObject copy = sage::scene::CopySubtree(scene, enemy.Entity(), scene, entt::null);
    CHECK_TRUE(scene.Registry().all_of<TestHealth>(copy.Entity()));
    const auto* ch = scene.Registry().try_get<TestHealth>(copy.Entity());
    const auto* hier = scene.Registry().try_get<HierarchyComponent>(copy.Entity());
    CHECK_TRUE(ch && hier && !hier->Children.empty());
    if (ch && hier && !hier->Children.empty()) {
        const int copiedSword = scene.Registry().get<IdComponent>(hier->Children[0]).Id;
        CHECK_EQ(ch->LastHitBy.Id, copiedSword);
        CHECK_TRUE(ch->LastHitBy.Id != sword.Id());
    }
    sage::scene::UnregisterComponent("test_health");
}

// Программа без кода игры (редактор без её модуля, старая версия) открывает
// и сохраняет сцену — компонент игры обязан дожить до следующего открытия.
TEST(ComponentRegistry_unknown_component_survives_load_save_and_copy) {
    sage::scene::UnregisterComponent("test_health");
    const std::string file = R"({"sage_scene_version": 7, "name": "S", "objects": [
        {"id": 1, "name": "Boss", "components": {"game_ai": {"state": "angry", "aggro": [1, 2, 3]}}}
    ]})";
    auto scene = SceneSerializer::LoadFromString(SceneSerializer::MigrateSceneJson(file));
    GameObject boss = scene->FindByName("Boss");
    CHECK_TRUE(boss.Valid() && scene->Registry().all_of<sage::scene::UnknownComponents>(boss.Entity()));
    // Копия объекта тоже несёт его.
    GameObject copy = sage::scene::CopySubtree(*scene, boss.Entity(), *scene, entt::null);
    const nlohmann::json saved = nlohmann::json::parse(SceneSerializer::SaveToString(*scene));
    int found = 0;
    for (const auto& o : saved["objects"])
        if (o.contains("components") && o["components"].contains("game_ai") &&
            o["components"]["game_ai"]["state"] == "angry" && o["components"]["game_ai"]["aggro"].size() == 3)
            ++found;
    CHECK_EQ(found, 2);   // оригинал и копия
    (void)copy;

    // Код игры вернулся — тот же JSON читается уже её типом.
    struct Ai { std::string State; };
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<Ai>(
        "game_ai", [](const Ai& a) { return nlohmann::json{{"state", a.State}}; },
        [](const nlohmann::json& j, Ai& a) { a.State = j.value("state", std::string()); }));
    auto again = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*scene));
    GameObject b2 = again->FindByName("Boss");
    CHECK_TRUE(b2.Valid() && again->Registry().all_of<Ai>(b2.Entity()));
    if (b2.Valid() && again->Registry().all_of<Ai>(b2.Entity())) CHECK_TRUE(again->Registry().get<Ai>(b2.Entity()).State == "angry");
    sage::scene::UnregisterComponent("game_ai");
}

// Загрузчик игры упал на чужих данных — данные остаются как есть, а не
// пропадают со следующим сохранением.
TEST(ComponentRegistry_failed_load_keeps_the_data) {
    struct Strict { int N = 0; };
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<Strict>(
        "strict", [](const Strict& s) { return nlohmann::json{{"n", s.N}}; },
        [](const nlohmann::json& j, Strict& s) { s.N = j.at("n").get<int>(); }));   // нет поля — бросает
    const std::string file = R"({"name": "S", "objects": [{"id": 1, "name": "X", "components": {"strict": {"m": 5}}}]})";
    auto scene = SceneSerializer::LoadFromString(SceneSerializer::MigrateSceneJson(file));
    const nlohmann::json saved = nlohmann::json::parse(SceneSerializer::SaveToString(*scene));
    CHECK_TRUE(saved["objects"][0]["components"]["strict"]["m"] == 5);
    sage::scene::UnregisterComponent("strict");
}
