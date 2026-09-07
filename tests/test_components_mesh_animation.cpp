// ---------------------------------------------------------------------------
// Mesh и Animation: одна модель, два компонента.
//
// ЧТО ИМЕННО ПРОВЕРЯЕТСЯ. Раньше в сцене существовало два разных вида объекта:
// «модель» (MeshRenderer) и «анимированная модель» (AnimatedModel), у которых
// были свои поля, свой путь к файлу и разные возможности — у первой материалы и
// слоты подмешей, у второй скелет и клипы. Теперь вид объекта один: Mesh
// описывает, ЧТО рисуем, Animation — КАК ЭТО ДВИЖЕТСЯ, и скелет она берёт из
// модели, заданной в Mesh.
//
// Главная опасность такой перестройки — не в новом коде, а в СТАРЫХ СЦЕНАХ:
// у человека на диске лежат уровни с персонажами, записанные прежним блоком
// «animatedModel». Если они откроются без модели, работа пропадёт молча — и
// узнает он об этом, только открыв свой проект. Поэтому миграция проверяется
// первой и на настоящем тексте сцены, а не на структуре в памяти.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <memory>
#include <string>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace {

// Сцена, записанная ДО разделения компонентов: путь к модели и проигрывание
// лежат в одном блоке animatedModel, компонента mesh у объекта нет вовсе.
const char* kLegacyScene = R"({
  "version": 5,
  "name": "Legacy",
  "objects": [
    {
      "id": 1,
      "name": "Hero",
      "transform": {"position": [1.0, 0.0, 2.0], "rotation": [0,0,0], "scale": [1,1,1]},
      "animatedModel": {
        "path": "assets/models/hero.glb",
        "demoSegments": 6,
        "clip": 2,
        "speed": 1.5,
        "loop": false,
        "playing": true,
        "blendTime": 0.4,
        "rootMotion": true
      }
    }
  ]
})";

TEST(Legacy_animated_model_becomes_Mesh_plus_Animation) {
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(kLegacyScene);
    CHECK_TRUE(scene != nullptr);
    GameObject hero = scene->FindByName("Hero");
    CHECK_TRUE(hero.Valid());

    // Модель переехала в Mesh — там, где её теперь ищут и редактор, и рендер.
    const MeshRendererComponent* mesh =
        scene->Registry().try_get<MeshRendererComponent>(hero.Entity());
    CHECK_TRUE(mesh != nullptr);
    CHECK_TRUE(mesh->Ref.type == MeshRef::Type::Model);
    CHECK_EQ(mesh->Ref.path, std::string("assets/models/hero.glb"));

    // Проигрывание — в Animation, со всеми настройками автора сцены.
    const AnimationComponent* anim =
        scene->Registry().try_get<AnimationComponent>(hero.Entity());
    CHECK_TRUE(anim != nullptr);
    CHECK_EQ(anim->Clip, 2);
    CHECK_NEAR(anim->Speed, 1.5f, 1e-4f);
    CHECK_TRUE(!anim->Loop);
    CHECK_TRUE(anim->Playing);
    CHECK_NEAR(anim->BlendTime, 0.4f, 1e-4f);
    CHECK_TRUE(anim->RootMotion);
}

// Пересохранение мигрированной сцены обязано дать НОВЫЙ формат: иначе старый
// блок жил бы вечно, и каждая загрузка снова была бы миграцией.
TEST(Migrated_scene_saves_in_the_new_format) {
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(kLegacyScene);
    CHECK_TRUE(scene != nullptr);
    const std::string saved = SceneSerializer::SaveToString(*scene);
    CHECK_TRUE(saved.find("\"animation\"") != std::string::npos);
    CHECK_TRUE(saved.find("\"animatedModel\"") == std::string::npos);
    CHECK_TRUE(saved.find("assets/models/hero.glb") != std::string::npos);

    // И читается обратно уже как два компонента.
    std::unique_ptr<Scene> again = SceneSerializer::LoadFromString(saved);
    CHECK_TRUE(again != nullptr);
    GameObject hero = again->FindByName("Hero");
    CHECK_TRUE(hero.Valid());
    CHECK_TRUE(again->Registry().all_of<MeshRendererComponent>(hero.Entity()));
    CHECK_TRUE(again->Registry().all_of<AnimationComponent>(hero.Entity()));
}

// Anim.Path больше не существует, и это не косметика: два поля с одним и тем же
// путём (в Mesh и в Animation) однажды разъехались бы, и «модель в инспекторе
// одна, а анимируется другая» искали бы неделю. Проверяем, что источник правды
// один — Mesh.
TEST(Animation_takes_the_model_from_Mesh) {
    auto scene = std::make_unique<Scene>("MeshAnim");
    GameObject obj = scene->CreateObject("Character");
    MeshRendererComponent& mr = scene->Registry().emplace<MeshRendererComponent>(obj.Entity());
    mr.Ref.type = MeshRef::Type::Model;
    mr.Ref.path = "assets/models/first.glb";
    scene->Registry().emplace<AnimationComponent>(obj.Entity());

    const std::string saved = SceneSerializer::SaveToString(*scene);
    // Путь к модели встречается в файле РОВНО ОДИН раз — в блоке mesh.
    size_t count = 0;
    for (size_t pos = saved.find("first.glb"); pos != std::string::npos;
         pos = saved.find("first.glb", pos + 1)) {
        ++count;
    }
    CHECK_EQ((int)count, 1);
}

// Сцена, у которой Mesh уже задан, миграцией НЕ портится: путь из старого блока
// не является источником правды, если объект уже описывает свою модель сам.
TEST(Legacy_migration_does_not_overwrite_existing_mesh) {
    const char* both = R"({
      "version": 5, "name": "Both",
      "objects": [{
        "id": 1, "name": "Hero",
        "transform": {"position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1]},
        "mesh": {"type": "model", "path": "assets/models/correct.glb"},
        "animatedModel": {"path": "assets/models/stale.glb", "clip": 0}
      }]
    })";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(both);
    CHECK_TRUE(scene != nullptr);
    GameObject hero = scene->FindByName("Hero");
    CHECK_TRUE(hero.Valid());
    const MeshRendererComponent& mesh =
        scene->Registry().get<MeshRendererComponent>(hero.Entity());
    CHECK_EQ(mesh.Ref.path, std::string("assets/models/correct.glb"));
    CHECK_TRUE(scene->Registry().all_of<AnimationComponent>(hero.Entity()));
}

} // namespace
