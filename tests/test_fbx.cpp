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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// Blender пишет Transform кластера ОТНОСИТЕЛЬНО КОСТИ (inverse(TransformLink) *
// мир меша), а не мир меша, как Autodesk. Прочитанный по Autodesk, такой файл
// умножался на обратную кость дважды: персонаж в позе покоя схлопывался в
// комок с «лучами» (Universal Animation Library, FBX для Unity). И клип там
// назван «Armature|Wave» — для человека и для Play("Wave") это «Wave».
TEST(Fbx_blender_cluster_transform_keeps_the_rest_pose_in_place) {
    const std::string path = MakeFbx("sage_test_skin_blender.fbx", "--skin --blender-bind");
    if (path.empty()) return;
    sage::render::ModelData data;
    std::string err;
    const bool ok = sage::assets::ImportFbxSkinned(path, data, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || data.Skeleton.Count() != 2 || data.SubMeshes.empty()) return;

    // Поза покоя: палитра не двигает ни одной вершины.
    sage::anim::Animator animator;
    animator.SetRig(&data.Skeleton, &data.Clips);
    animator.Update(0.0f);
    const std::vector<glm::mat4>& palette = animator.BoneMatrices();
    float worst = 0.0f;
    for (const sage::render::SkinnedVertex& v : data.SubMeshes[0].Vertices) {
        const glm::vec3 moved = glm::vec3(palette[(size_t)v.Joints[0]] * glm::vec4(v.Position, 1.0f));
        worst = std::max(worst, glm::length(moved - v.Position));
    }
    std::printf("       поза покоя сдвинула вершины на %.4f\n", worst);
    CHECK_TRUE(worst < 1e-4f);

    CHECK_EQ((int)data.Clips.size(), 1);
    if (!data.Clips.empty()) CHECK_EQ(data.Clips[0].Name, std::string("Wave"));
}

// Клип по умолчанию — самая простая поза покоя: библиотека анимаций несёт
// десяток «idle», и персонаж на корточках (Crouch_Idle_Loop) выглядит сломанным.
TEST(Preferred_idle_clip_is_the_plainest_one) {
    std::vector<sage::anim::AnimationClip> clips(5);
    clips[0].Name = "A_TPose";
    clips[1].Name = "Crouch_Idle_Loop";
    clips[2].Name = "Idle_Talking_Loop";
    clips[3].Name = "Armature|Idle_Loop";
    clips[4].Name = "Pistol_Idle_Loop";
    CHECK_EQ(sage::anim::PreferredIdleClip(clips), 3);

    std::vector<sage::anim::AnimationClip> none(2);
    none[0].Name = "Run";
    none[1].Name = "Jump";
    CHECK_EQ(sage::anim::PreferredIdleClip(none), 0);   // idle нет — первый
    CHECK_EQ(sage::anim::PreferredIdleClip({}), -1);
}

