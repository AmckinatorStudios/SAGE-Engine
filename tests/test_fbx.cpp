// Импорт FBX. До этого движок открывал .obj/.gltf/.glb/.blend/.bbmodel — то
// есть всё, кроме формата, в который по умолчанию экспортируют Blender, Maya,
// 3ds Max, Mixamo и любой ассет-стор. Человек со скачанной моделью упирался в
// «формат не поддерживается» и делал вывод, что своя модель движку не нужна.
//
// Файлы для проверки собирает tools/make_test_fbx.py: чужой .fbx в репозиторий
// не положить (вес и лицензия), а скачивать в тестах нечего — CI без сети.
// Генератор пишет ровно то, что разбирает импортёр, и значения в нём известны
// заранее — поэтому здесь сверяются КООРДИНАТЫ, а не факт «загрузилось».
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"

namespace fs = std::filesystem;

namespace {

// Зовёт генератор. Возвращает пустую строку, если питона нет, — тогда тест
// честно пропускается, а не падает по чужой причине.
std::string MakeFbx(const char* name, const char* extraArgs) {
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path();
    const fs::path script = repo / "tools" / "make_test_fbx.py";
    const fs::path out = fs::temp_directory_path() / name;
    std::string cmd = "python3 \"" + script.string() + "\" \"" + out.string() + "\" " + extraArgs +
                      " > /dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return {};
    std::error_code ec;
    if (!fs::exists(out, ec)) return {};
    return out.string();
}

sage::assets::ImportedScene Import(const std::string& path, bool& ok, std::string& err) {
    sage::assets::ImportedScene scene;
    ok = sage::assets::ImporterRegistry::Instance().Import(path, scene, err);
    return scene;
}

} // namespace

TEST(Fbx_is_registered_as_an_importable_format) {
    // Само по себе важно: панель Assets, диалог «Обзор…» и подсказка
    // «поддерживаются…» спрашивают реестр, а не свой список.
    CHECK_TRUE(sage::assets::ImporterRegistry::Instance().CanImport(".fbx"));
}

TEST(Fbx_binary_cube_loads_with_geometry_normals_and_uvs) {
    const std::string path = MakeFbx("sage_test_cube.fbx", "");
    if (path.empty()) return; // нет python3 — проверять нечем
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());

    CHECK_TRUE(ok);
    if (!ok) {
        std::printf("       ошибка импорта: %s\n", err.c_str());
        return;
    }
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (scene.Nodes.empty()) return;

    const sage::render::MeshData& mesh = scene.Nodes[0].Mesh;
    // Шесть четырёхугольников -> двенадцать треугольников -> 36 вершин.
    CHECK_EQ((int)mesh.Indices.size(), 36);
    CHECK_EQ((int)mesh.Vertices.size(), 36);

    // Имя берётся у узла Model, а не у файла: в сцене из нескольких мешей иначе
    // все объекты назывались бы одинаково.
    CHECK_TRUE(scene.Nodes[0].Name == "TestCube");

    // Куб со стороной 1 при UnitScaleFactor 100 (единица = метр) обязан
    // остаться стороной 1: половина ребра — ровно 0.5.
    float maxAbs = 0.0f;
    for (const Vertex& v : mesh.Vertices) {
        maxAbs = std::max(maxAbs, std::max(std::abs(v.Position.x),
                                           std::max(std::abs(v.Position.y), std::abs(v.Position.z))));
    }
    CHECK_NEAR(maxAbs, 0.5f, 1e-4);

    // Нормали пришли из файла и нормированы (а не остались нулями).
    bool normalsOk = true;
    for (const Vertex& v : mesh.Vertices) {
        if (std::abs(glm::length(v.Normal) - 1.0f) > 1e-3f) normalsOk = false;
    }
    CHECK_TRUE(normalsOk);

    // UV не все нулевые — иначе текстура легла бы одной точкой.
    bool anyUv = false;
    for (const Vertex& v : mesh.Vertices) {
        if (v.TexCoords.x > 0.5f || v.TexCoords.y > 0.5f) anyUv = true;
    }
    CHECK_TRUE(anyUv);
}

