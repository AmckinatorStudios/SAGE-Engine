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

#include <cmath>
#include <memory>

#include "sage/anim/Animator.h"
#include "sage/assets/import/Importer.h"
#include "sage/assets/import/FbxSkin.h"
#include "sage/render/ModelData.h"
#include "sage/render/SkinnedModel.h"
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

TEST(Fbx_node_transform_places_and_scales_the_mesh) {
    // Импортёр читал только Geometry и ИГНОРИРОВАЛ узлы Model, то есть их
    // положение, поворот и масштаб. Для файла с одним мешем это сходило с рук;
    // настоящая модель состоит из нескольких частей — и все они сваливались в
    // начало координат без своего масштаба. Проверено на живом ассете: рюкзак
    // из 53 деталей приезжал кучей размером 3.5 см вместо собранной сумки, и
    // тот же рюкзак в .glb был в сто раз крупнее — два формата одной модели
    // расходились в сто раз.
    const std::string path = MakeFbx("sage_test_node_xform.fbx", "--offset 5 0 0 --node-scale 2");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());

    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty()) return;

    glm::vec3 mn(1e9f), mx(-1e9f);
    for (const Vertex& v : scene.Nodes[0].Mesh.Vertices) {
        mn = glm::min(mn, v.Position);
        mx = glm::max(mx, v.Position);
    }
    // Масштаб узла 2 -> куб со стороной 1 приезжает стороной 2.
    CHECK_NEAR(mx.x - mn.x, 2.0f, 1e-3f);
    // Смещение узла 5 по X -> центр куба там же (единицы = метры при
    // UnitScaleFactor 100).
    CHECK_NEAR((mn.x + mx.x) * 0.5f, 5.0f, 1e-3f);
    CHECK_NEAR((mn.y + mx.y) * 0.5f, 0.0f, 1e-3f);
}

// --- СКИН: кости, веса и клип ----------------------------------------------
//
// FBX — то, во что экспортируют по умолчанию Blender, Maya, Mixamo и любой
// ассет-стор, и персонажи оттуда приходят СО СКИНОМ. Пока скин не читался,
// движок отвечал «не загрузить glTF … parse error» — сообщением о чужом
// формате, из которого следовал вывод «моя модель движку не подходит».
//
// Эталон устроен так, чтобы измерять, а не «проверять факт загрузки»: полоса
// из четырёх вершин на двух костях, нижние держит первая кость, верхние —
// вторая. Значит, поворот второй кости обязан двигать РОВНО верхние вершины.
TEST(Fbx_with_skin_loads_bones_weights_and_clips) {
    // --unit 1 — файл в САНТИМЕТРАХ, как их и отдают экспортёры: заодно
    // проверяется, что кости и вершины приводятся к метрам одинаково.
    const std::string path = MakeFbx("sage_test_skin.fbx", "--skin --unit 1");
    if (path.empty()) return; // нет python3 — проверять нечем

    // Проверяется РАЗБОР, а не загрузка на видеокарту: контекста в модульных
    // тестах нет, а спрашиваем мы про данные — кости, веса, клипы.
    sage::render::ModelData data;
    std::string err;
    const bool ok = sage::assets::ImportFbxSkinned(path, data, err);
    std::remove(path.c_str());
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    if (!ok) return;

    // Скелет: две кости, вторая — ребёнок первой, имена из файла.
    CHECK_EQ(data.Skeleton.Count(), 2);
    if (data.Skeleton.Count() != 2) return;
    CHECK_EQ(data.Skeleton.Joints[0].Name, std::string("Root"));
    CHECK_EQ(data.Skeleton.Joints[1].Name, std::string("Upper"));
    CHECK_EQ(data.Skeleton.Joints[1].Parent, 0);
    // Единицы: файл в сантиметрах, кость на высоте 1 единицы -> 0.01 метра.
    CHECK_NEAR(data.Skeleton.Joints[1].Translation.y, 0.01f, 1e-4f);

    // ВЕСА ДОЕХАЛИ ДО ВЕРШИН, и именно те: нижние вершины держит первая кость,
    // верхние — вторая. Проверка на «веса вообще есть» пропустила бы самую
    // частую поломку — перепутанные местами кости.
    CHECK_EQ((int)data.SubMeshes.size(), 1);
    if (data.SubMeshes.empty()) return;
    const std::vector<sage::render::SkinnedVertex>& verts = data.SubMeshes[0].Vertices;
    CHECK_TRUE(verts.size() >= 6);   // четырёхугольник -> два треугольника
    int lowOnRoot = 0, highOnUpper = 0;
    for (const sage::render::SkinnedVertex& v : verts) {
        const int joint = (int)v.Joints[0];
        const bool low = v.Position.y < 0.005f;
        if (low && joint == 0 && v.Weights[0] > 0.99f) ++lowOnRoot;
        if (!low && joint == 1 && v.Weights[0] > 0.99f) ++highOnUpper;
    }
    std::printf("       вершин: низ на Root %d, верх на Upper %d\n", lowOnRoot, highOnUpper);
    CHECK_TRUE(lowOnRoot >= 2);
    CHECK_TRUE(highOnUpper >= 2);

    // Клип назван по стеку анимации, а не номером: номер молча меняется при
    // переэкспорте (см. anim/AnimationComponents.h).
    CHECK_EQ((int)data.Clips.size(), 1);
    if (data.Clips.empty()) return;
    CHECK_EQ(data.Clips[0].Name, std::string("Wave"));
    CHECK_NEAR(data.Clips[0].Duration, 1.0f, 1e-3f);

    // И ГЛАВНОЕ: клип РЕАЛЬНО ДВИГАЕТ СКЕЛЕТ. Гоним его до конца (поворот
    // второй кости на 90° вокруг Z) и смотрим палитру: первая кость обязана
    // остаться на месте, вторая — повернуться. Без матриц привязки и каналов
    // эта проверка не проходит ни при каких обстоятельствах.
    sage::anim::Animator animator;
    animator.SetRig(&data.Skeleton, &data.Clips);
    animator.Play(0, false);
    animator.Update(1.0f);
    const std::vector<glm::mat4>& palette = animator.BoneMatrices();
    CHECK_TRUE(palette.size() >= 2);
    if (palette.size() < 2) return;

    // Точка на сантиметр выше начала верхней кости: поворот на 90° вокруг Z
    // кладёт её набок — x уходит от нуля, y падает.
    const glm::vec3 tip = glm::vec3(palette[1] * glm::vec4(0.0f, 0.02f, 0.0f, 1.0f));
    std::printf("       верх после поворота: (%.4f, %.4f, %.4f)\n", tip.x, tip.y, tip.z);
    CHECK_TRUE(std::abs(tip.x) > 0.005f);
    CHECK_TRUE(tip.y < 0.019f);

    const glm::vec3 base = glm::vec3(palette[0] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK_NEAR(base.x, 0.0f, 1e-4f);
    CHECK_NEAR(base.y, 0.0f, 1e-4f);
}