// ============================================================================
//  Жёсткие детали на костях и материалы скиновой модели
//
//  Обе беды видны на одной жалобе: «модель грузится без текстур и выглядит не
//  как в Blender». Причин было две, и обе — в скиновом пути FBX.
//
//  1. Отбор геометрии шёл по одному признаку — есть ли у неё скин, — и всё
//     остальное молча выбрасывалось. Но деталь, которая не гнётся (панель,
//     зуб, глазница, пряжка, инструмент), риггят НЕ весами, а привязкой узла к
//     кости: это дешевле и точнее. У разбираемой модели (Spring Bonnie из
//     Blender) со скином 6 геометрий из 98 — в сцену попадали шесть кусков.
//  2. Материалы не читались ВООБЩЕ. Цвета в FBX есть всегда (Properties70
//     материала), статический путь их брал — и одна и та же модель была
//     цветной в панели ассетов и белой в сцене.
// ============================================================================
TEST(Fbx_skin_keeps_rigid_parts_on_bones_and_their_materials) {
    const std::string path = MakeFbx("sage_test_rigid.fbx", "--skin --rigid --unit 1");
    if (path.empty()) return; // нет python3 — проверять нечем

    sage::render::ModelData data;
    std::string err;
    const bool ok = sage::assets::ImportFbxSkinned(path, data, err);
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    if (!ok) { std::remove(path.c_str()); return; }

    // ДВЕ части, а не одна: скиновая полоса И жёсткая панель.
    CHECK_EQ((int)data.SubMeshes.size(), 2);

    // Цвет материала доехал до ОБЕИХ частей.
    int coloured = 0;
    for (const sage::render::ModelSubMeshData& sub : data.SubMeshes) {
        if (std::fabs(sub.Material.Tint.x - 0.36f) < 0.01f &&
            std::fabs(sub.Material.Tint.y - 0.21f) < 0.01f &&
            std::fabs(sub.Material.Tint.z - 0.03f) < 0.01f) {
            ++coloured;
        }
    }
    std::printf("       частей с цветом материала: %d из %d\n", coloured,
                (int)data.SubMeshes.size());
    CHECK_EQ(coloured, (int)data.SubMeshes.size());

    // И ГЛАВНОЕ: жёсткая деталь стоит ТАМ ЖЕ, где её ставит статический разбор
    // того же файла. Проверка на «часть появилась» пропустила бы самую частую
    // поломку такого переноса — деталь есть, но улетела в начало координат или
    // развёрнута костью наизнанку.
    //
    // Вершины жёсткой части лежат в системе координат КОСТИ: чтобы получить
    // место в модели, их надо прогнать через матрицу кости в позе привязки —
    // ровно то, что сделает палитра в кадре.
    sage::assets::ImportedScene flat;
    std::string ferr;
    const bool fok = sage::assets::ImporterRegistry::Instance().Import(path, flat, ferr);
    std::remove(path.c_str());
    CHECK_TRUE(fok);
    if (!fok) return;

    // Панель в статическом разборе — часть с вершинами правее полосы (x >= 2
    // единицы файла, то есть 0.02 м).
    float staticRight = -1e9f, staticTop = -1e9f;
    for (const sage::assets::ImportedNode& n : flat.Nodes) {
        for (const Vertex& v : n.Mesh.Vertices) {
            staticRight = std::max(staticRight, v.Position.x);
            staticTop = std::max(staticTop, v.Position.y);
        }
    }

    // Та же крайняя точка, посчитанная через кости.
    std::vector<glm::mat4> bind(data.Skeleton.Joints.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < data.Skeleton.Joints.size(); ++i) {
        glm::mat4 g = data.Skeleton.Joints[i].LocalMatrix();
        for (int p = data.Skeleton.Joints[i].Parent; p >= 0;
             p = data.Skeleton.Joints[(size_t)p].Parent)
            g = data.Skeleton.Joints[(size_t)p].LocalMatrix() * g;
        bind[i] = g * data.Skeleton.Joints[i].InverseBind;
    }
    float skinnedRight = -1e9f;
    for (const sage::render::ModelSubMeshData& sub : data.SubMeshes) {
        for (const sage::render::SkinnedVertex& v : sub.Vertices) {
            const int joint = (int)v.Joints[0];
            const glm::mat4 m = (v.Weights[0] > 0.5f && joint >= 0 && joint < (int)bind.size())
                                    ? bind[(size_t)joint] : glm::mat4(1.0f);
            skinnedRight = std::max(skinnedRight, (m * glm::vec4(v.Position, 1.0f)).x);
        }
    }
    std::printf("       правый край: статикой %.4f, через кости %.4f\n", (double)staticRight,
                (double)skinnedRight);
    CHECK_NEAR(skinnedRight, staticRight, 1e-3f);
    // И деталь не схлопнулась в точку: панель шире полосы.
    CHECK_TRUE(staticRight > 0.02f - 1e-4f);
    (void)staticTop;
}