TEST(Fbx_centimetres_are_converted_to_metres) {
    // UnitScaleFactor 1 — файл в САНТИМЕТРАХ (умолчание FBX). Куб со стороной 1
    // единица обязан приехать сантиметровым, а не метровым: иначе персонаж
    // ростом 180 см встаёт в сцену высотой сто восемьдесят метров, и это
    // списывают на «модель кривая».
    const std::string path = MakeFbx("sage_test_cube_cm.fbx", "--unit 1");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());

    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty()) return;

    float maxAbs = 0.0f;
    for (const Vertex& v : scene.Nodes[0].Mesh.Vertices) {
        maxAbs = std::max(maxAbs, std::abs(v.Position.x));
    }
    CHECK_NEAR(maxAbs, 0.005f, 1e-5); // 0.5 см в метрах
}

TEST(Fbx_z_up_files_are_turned_into_engine_orientation) {
    // 3ds Max и часть экспортов Blender пишут Z вверх. Без разворота модель
    // лежит на боку — и это чинят вручную поворотом на 90°, который потом
    // ломает физику и анимацию.
    const std::string flat = MakeFbx("sage_test_cube_y.fbx", "");
    const std::string zup = MakeFbx("sage_test_cube_z.fbx", "--zup");
    if (flat.empty() || zup.empty()) return;

    bool okA = false, okB = false;
    std::string errA, errB;
    sage::assets::ImportedScene a = Import(flat, okA, errA);
    sage::assets::ImportedScene b = Import(zup, okB, errB);
    std::remove(flat.c_str());
    std::remove(zup.c_str());

    CHECK_TRUE(okA && okB);
    if (!okA || !okB || a.Nodes.empty() || b.Nodes.empty()) return;

    // Тот же куб, записанный двумя способами, обязан приехать одинаковым.
    CHECK_EQ((int)a.Nodes[0].Mesh.Vertices.size(), (int)b.Nodes[0].Mesh.Vertices.size());
    float diff = 0.0f;
    for (size_t i = 0; i < a.Nodes[0].Mesh.Vertices.size(); ++i) {
        diff = std::max(diff, glm::length(a.Nodes[0].Mesh.Vertices[i].Position -
                                          b.Nodes[0].Mesh.Vertices[i].Position));
    }
    CHECK_NEAR(diff, 0.0f, 1e-4);
}

TEST(Fbx_ascii_file_says_what_to_do_instead_of_just_failing) {
    // Текстовый FBX (до 2013) здесь не разбирается — и ответ «формат не
    // поддерживается» был бы издевательством: человек видит .fbx и в списке
    // поддерживаемых .fbx тоже видит. Сообщение обязано называть ПРИЧИНУ и
    // ДЕЙСТВИЕ.
    const std::string path = MakeFbx("sage_test_old.fbx", "--ascii");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    Import(path, ok, err);
    std::remove(path.c_str());

    CHECK_FALSE(ok);
    CHECK_TRUE(err.find("ТЕКСТОВЫЙ") != std::string::npos);
    CHECK_TRUE(err.find("glb") != std::string::npos || err.find("ASCII") != std::string::npos);
}

TEST(Fbx_goes_through_the_same_door_as_every_other_model) {
    // Реестр — это половина пути. Редактор и игра зовут не его, а
    // ModelLoader::LoadMeshData (и GetModel поверх него): если формат
    // зарегистрирован, но загрузчик про него не знает, «поддержка» есть только
    // в списке поддерживаемых.
    const std::string path = MakeFbx("sage_test_door.fbx", "");
    if (path.empty()) return;

    bool loaded = false;
    size_t triangles = 0;
    try {
        const sage::render::MeshData data = ModelLoader::LoadMeshData(path);
        loaded = !data.Empty();
        triangles = data.Indices.size() / 3;
    } catch (const std::exception& e) {
        std::printf("       LoadMeshData(.fbx): %s\n", e.what());
    }
    std::remove(path.c_str());

    CHECK_TRUE(loaded);
    CHECK_EQ((int)triangles, 12); // куб
    CHECK_TRUE(ModelLoader::IsSupportedModel("собственная модель.FBX")); // и регистр букв не важен
}
