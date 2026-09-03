// Кэш разобранных моделей: пишется ли он, читается ли обратно, и — главное —
// протухает ли, когда исходник изменился.
//
// ЗАЧЕМ ЭТИ ТЕСТЫ. У кэша ровно два способа сломаться, и оба тихие:
//
//   • он ПИШЕТСЯ, но не ЧИТАЕТСЯ. Внешне всё работает, просто каждый запуск
//     заново разбирает модель — и заодно каждый раз перезаписывает кэш,
//     который никто не прочтёт. Заметить это можно только по времени загрузки;
//
//   • он читается, КОГДА НЕ ДОЛЖЕН. Исходник поменяли, а на экране прежняя
//     модель. Это выглядит как «правка не применилась», и ищут её в чём
//     угодно, кроме кэша.
//
// До этих тестов кэш не был покрыт ничем.
#include "TestFramework.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "sage/assets/AssetCache.h"
#include "sage/render/Material.h"
#include "sage/render/ModelData.h"
#include "sage/render/ResourceManager.h"

namespace fs = std::filesystem;

namespace {

fs::path MakeDir(const char* name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / name;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// Исходник-пустышка: кэш смотрит на его размер и время правки, а не на
// содержимое, поэтому разбирать его никто не будет.
void WriteSource(const fs::path& file, const std::string& text) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << text;
}

sage::render::ModelData SampleModel() {
    sage::render::ModelData data;
    data.Skeleton.Joints.resize(2);
    data.Skeleton.Joints[0].Name = "root";
    data.Skeleton.Joints[0].Parent = -1;
    data.Skeleton.Joints[1].Name = "hand";
    data.Skeleton.Joints[1].Parent = 0;
    data.SubMeshes.resize(1);
    data.SubMeshes[0].Vertices.resize(3);
    data.SubMeshes[0].Indices = {0, 1, 2};
    return data;
}

} // namespace