// --- Дерево из набора: один меш, два материала, вырез по альфе --------------
//
// Так экспортирует Blender ЛЮБОЕ дерево (проверено на Stylized Nature MegaKit):
// кора и листва — одна геометрия, материал задан на каждой грани
// (LayerElementMaterial). Импортёр брал один «первый попавшийся» материал на
// всю геометрию — листва рисовалась корой, сплошными квадратами, и так же
// квадратами отбрасывала тень. Карта прозрачности (TransparencyFactor) не
// читалась вовсе.
namespace {

float TriangleArea(const Vertex& a, const Vertex& b, const Vertex& c, glm::vec3* normal = nullptr) {
    const glm::vec3 n = glm::cross(b.Position - a.Position, c.Position - a.Position);
    if (normal) *normal = n;
    return glm::length(n) * 0.5f;
}

} // namespace

TEST(Fbx_one_mesh_painted_by_faces_splits_into_parts_by_material) {
    const std::string path = MakeFbx("sage_test_foliage.fbx", "--foliage");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok) return;

    CHECK_EQ((int)scene.Nodes.size(), 2);
    CHECK_EQ((int)scene.Materials.size(), 2);
    if (scene.Nodes.size() != 2 || scene.Materials.size() != 2) return;
    const sage::assets::ImportedNode& bark = scene.Nodes[0];
    const sage::assets::ImportedNode& leaf = scene.Nodes[1];
    CHECK_TRUE(bark.MaterialIndex != leaf.MaterialIndex);
    if (bark.MaterialIndex < 0 || leaf.MaterialIndex < 0) return;
    CHECK_TRUE(scene.Materials[(size_t)bark.MaterialIndex].Name == "Bark");
    CHECK_TRUE(scene.Materials[(size_t)leaf.MaterialIndex].Name == "Leaves");
    // Лист — выше трёх метров, кора — ниже двух: части не перепутаны.
    for (const Vertex& v : leaf.Mesh.Vertices) CHECK_TRUE(v.Position.y > 2.9f);
    for (const Vertex& v : bark.Mesh.Vertices) CHECK_TRUE(v.Position.y < 2.1f);
}

TEST(Fbx_transparency_map_makes_an_alpha_cutout_double_sided_material) {
    const std::string path = MakeFbx("sage_test_foliage_alpha.fbx", "--foliage");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok) return;
    for (const sage::assets::ImportedMaterial& m : scene.Materials) {
        if (m.Name == "Leaves") {
            CHECK_EQ(m.AlphaMode, 1);
            CHECK_TRUE(m.DoubleSided);
            CHECK_TRUE(m.AlbedoTexture == "leaf.png");
        } else {
            // Кора без карты прозрачности — сплошная, как и была.
            CHECK_EQ(m.AlphaMode, 0);
            CHECK_TRUE(!m.DoubleSided);
        }
    }
}

TEST(Fbx_concave_face_is_cut_inside_its_outline_not_by_a_fan) {
    // Кора в тестовом файле — вогнутый «дротик» площадью 1.25 м². Веер от
    // первой вершины режет его по диагонали СНАРУЖИ: треугольник вылезает за
    // контур, второй выворачивается изнанкой, площадь выходит 2.75.
    const std::string path = MakeFbx("sage_test_concave.fbx", "--foliage");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty()) return;

    const sage::render::MeshData& mesh = scene.Nodes[0].Mesh;
    CHECK_EQ((int)mesh.Indices.size(), 6);
    float area = 0.0f;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        glm::vec3 n;
        area += TriangleArea(mesh.Vertices[mesh.Indices[i]], mesh.Vertices[mesh.Indices[i + 1]],
                             mesh.Vertices[mesh.Indices[i + 2]], &n);
        CHECK_TRUE(n.z > 0.0f);   // ни один треугольник не вывернут изнанкой
    }
    CHECK_NEAR(area, 1.25f, 1e-3f);
}

TEST(Fbx_pre_rotation_of_a_node_turns_its_mesh) {
    // Maya и 3ds Max кладут разворот узла в PreRotation. Статический импорт
    // читал только Lcl Translation/Rotation/Scaling, и такие детали стояли
    // повёрнутыми не туда — «часть модели искажена».
    const std::string path = MakeFbx("sage_test_prerot.fbx", "--pre-rotation 0 0 90");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty() || scene.Nodes[0].Mesh.Vertices.empty()) return;
    // Первая вершина куба (-0.5,-0.5,-0.5) после поворота на 90° вокруг Z.
    const glm::vec3 p = scene.Nodes[0].Mesh.Vertices[0].Position;
    CHECK_NEAR(p.x, 0.5f, 1e-4f);
    CHECK_NEAR(p.y, -0.5f, 1e-4f);
    CHECK_NEAR(p.z, -0.5f, 1e-4f);
}

