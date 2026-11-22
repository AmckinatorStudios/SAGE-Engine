// РЕЕСТР КОМПОНЕНТОВ (sage/scene/ComponentRegistry.h): компонент игры
// сохраняется, читается, копируется и переписывает ссылки сам — без правки
// движка. И чужой компонент файла не теряется у программы, которая его не знает.
#include "TestFramework.h"

#include <filesystem>
#include <stdexcept>

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

// --- Надёжность реестра и операций над компонентами --------------------------

#include <atomic>
#include <thread>
#include <vector>

namespace {
struct Fragile { int N = 0; };
struct Plain { int N = 0; };

// Тип, описанный «руками»: Load заводит компонент и падает на полпути — так
// ведёт себя наивная пользовательская загрузка.
sage::scene::ComponentType HalfLoader() {
    sage::scene::ComponentType t;
    t.Key = "test_half";
    t.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<Plain>(e); };
    t.Save = [](const entt::registry& r, entt::entity e) { return nlohmann::json{{"n", r.get<Plain>(e).N}}; };
    t.Load = [](entt::registry& r, entt::entity e, const nlohmann::json& j) {
        r.emplace_or_replace<Plain>(e);
        r.get<Plain>(e).N = j.at("n").get<int>();   // нет поля — бросает ПОСЛЕ emplace
    };
    t.Remove = [](entt::registry& r, entt::entity e) { r.remove<Plain>(e); };
    return t;
}
} // namespace

TEST(ComponentRegistry_rejects_incomplete_types) {
    sage::scene::ComponentType t;
    CHECK_FALSE(sage::scene::RegisterComponent(t));   // нет ключа
    t = HalfLoader();
    t.Remove = nullptr;
    CHECK_FALSE(sage::scene::RegisterComponent(t));   // нет Remove — откатить нечем
    CHECK_TRUE(sage::scene::FindComponentType("test_half") == nullptr);
}

// Описание, взятое ДО замены и снятия, остаётся целым: это shared_ptr на
// неизменяемую запись, а не указатель внутрь вектора реестра.
TEST(ComponentRegistry_held_type_survives_replace_and_unregister) {
    CHECK_TRUE(sage::scene::RegisterComponent(HalfLoader()));
    sage::scene::ComponentTypePtr held = sage::scene::FindComponentType("test_half");
    CHECK_TRUE(held != nullptr);
    sage::scene::ComponentType replacement = HalfLoader();
    replacement.VisitRefs = [](entt::registry&, entt::entity, const sage::scene::RefVisit&) {};
    CHECK_TRUE(sage::scene::RegisterComponent(replacement));
    CHECK_TRUE(sage::scene::UnregisterComponent("test_half"));
    for (int i = 0; i < 64; ++i) sage::scene::RegisterComponent(HalfLoader());   // реестр растёт
    sage::scene::UnregisterComponent("test_half");
    CHECK_TRUE(held->Key == "test_half" && !held->VisitRefs);
    CHECK_TRUE(sage::scene::FindComponentType("test_half") == nullptr);
    // Ровно одна запись при повторной регистрации.
    sage::scene::RegisterComponent(HalfLoader());
    sage::scene::RegisterComponent(HalfLoader());
    int count = 0;
    for (const auto& t : sage::scene::ComponentTypes())
        if (t->Key == "test_half") ++count;
    CHECK_EQ(count, 1);
    sage::scene::UnregisterComponent("test_half");
}

// Load упал после emplace — недочитанного компонента нет, JSON сохранён.
TEST(ComponentRegistry_failed_load_leaves_no_partial_component) {
    CHECK_TRUE(sage::scene::RegisterComponent(HalfLoader()));
    const std::string file = R"({"name": "S", "objects": [{"id": 1, "name": "X", "components": {"test_half": {"m": 7}}}]})";
    auto scene = SceneSerializer::LoadFromString(SceneSerializer::MigrateSceneJson(file));
    GameObject x = scene->FindByName("X");
    CHECK_TRUE(x.Valid());
    CHECK_FALSE(scene->Registry().all_of<Plain>(x.Entity()));
    const nlohmann::json saved = nlohmann::json::parse(SceneSerializer::SaveToString(*scene));
    CHECK_TRUE(saved["objects"][0]["components"]["test_half"]["m"] == 7);
    sage::scene::UnregisterComponent("test_half");
}

// Ошибка Save не пропускается молча: SaveToString бросает с ключом компонента.
TEST(ComponentRegistry_save_error_is_reported_not_skipped) {
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<Fragile>(
        "test_fragile", [](const Fragile&) -> nlohmann::json { throw std::runtime_error("нет"); },
        [](const nlohmann::json&, Fragile&) {}));
    Scene s("S");
    GameObject o = s.CreateObject("O");
    s.Registry().emplace<Fragile>(o.Entity());
    bool threw = false;
    try {
        (void)SceneSerializer::SaveToString(s);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("test_fragile") != std::string::npos;
    }
    CHECK_TRUE(threw);
    sage::scene::UnregisterComponent("test_fragile");
}