TEST(AssetCache_writes_and_reads_back) {
    const fs::path dir = MakeDir("sage_cache_rt");
    sage::assets::SetCacheDirectory((dir / "cache").string());
    sage::assets::SetCacheEnabled(true);

    const fs::path source = dir / "hero.gltf";
    WriteSource(source, "исходник модели");

    const sage::render::ModelData written = SampleModel();
    CHECK_TRUE(sage::assets::WriteModelCache(source.string(), written));

    sage::render::ModelData readBack;
    CHECK_TRUE(sage::assets::ReadModelCache(source.string(), readBack));
    CHECK_EQ((int)readBack.Skeleton.Joints.size(), 2);
    if (readBack.Skeleton.Joints.size() == 2) {
        CHECK_TRUE(readBack.Skeleton.Joints[1].Name == "hand");
        CHECK_EQ(readBack.Skeleton.Joints[1].Parent, 0);
    }
    CHECK_EQ((int)readBack.SubMeshes.size(), 1);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(AssetCache_goes_stale_when_the_source_changes) {
    const fs::path dir = MakeDir("sage_cache_stale");
    sage::assets::SetCacheDirectory((dir / "cache").string());
    sage::assets::SetCacheEnabled(true);

    const fs::path source = dir / "hero.gltf";
    WriteSource(source, "первая версия");
    CHECK_TRUE(sage::assets::WriteModelCache(source.string(), SampleModel()));

    sage::render::ModelData first;
    CHECK_TRUE(sage::assets::ReadModelCache(source.string(), first));

    // Меняем исходник. Размер ДРУГОЙ намеренно: на файловых системах с грубым
    // разрешением времени правка в ту же секунду не меняет время, и проверка
    // «только по времени» пропустила бы изменение. Кэш обязан заметить.
    WriteSource(source, "вторая версия, заметно длиннее первой");

    sage::render::ModelData afterEdit;
    CHECK_FALSE(sage::assets::ReadModelCache(source.string(), afterEdit));
}

// САМОЕ ВАЖНОЕ ЗДЕСЬ. Один и тот же файл движок называет по-разному: сцена
// хранит путь относительно проекта ("assets/hero.gltf"), файловый диалог отдаёт
// абсолютный, а текущий каталог у редактора и у собранной игры разные.
//
// Если ключ кэша считается от НАПИСАНИЯ пути, то запись и чтение попадают в
// разные файлы: кэш исправно пишется и никогда не читается. Внешне это выглядит
// как «кэш не работает», а на диске растёт по копии на каждое написание.
TEST(AssetCache_key_does_not_depend_on_how_the_path_is_spelled) {
    const fs::path dir = MakeDir("sage_cache_spelling");
    sage::assets::SetCacheDirectory((dir / "cache").string());
    sage::assets::SetCacheEnabled(true);

    fs::create_directories(dir / "assets");
    const fs::path source = dir / "assets" / "hero.gltf";
    WriteSource(source, "исходник модели");

    std::error_code ec;
    const fs::path saved = fs::current_path(ec);
    fs::current_path(dir, ec);

    // Пишем по ОТНОСИТЕЛЬНОМУ пути — так его хранит сцена.
    CHECK_TRUE(sage::assets::WriteModelCache("assets/hero.gltf", SampleModel()));

    // Читаем по АБСОЛЮТНОМУ — так его отдаёт файловый диалог.
    sage::render::ModelData byAbsolute;
    const bool absoluteHit = sage::assets::ReadModelCache(source.string(), byAbsolute);

    // И по пути с «./» и лишним сегментом — так он приходит из склейки каталогов.
    sage::render::ModelData byWeird;
    const bool weirdHit = sage::assets::ReadModelCache("./assets/../assets/hero.gltf", byWeird);

    fs::current_path(saved, ec);
    fs::remove_all(dir, ec);

    CHECK_TRUE(absoluteHit);
    CHECK_TRUE(weirdHit);
}

TEST(AssetCache_disabled_means_disabled) {
    const fs::path dir = MakeDir("sage_cache_off");
    sage::assets::SetCacheDirectory((dir / "cache").string());
    sage::assets::SetCacheEnabled(true);

    const fs::path source = dir / "hero.gltf";
    WriteSource(source, "исходник");
    CHECK_TRUE(sage::assets::WriteModelCache(source.string(), SampleModel()));

    // Выключенный кэш не должен ни читать, ни писать: иначе сравнение «с кэшем
    // и без» ничего не сравнивает.
    sage::assets::SetCacheEnabled(false);
    sage::render::ModelData data;
    CHECK_FALSE(sage::assets::ReadModelCache(source.string(), data));
    CHECK_FALSE(sage::assets::WriteModelCache(source.string(), SampleModel()));

    sage::assets::SetCacheEnabled(true);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --- Кэш ресурсов В ПАМЯТИ: замечает ли правку файла -------------------------
//
// Вторая половина той же беды. Кэш ресурсов хранит уже загруженный объект по
// пути и НЕ смотрит на файл. Пока правки шли только через редактор, это
// сходило — он сам звал ReloadModel. Но материал правят и снаружи: другим
// редактором, скриптом, из Blender. Тогда движок продолжал показывать ПРЕЖНИЕ
// данные до перезапуска, и выглядело это как «правка не применилась».
//
// Материал, а не модель: он читается из текстового файла без графического
// контекста, поэтому проверяется на CPU. Механизм у обоих один.
TEST(ResourceManager_reloads_a_material_edited_outside_the_editor) {
    const fs::path dir = MakeDir("sage_reload");
    std::error_code ec;
    const fs::path saved = fs::current_path(ec);
    fs::current_path(dir, ec);

    const fs::path file = dir / "wall.sagemat";
    // Цвет — МАССИВОМ [r,g,b]: таков формат .sagemat. Раньше здесь стоял
    // объект {"x":..}, который читатель законно игнорирует, — и проверки
    // альбедо ниже сравнивали значение по умолчанию само с собой.
    { std::ofstream f(file); f << R"({"albedo":[1.0,0.0,0.0],"metallic":0.0})"; }

    ResourceManager& rm = ResourceManager::Instance();
    std::shared_ptr<Material> first = rm.GetMaterial("wall.sagemat");
    CHECK_TRUE(first != nullptr);
    if (first) {
        CHECK_NEAR(first->Albedo.x, 1.0f, 1e-4);
        CHECK_NEAR(first->Albedo.y, 0.0f, 1e-4); // а не «единица по умолчанию»
    }

    // Тот же путь — обязан прийти ТОТ ЖЕ объект: на него ссылаются компоненты
    // сцены, и подменять указатель при каждом обращении нельзя.
    CHECK_TRUE(rm.GetMaterial("wall.sagemat") == first);

    // Правим файл снаружи. Спим, чтобы время правки заведомо отличалось: на
    // файловых системах с разрешением в секунду две записи подряд неотличимы,
    // и тест ловил бы не дефект, а гранулярность часов.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    { std::ofstream f(file); f << R"({"albedo":[0.0,1.0,0.0],"metallic":0.5})"; }

    const int reloaded = rm.ReloadChangedAssets();
    CHECK_TRUE(reloaded >= 1);

    // И главное: правка видна ЧЕРЕЗ ПРЕЖНИЙ указатель. Если бы мы просто
    // положили в кэш новый объект, сцена осталась бы со старым материалом —
    // то есть кэш починился бы, а картинка нет.
    CHECK_NEAR(first->Albedo.x, 0.0f, 1e-4);
    CHECK_NEAR(first->Albedo.y, 1.0f, 1e-4);
    CHECK_NEAR(first->Metallic, 0.5f, 1e-4);

    // Без правок повторный вызов ничего не перечитывает: иначе он делал бы это
    // каждый кадр и перезагружал бы всю сцену на ровном месте.
    CHECK_EQ(rm.ReloadChangedAssets(), 0);

    rm.Clear();
    fs::current_path(saved, ec);
    fs::remove_all(dir, ec);
}

// ОДИН ФАЙЛ — ОДИН МАТЕРИАЛ, как бы к нему ни обратились.
//
// Из-за этого «редактор материалов не работал». Сущность держит ссылку
// ОТНОСИТЕЛЬНО ПРОЕКТА ("assets/wall.sagemat" — так пишет Project::AssetRef, и
// так она уезжает в .sage), а панель ассетов работает НАСТОЯЩИМ путём в
// файловой системе ("<проект>/assets/wall.sagemat"). Ключом кэша был путь «как
// дали», поэтому у одного файла заводилось ДВА объекта Material: редактор
// правил один, а рендер рисовал другой. Со стороны это выглядело как «правки
// не применяются» — и не применялись они никогда, ни к одному полю.
//
// Проверяется самое сильное утверждение, какое здесь можно сделать: любые
// написания пути к одному файлу дают ОДИН И ТОТ ЖЕ указатель.
TEST(ResourceManager_one_file_is_one_material_however_it_is_spelled) {
    const fs::path dir = MakeDir("sage_matkey");
    std::error_code ec;
    const fs::path saved = fs::current_path(ec);
    fs::current_path(dir, ec);

    fs::create_directories("assets", ec);
    { std::ofstream f("assets/wall.sagemat"); f << R"({"albedo":[1.0,0.0,0.0]})"; }

    ResourceManager& rm = ResourceManager::Instance();
    std::shared_ptr<Material> byRef = rm.GetMaterial("assets/wall.sagemat");
    CHECK_TRUE(byRef != nullptr);

    // Те же самые байты на диске, четыре способа их назвать.
    CHECK_TRUE(rm.GetMaterial("./assets/wall.sagemat") == byRef);
    CHECK_TRUE(rm.GetMaterial("assets/../assets/wall.sagemat") == byRef);
    CHECK_TRUE(rm.GetMaterial((fs::current_path(ec) / "assets/wall.sagemat").string()) == byRef);
    // Обратный слэш здесь НЕ проверяется намеренно: на POSIX это законный
    // символ имени, то есть «assets\\wall.sagemat» — другой файл, а не другое
    // написание того же. На Windows разделители сводит сама файловая система.

    // И правка через одно написание видна через другое — это и есть то, чего
    // ждут от редактора материалов.
    // Значение НАРОЧНО не совпадает ни с одним значением по умолчанию: иначе
    // «тот же объект» и «новый объект с дефолтом» дали бы одинаковый ответ.
    byRef->Albedo = glm::vec3(0.25f, 0.5f, 0.75f);
    std::shared_ptr<Material> again = rm.GetMaterial("./assets/wall.sagemat");
    CHECK_TRUE(again != nullptr);
    if (again) {
        CHECK_NEAR(again->Albedo.x, 0.25f, 1e-4);
        CHECK_NEAR(again->Albedo.y, 0.50f, 1e-4);
        CHECK_NEAR(again->Albedo.z, 0.75f, 1e-4);
    }

    rm.Clear();
    fs::current_path(saved, ec);
}