TEST(Fbx_mirrored_node_keeps_its_faces_facing_outward) {
    // Масштаб -1 — обычный приём для симметричных деталей. Отражение меняет
    // обход треугольников, и без разворота деталь оказывалась вывернутой
    // наизнанку: отсечение задних граней съедало её целиком.
    const std::string path = MakeFbx("sage_test_mirror.fbx", "--node-scale -1");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty()) return;
    const sage::render::MeshData& mesh = scene.Nodes[0].Mesh;
    int agree = 0, triangles = 0;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        const Vertex& a = mesh.Vertices[mesh.Indices[i]];
        glm::vec3 n;
        TriangleArea(a, mesh.Vertices[mesh.Indices[i + 1]], mesh.Vertices[mesh.Indices[i + 2]], &n);
        // Лицевая сторона по обходу обязана совпадать с нормалью из файла
        // (отражённой вместе с узлом): иначе свет падает на одну сторону, а
        // видна другая.
        if (glm::dot(n, a.Normal) > 0.0f) ++agree;
        ++triangles;
    }
    CHECK_EQ(triangles, 12);
    CHECK_EQ(agree, triangles);
}

TEST(Fbx_normals_given_per_face_light_each_face_by_its_own_normal) {
    // ByPolygon — одна нормаль на грань. Раньше она читалась как «по углу»:
    // угол k брал нормаль грани k, и уже со второй грани свет ложился чужой
    // нормалью (а после шестого угла — нулевой).
    const std::string path = MakeFbx("sage_test_facenormals.fbx", "--normals-by-polygon");
    if (path.empty()) return;
    bool ok = false;
    std::string err;
    sage::assets::ImportedScene scene = Import(path, ok, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || scene.Nodes.empty()) return;
    const sage::render::MeshData& mesh = scene.Nodes[0].Mesh;
    int agree = 0, corners = 0;
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        glm::vec3 n;
        TriangleArea(mesh.Vertices[mesh.Indices[i]], mesh.Vertices[mesh.Indices[i + 1]],
                     mesh.Vertices[mesh.Indices[i + 2]], &n);
        n = glm::normalize(n);
        for (int c = 0; c < 3; ++c) {
            ++corners;
            if (glm::dot(mesh.Vertices[mesh.Indices[i + (size_t)c]].Normal, n) > 0.99f) ++agree;
        }
    }
    CHECK_EQ(corners, 36);
    CHECK_EQ(agree, corners);
}

TEST(Fbx_skinned_uv_follows_the_image_convention_of_the_skinned_pass) {
    // Развёртка FBX — с началом внизу, а картинки скелетной модели лежат
    // строками сверху вниз (соглашение glTF, общее для скелетного прохода).
    // Взятая как есть, развёртка читала текстуру вверх ногами.
    const std::string path = MakeFbx("sage_test_skin_uv.fbx", "--skin");
    if (path.empty()) return;
    sage::render::ModelData data;
    std::string err;
    const bool ok = sage::assets::ImportFbxSkinned(path, data, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok || data.SubMeshes.empty()) return;
    // Нижняя левая вершина полосы (y = 0, x < 0) в FBX имеет uv (0, 0) —
    // низ картинки, то есть v = 1 в соглашении «строки сверху вниз».
    bool found = false;
    for (const sage::render::SkinnedVertex& v : data.SubMeshes[0].Vertices) {
        if (v.Position.y < 0.01f && v.Position.x < 0.0f) {
            found = true;
            CHECK_NEAR(v.TexCoords.x, 0.0f, 1e-5f);
            CHECK_NEAR(v.TexCoords.y, 1.0f, 1e-5f);
        }
    }
    CHECK_TRUE(found);
}