// Копия поддерева упала на внуке — в сцене ни одного нового объекта, ссылки
// оригинала не тронуты; префаб с таким компонентом не ставится вовсе (-1).
TEST(ComponentRegistry_failed_copy_rolls_back_whole_subtree) {
    auto t = sage::scene::MakeComponentType<Fragile>(
        "test_fragile", [](const Fragile& f) { return nlohmann::json{{"n", f.N}}; },
        [](const nlohmann::json& j, Fragile& f) { f.N = j.value("n", 0); });
    t.Copy = [](const entt::registry&, entt::entity, entt::registry& d, entt::entity de) {
        d.emplace_or_replace<Fragile>(de);   // завёл — и упал
        throw std::runtime_error("копия не удалась");
    };
    CHECK_TRUE(sage::scene::RegisterComponent(t));
    RegisterHealth();
    Scene scene("S");
    GameObject root = scene.CreateObject("Root");
    GameObject mid = scene.CreateObject("Mid");
    GameObject leaf = scene.CreateObject("Leaf");
    scene.SetParent(mid.Entity(), root.Entity());
    scene.SetParent(leaf.Entity(), mid.Entity());
    TestHealth hp;
    hp.LastHitBy.Id = leaf.Id();
    scene.Registry().emplace<TestHealth>(root.Entity(), hp);
    scene.Registry().emplace<Fragile>(leaf.Entity());
    const size_t before = scene.Count();
    bool threw = false;
    try {
        (void)sage::scene::CopySubtree(scene, root.Entity(), scene, entt::null);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
    CHECK_EQ(scene.Count(), before);
    size_t alive = 0;
    for (auto e : scene.Registry().view<IdComponent>()) { (void)e; ++alive; }
    CHECK_EQ(alive, before);
    CHECK_EQ(scene.Registry().get<TestHealth>(root.Entity()).LastHitBy.Id, leaf.Id());

    // Префаб: сохраняется (Save работает), а постановка откатывается целиком.
    const std::string path = (std::filesystem::temp_directory_path() / "sage_test_fragile.prefab").string();
    std::string err;
    sage::scene::ComponentType plain = t;
    plain.Copy = nullptr;   // для сохранения префаба копия должна пройти
    sage::scene::RegisterComponent(plain);
    CHECK_TRUE(sage::scene::SavePrefab(scene, root.Entity(), path, err));
    sage::scene::RegisterComponent(t);
    sage::scene::ClearPrefabCache();
    Scene target("T");
    target.CreateObject("Existing");
    CHECK_EQ(sage::scene::InstantiatePrefab(target, path), -1);
    CHECK_EQ(target.Count(), (size_t)1);
    std::filesystem::remove(path);
    sage::scene::ClearPrefabCache();
    sage::scene::UnregisterComponent("test_fragile");
    sage::scene::UnregisterComponent("test_health");
}

// Пользовательские функции зовутся ВНЕ блокировки: Save, сам обращающийся к
// реестру, не зависает; параллельные регистрация, поиск и обход не гоняются.
TEST(ComponentRegistry_callbacks_run_unlocked_and_concurrent_use_is_safe) {
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<Fragile>(
        "test_reentrant",
        [](const Fragile& f) {
            (void)sage::scene::FindComponentType("test_reentrant");
            sage::scene::RegisterComponent(sage::scene::MakeComponentType<Plain>(
                "test_from_cb", [](const Plain&) { return nlohmann::json::object(); },
                [](const nlohmann::json&, Plain&) {}));
            return nlohmann::json{{"n", f.N}};
        },
        [](const nlohmann::json& j, Fragile& f) { f.N = j.value("n", 0); }));
    Scene s("S");
    GameObject o = s.CreateObject("O");
    s.Registry().emplace<Fragile>(o.Entity(), Fragile{5});
    CHECK_TRUE(SceneSerializer::SaveToString(s).find("test_reentrant") != std::string::npos);

    std::atomic<bool> bad{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t, &bad] {
            for (int i = 0; i < 300; ++i) {
                const std::string key = "test_mt_" + std::to_string(t) + "_" + std::to_string(i % 5);
                sage::scene::RegisterComponent(sage::scene::MakeComponentType<Plain>(
                    key, [](const Plain&) { return nlohmann::json::object(); }, [](const nlohmann::json&, Plain&) {}));
                auto found = sage::scene::FindComponentType(key);
                if (!found || found->Key != key) bad = true;
                for (const auto& type : sage::scene::ComponentTypes())
                    if (!type || type->Key.empty()) bad = true;
                if (i % 3 == 0) sage::scene::UnregisterComponent(key);
            }
        });
    }
    // Сохранение сцены параллельно с правкой реестра.
    for (int i = 0; i < 50; ++i) (void)SceneSerializer::SaveToString(s);
    for (std::thread& th : threads) th.join();
    CHECK_FALSE(bad.load());
    for (int t = 0; t < 4; ++t)
        for (int k = 0; k < 5; ++k) sage::scene::UnregisterComponent("test_mt_" + std::to_string(t) + "_" + std::to_string(k));
    sage::scene::UnregisterComponent("test_reentrant");
    sage::scene::UnregisterComponent("test_from_cb");
    // Встроенные на месте и по одному разу.
    int ai = 0;
    for (const auto& type : sage::scene::ComponentTypes())
        if (type->Key == "ai_agent") ++ai;
    CHECK_EQ(ai, 1);
}
