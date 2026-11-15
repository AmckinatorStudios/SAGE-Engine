// ===========================================================================
//  СЛОИ СЦЕНЫ И ТЕГИ (sage/scene/SceneLayer.h): назначение объекта, список
//  слоёв сцены, переименование и удаление, запись в файл сцены.
// ===========================================================================
#include "TestFramework.h"

#include "sage/scene/Scene.h"
#include "sage/scene/SceneComponents.h"
#include "sage/scene/SceneLayer.h"
#include "sage/scene/SceneSerializer.h"

namespace sc = sage::scene;

TEST(SceneLayers_assign_rename_remove) {
    Scene scene("layers");
    GameObject a = scene.CreateEmptyObject("A");
    GameObject b = scene.CreateEmptyObject("B");
    CHECK_TRUE(sc::AddSceneLayer(scene, "Enemies"));
    CHECK_TRUE(!sc::AddSceneLayer(scene, "Enemies"));   // имя занято
    CHECK_TRUE(!sc::AddSceneLayer(scene, ""));
    sc::SetLayerOf(scene, a.Id(), "Enemies");
    sc::SetLayerOf(scene, b.Id(), "Lights");             // слой заводится сам
    CHECK_EQ(sc::LayerOf(scene, a.Id()), std::string("Enemies"));
    CHECK_EQ((int)sc::SceneLayerNames(scene).size(), 2);
    // Переименование — и у объектов; в занятое имя — нельзя.
    CHECK_TRUE(!sc::RenameSceneLayer(scene, "Enemies", "Lights"));
    CHECK_TRUE(sc::RenameSceneLayer(scene, "Enemies", "Foes"));
    CHECK_EQ(sc::LayerOf(scene, a.Id()), std::string("Foes"));
    // Удаление — объекты остаются без слоя.
    sc::RemoveSceneLayer(scene, "Foes");
    CHECK_EQ(sc::LayerOf(scene, a.Id()), std::string());
    CHECK_EQ((int)sc::SceneLayerNames(scene).size(), 1);
    // Снять слой с объекта — пустым именем; слой в списке остаётся.
    sc::SetLayerOf(scene, b.Id(), "");
    CHECK_EQ(sc::LayerOf(scene, b.Id()), std::string());
    CHECK_EQ((int)sc::SceneLayerNames(scene).size(), 1);
}

TEST(SceneLayers_and_tags_survive_the_scene_file) {
    Scene scene("layers");
    GameObject a = scene.CreateEmptyObject("A");
    GameObject b = scene.CreateEmptyObject("B");
    sc::AddSceneLayer(scene, "Empty layer");             // без объектов — тоже сохраняется
    sc::SetLayerOf(scene, a.Id(), "Props");
    scene.Registry().emplace<TagComponent>(a.Entity(), TagComponent{"Enemy"});
    scene.Registry().emplace<TagComponent>(b.Entity(), TagComponent{"Coin"});
    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    const std::vector<std::string> names = sc::SceneLayerNames(*back);
    CHECK_EQ((int)names.size(), 2);
    CHECK_EQ(names[0], std::string("Empty layer"));
    GameObject a2 = back->FindByName("A");
    CHECK_EQ(sc::LayerOf(*back, a2.Id()), std::string("Props"));
    // Подсказки тегов — теги сцены по алфавиту, без повторов.
    const std::vector<std::string> tags = sc::SceneTags(*back);
    CHECK_EQ((int)tags.size(), 2);
    CHECK_EQ(tags[0], std::string("Coin"));
}