// --- Есть ли скелет: по оглавлению, без разбора модели ----------------------
//
// Редактор спрашивает скелетную версию у КАЖДОЙ поставленной модели. Без пробы
// .obj уходил в разбор glTF и сыпал в консоль «Скиннинг-модель не загрузилась
// … parse error» — ошибкой на обычной декорации (набор Stylized Nature MegaKit:
// каждый куст, каждое дерево).
#include "sage/assets/import/ModelProbe.h"

TEST(Model_probe_tells_skinned_files_from_static_ones) {
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path();
    CHECK_TRUE(sage::assets::ModelHasSkeleton((repo / "engine/assets/test_rig.glb").string()));
    // Статический .gltf — дерево из набора устроено так же: сетка и материалы,
    // без skins.
    const fs::path gltf = fs::temp_directory_path() / "sage_test_probe_static.gltf";
    {
        std::ofstream f(gltf);
        f << R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
             R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})";
    }
    CHECK_TRUE(!sage::assets::ModelHasSkeleton(gltf.string()));
    std::remove(gltf.string().c_str());

    const std::string skin = MakeFbx("sage_test_probe_skin.fbx", "--skin");
    const std::string cube = MakeFbx("sage_test_probe_cube.fbx", "");
    if (!skin.empty()) CHECK_TRUE(sage::assets::ModelHasSkeleton(skin));
    if (!cube.empty()) CHECK_TRUE(!sage::assets::ModelHasSkeleton(cube));
    std::remove(skin.c_str());
    std::remove(cube.c_str());

    // .obj скелета не несёт вовсе — и в разбор glTF его отправлять нельзя.
    const fs::path obj = fs::temp_directory_path() / "sage_test_probe.obj";
    { std::ofstream f(obj); f << "# Blender\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
    CHECK_TRUE(!sage::assets::ModelHasSkeleton(obj.string()));
    std::remove(obj.string().c_str());
}

// Настройки импорта (.sageimport) не доходили до FBX вовсе: их применяли
// только загрузчики .obj и glTF, и масштаб, выставленный у FBX-модели, ни на
// что не влиял.
TEST(Fbx_import_settings_reach_the_mesh) {
    const std::string path = MakeFbx("sage_test_fbx_settings.fbx", "");
    if (path.empty()) return;
    ModelLoader::ImportSettings s;
    s.Scale = 3.0f;
    CHECK_TRUE(ModelLoader::SaveImportSettings(path, s));
    const sage::render::MeshData mesh = ModelLoader::LoadMeshData(path);
    std::error_code ec;
    fs::remove(ModelLoader::ImportSidecarPath(path), ec);
    std::remove(path.c_str());
    glm::vec3 mn(1e9f), mx(-1e9f);
    for (const Vertex& v : mesh.Vertices) { mn = glm::min(mn, v.Position); mx = glm::max(mx, v.Position); }
    CHECK_NEAR(mx.x - mn.x, 3.0f, 1e-3f);   // куб со стороной 1, масштаб 3
}

// Выбор в окне импорта главнее того, что описал файл: «вырез» и «обе
// стороны» доходят до материала, даже если экспорт их не записал.
#include "sage/render/ModelMaterial.h"

TEST(Fbx_import_settings_override_material_transparency) {
    const std::string path = MakeFbx("sage_test_fbx_matsettings.fbx", "--foliage");
    if (path.empty()) return;
    ModelLoader::ImportSettings s;
    s.Alpha = ModelLoader::ImportSettings::AlphaMode::Cutout;
    s.AlphaCutoff = 0.3f;
    s.DoubleSided = ModelLoader::ImportSettings::TwoSided::On;
    CHECK_TRUE(ModelLoader::SaveImportSettings(path, s));
    const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials(path);
    std::error_code ec;
    fs::remove(ModelLoader::ImportSidecarPath(path), ec);
    std::remove(path.c_str());
    CHECK_EQ((int)set.Materials.size(), 2);
    for (const ModelLoader::ExtractedMaterial& m : set.Materials) {
        CHECK_EQ(m.AlphaMode, 1);
        CHECK_NEAR(m.AlphaCutoff, 0.3f, 1e-4f);
        CHECK_TRUE(m.DoubleSided);
    }
}
