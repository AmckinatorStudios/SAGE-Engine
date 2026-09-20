// Реестр импортёров и разбор чужих форматов.
//
// Blockbench проверяется по геометрии, а не по «функция вернула true»:
// координаты углов, число граней, влияние поворота и то, что грань без текстуры
// не рисуется. Всё это можно посчитать на бумаге, поэтому и проверяется точно.
#include "TestFramework.h"

#include <glm/gtc/quaternion.hpp>

#include "sage/assets/import/Importer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using namespace sage::assets;

namespace {

std::string WriteTemp(const std::string& name, const std::string& contents) {
    std::ofstream f(name, std::ios::binary);
    f << contents;
    f.close();
    return name;
}

// Один куб 16x16x16 в начале координат — то есть ровно один блок.
const char* kOneCube = R"({
  "meta": {"format_version": "4.5", "model_format": "free", "box_uv": false},
  "resolution": {"width": 16, "height": 16},
  "elements": [
    {"name": "block", "type": "cube", "from": [0,0,0], "to": [16,16,16],
     "faces": {
       "north": {"uv": [0,0,16,16], "texture": 0},
       "south": {"uv": [0,0,16,16], "texture": 0},
       "west":  {"uv": [0,0,16,16], "texture": 0},
       "east":  {"uv": [0,0,16,16], "texture": 0},
       "up":    {"uv": [0,0,16,16], "texture": 0},
       "down":  {"uv": [0,0,16,16], "texture": 0}
     }}
  ],
  "textures": [{"name": "stone", "relative_path": "../textures/stone.png"}]
})";

} // namespace

TEST(importer_registry_knows_its_formats) {
    ImporterRegistry& reg = ImporterRegistry::Instance();
    CHECK_TRUE(reg.CanImport(".obj"));
    CHECK_TRUE(reg.CanImport(".gltf"));
    CHECK_TRUE(reg.CanImport(".glb"));
    CHECK_TRUE(reg.CanImport(".bbmodel"));
    CHECK_TRUE(reg.CanImport(".blend"));
    CHECK_TRUE(reg.CanImport(".sagemesh"));
    // Регистр букв не должен иметь значения: файлы приходят от людей.
    CHECK_TRUE(reg.CanImport(".BBMODEL"));
    CHECK_TRUE(reg.CanImport("obj"));   // и точка необязательна
    CHECK_TRUE(reg.CanImport(".fbx"));   // самый частый формат обмена
    CHECK_FALSE(reg.CanImport(".3ds"));  // а этого движок не умеет

    // Отказ обязан перечислить, что движок умеет: человеку с чужим форматом
    // нужно знать, во что пересохранить, а не только что его не приняли.
    ImportedScene scene;
    std::string err;
    CHECK_FALSE(reg.Import("nothing.3ds", scene, err));
    CHECK_TRUE(err.find(".bbmodel") != std::string::npos);
}

TEST(importer_registry_is_open_for_new_formats) {
    ImporterRegistry& reg = ImporterRegistry::Instance();
    CHECK_FALSE(reg.CanImport(".mine"));
    reg.Register(".mine", "Тестовый формат", [](const std::string&, ImportedScene& out,
                                                std::string&) {
        ImportedNode n;
        n.Name = "from-plugin";
        n.Mesh = sage::render::BuildCube();
        out.Nodes.push_back(std::move(n));
        return true;
    });
    CHECK_TRUE(reg.CanImport(".mine"));

    const std::string path = WriteTemp("registry_probe.mine", "не важно");
    ImportedScene scene;
    std::string err;
    CHECK_TRUE(reg.Import(path, scene, err));
    CHECK_EQ((int)scene.Nodes.size(), 1);
    CHECK_TRUE(!scene.Nodes.empty() && scene.Nodes[0].Name == "from-plugin");
    std::remove(path.c_str());
}

TEST(blockbench_cube_geometry_is_exact) {
    const std::string path = WriteTemp("bb_one_cube.bbmodel", kOneCube);
    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportBlockbench(path, scene, err));
    if (!err.empty()) std::printf("    err: %s\n", err.c_str());
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (scene.Nodes.empty()) return;

    const sage::render::MeshData& m = scene.Nodes[0].Mesh;
    // Шесть граней по четыре вершины и по два треугольника.
    CHECK_EQ((int)m.Vertices.size(), 24);
    CHECK_EQ((int)m.Indices.size(), 36);

    // 16 «пикселей» Blockbench — это ровно один блок движка. Куб обязан занять
    // отрезок [0,1], иначе модели приходят в шестнадцатикратном масштабе.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const Vertex& v : m.Vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
    for (int k = 0; k < 3; ++k) {
        CHECK_NEAR(lo[k], 0.0f, 1e-6f);
        CHECK_NEAR(hi[k], 1.0f, 1e-6f);
    }

    // Нормали — единичные и осевые, по одной паре граней на ось.
    int axisFaces[3] = {0, 0, 0};
    for (const Vertex& v : m.Vertices) {
        CHECK_NEAR(glm::length(v.Normal), 1.0f, 1e-5f);
        for (int k = 0; k < 3; ++k)
            if (std::abs(v.Normal[k]) > 0.5f) axisFaces[k]++;
    }
    for (int k = 0; k < 3; ++k) CHECK_EQ(axisFaces[k], 8);   // 2 грани x 4 вершины

    // UV в пределах текстуры.
    for (const Vertex& v : m.Vertices) {
        CHECK_TRUE(v.TexCoords.x >= -1e-5f && v.TexCoords.x <= 1.0f + 1e-5f);
        CHECK_TRUE(v.TexCoords.y >= -1e-5f && v.TexCoords.y <= 1.0f + 1e-5f);
    }

    // Текстура доехала как материал.
    CHECK_EQ((int)scene.Materials.size(), 1);
    CHECK_TRUE(!scene.Materials.empty() &&
               scene.Materials[0].AlbedoTexture == "../textures/stone.png");
    std::remove(path.c_str());
}

TEST(blockbench_skips_faces_without_texture) {
    // Грань с "texture": null в Blockbench не рисуется — на этом держится
    // экономия внутренних граней. Импорт обязан её выбросить, а не нарисовать.
    const char* twoFaces = R"({
      "resolution": {"width": 16, "height": 16},
      "elements": [{"type":"cube","from":[0,0,0],"to":[16,16,16],
        "faces": {
          "north": {"uv":[0,0,16,16],"texture":0},
          "south": {"uv":[0,0,16,16],"texture":null},
          "up":    {"uv":[0,0,16,16],"texture":0}
        }}]
    })";
    const std::string path = WriteTemp("bb_holes.bbmodel", twoFaces);
    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportBlockbench(path, scene, err));
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (!scene.Nodes.empty()) {
        // Две грани из трёх описанных: north и up.
        CHECK_EQ((int)scene.Nodes[0].Mesh.Vertices.size(), 8);
        CHECK_EQ((int)scene.Nodes[0].Mesh.Indices.size(), 12);
    }
    std::remove(path.c_str());
}

TEST(blockbench_rotation_goes_around_origin) {
    // Поворот на 90° вокруг Y относительно центра куба обязан оставить куб на
    // месте (он симметричен), а не увезти его: origin — это шарнир, и поворот
    // вокруг нуля вместо него смещает деталь.
    // Куб описан ЦЕЛИКОМ: с одной гранью получился бы плоский четырёхугольник,
    // у которого габарит по одной оси нулевой и проверять «тот же куб» не на чем.
    const char* rotated = R"({
      "resolution": {"width": 16, "height": 16},
      "elements": [{"type":"cube","from":[0,0,0],"to":[16,16,16],
        "rotation":[0,90,0], "origin":[8,8,8],
        "faces": {
          "north": {"uv":[0,0,16,16],"texture":0},
          "south": {"uv":[0,0,16,16],"texture":0},
          "west":  {"uv":[0,0,16,16],"texture":0},
          "east":  {"uv":[0,0,16,16],"texture":0},
          "up":    {"uv":[0,0,16,16],"texture":0},
          "down":  {"uv":[0,0,16,16],"texture":0}
        }}]
    })";
    const std::string path = WriteTemp("bb_rot.bbmodel", rotated);
    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportBlockbench(path, scene, err));
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (!scene.Nodes.empty()) {
        const sage::render::MeshData flat = scene.Flatten();
        glm::vec3 lo(1e9f), hi(-1e9f);
        for (const Vertex& v : flat.Vertices) {
            lo = glm::min(lo, v.Position);
            hi = glm::max(hi, v.Position);
        }
        // Габарит после поворота — тот же [0,1] по всем осям.
        for (int k = 0; k < 3; ++k) {
            CHECK_NEAR(lo[k], 0.0f, 1e-5f);
            CHECK_NEAR(hi[k], 1.0f, 1e-5f);
        }
        // И нормаль повернулась вместе с гранью: была -Z, стала -X.
        CHECK_NEAR(flat.Vertices[0].Normal.x, -1.0f, 1e-4f);
    }
    std::remove(path.c_str());
}

TEST(blockbench_box_uv_gives_each_face_its_own_rect) {
    // box_uv раскладывает шесть граней по кресту без наложений. Проверяем, что
    // грани получили РАЗНЫЕ прямоугольники: одинаковые означали бы, что
    // раскладка не применилась и вся модель красится одним куском текстуры.
    const char* boxUv = R"({
      "meta": {"box_uv": true},
      "resolution": {"width": 64, "height": 64},
      "elements": [{"type":"cube","from":[0,0,0],"to":[16,16,16],"uv_offset":[0,0]}]
    })";
    const std::string path = WriteTemp("bb_boxuv.bbmodel", boxUv);
    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportBlockbench(path, scene, err));
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (!scene.Nodes.empty()) {
        const sage::render::MeshData& m = scene.Nodes[0].Mesh;
        CHECK_EQ((int)m.Vertices.size(), 24);   // все шесть граней
        int distinct = 0;
        for (int face = 0; face < 6; ++face) {
            const glm::vec2 a = m.Vertices[face * 4].TexCoords;
            bool unique = true;
            for (int other = 0; other < face; ++other) {
                const glm::vec2 b = m.Vertices[other * 4].TexCoords;
                if (std::abs(a.x - b.x) < 1e-6f && std::abs(a.y - b.y) < 1e-6f) unique = false;
            }
            if (unique) ++distinct;
        }
        CHECK_TRUE(distinct >= 5);   // up/down делят строку, но начала у всех разные
    }
    std::remove(path.c_str());
}

TEST(blockbench_reports_what_it_cannot_do) {
    // Модель целиком из элементов свободной формы: отказ обязан объяснить
    // причину, а не отдать пустую сцену как успех.
    const char* freeform = R"({
      "resolution": {"width": 16, "height": 16},
      "elements": [{"type":"mesh","name":"blob","vertices":{}}]
    })";
    const std::string path = WriteTemp("bb_free.bbmodel", freeform);
    ImportedScene scene;
    std::string err;
    CHECK_FALSE(ImportBlockbench(path, scene, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());

    // Не-JSON — тоже внятный отказ.
    const std::string bad = WriteTemp("bb_bad.bbmodel", "это не json {{{");
    ImportedScene s2;
    std::string err2;
    CHECK_FALSE(ImportBlockbench(bad, s2, err2));
    CHECK_FALSE(err2.empty());
    std::remove(bad.c_str());
}

TEST(blend_refuses_clearly_when_it_cannot_read) {
    // Не .blend вовсе.
    const std::string notBlend = WriteTemp("fake.blend", "PK\x03\x04 это zip");
    ImportedScene scene;
    std::string err;
    CHECK_FALSE(ImportBlend(notBlend, scene, err));
    CHECK_FALSE(err.empty());
    std::remove(notBlend.c_str());

    // Сжатый .blend (zstd — умолчание Blender 3.0+): сообщение обязано назвать
    // причину и дать выход, иначе человек видит «не загрузилось» и всё.
    std::string zstd;
    zstd.push_back((char)0x28);
    zstd.push_back((char)0xB5);
    zstd.push_back((char)0x2F);
    zstd.push_back((char)0xFD);
    zstd += "остальное неважно";
    const std::string packed = WriteTemp("packed.blend", zstd);
    ImportedScene s2;
    std::string err2;
    CHECK_FALSE(ImportBlend(packed, s2, err2));
    // Либо сказали про сжатие, либо на машине есть Blender и он не справился —
    // но пустым сообщение быть не имеет права.
    CHECK_FALSE(err2.empty());
    std::remove(packed.c_str());
}

TEST(imported_scene_flatten_bakes_transforms) {
    ImportedScene scene;
    ImportedNode a;
    a.Name = "a";
    a.Mesh = sage::render::BuildCube();
    a.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    ImportedNode b;
    b.Name = "b";
    b.Mesh = sage::render::BuildCube();
    const size_t perCube = a.Mesh.Vertices.size();
    const size_t idxPerCube = a.Mesh.Indices.size();
    scene.Nodes.push_back(std::move(a));
    scene.Nodes.push_back(std::move(b));

    const sage::render::MeshData flat = scene.Flatten();
    CHECK_EQ((int)flat.Vertices.size(), (int)(perCube * 2));
    CHECK_EQ((int)flat.Indices.size(), (int)(idxPerCube * 2));
    // Индексы второго куба сдвинуты на размер первого — иначе оба куба
    // ссылались бы на одни вершины и второй схлопнулся бы в первый.
    bool anyHigh = false;
    for (unsigned int i : flat.Indices) anyHigh = anyHigh || i >= perCube;
    CHECK_TRUE(anyHigh);

    // Трансформ запёкся: первый куб уехал по X, второй остался.
    float maxX = -1e9f, minX = 1e9f;
    for (const Vertex& v : flat.Vertices) {
        maxX = std::max(maxX, v.Position.x);
        minX = std::min(minX, v.Position.x);
    }
    CHECK_TRUE(maxX > 9.0f);
    CHECK_TRUE(minX < 0.0f);
    CHECK_EQ((int)scene.TotalTriangles(), (int)(idxPerCube * 2 / 3));
}

// ============================================================================
//  Разреженные аксессоры glTF
//
//  На них редактор ПАДАЛ. По спецификации поле bufferView у аксессора
//  необязательно: без него аксессор считается нулевым, а значения лежат в блоке
//  sparse парами «номер вершины — значение». Blender пишет так каждый ключ
//  формы, tinygltf разворачивать их не умеет и оставляет bufferView = -1, а
//  разбор скелетной модели читал bufferViews[-1] — то есть за начало вектора.
//  Не «модель приехала кривой», а падение процесса: у проверочной модели
//  (Spring Bonnie из Blender) таких аксессоров 29 из 535.
// ============================================================================
#include "GltfSparseModel.h"
#include "sage/assets/import/GltfAccessor.h"

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <filesystem>

TEST(gltf_sparse_accessor_is_expanded_not_read_out_of_bounds) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_sparse_gltf";
    const std::string path = sage_test::WriteSparseGltf(dir, "sparse");
    CHECK_TRUE(!path.empty());
    if (path.empty()) return;

    tinygltf::TinyGLTF loader;
    // Картинок в файле нет, но без загрузчика tinygltf ругается на сам разбор.
    loader.SetImageLoader([](tinygltf::Image*, const int, std::string*, std::string*, int, int,
                             const unsigned char*, int, void*) { return true; },
                          nullptr);
    tinygltf::Model g;
    std::string err, warn;
    CHECK_TRUE(loader.LoadASCIIFromFile(&g, &err, &warn, path));
    CHECK_EQ((int)g.accessors.size(), 6);
    // Именно то состояние, на котором ломался разбор: bufferView отсутствует.
    CHECK_EQ(g.accessors[5].bufferView, -1);
    CHECK_TRUE(g.accessors[5].sparse.isSparse);

    // Три вершины по три числа — ровно столько, сколько объявлено в count, а не
    // пусто и не «сколько нашлось в sparse».
    const std::vector<float> delta = sage::assets::gltf::ReadFloats(g, 5, 3);
    CHECK_EQ((int)delta.size(), 9);
    if (delta.size() == 9) {
        // Нетронутые вершины — нули (так велит спецификация), сдвинута третья.
        for (int i = 0; i < 6; ++i) CHECK_TRUE(delta[(size_t)i] == 0.0f);
        CHECK_TRUE(delta[6] == 0.0f);
        CHECK_TRUE(delta[7] == 0.0f);
        CHECK_TRUE(std::fabs(delta[8] - 0.5f) < 1e-6f);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(gltf_accessor_reader_refuses_broken_indices_instead_of_reading_memory) {
    tinygltf::Model g; // пустая модель: ни аксессоров, ни буферов
    CHECK_TRUE(sage::assets::gltf::ReadFloats(g, 0, 3).empty());
    CHECK_TRUE(sage::assets::gltf::ReadFloats(g, -1, 3).empty());
    CHECK_TRUE(sage::assets::gltf::ReadUInts(g, 7, 1).empty());
    CHECK_EQ((int)sage::assets::gltf::AccessorCount(g, 0), 0);
}

TEST(gltf_with_sparse_morph_target_still_imports_its_geometry) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_sparse_gltf_import";
    const std::string path = sage_test::WriteSparseGltf(dir, "sparse");
    CHECK_TRUE(!path.empty());
    if (path.empty()) return;

    ImportedScene scene;
    std::string err;
    const bool ok = ImporterRegistry::Instance().Import(path, scene, err);
    CHECK_TRUE(ok);
    if (!ok) std::printf("       ошибка импорта: %s\n", err.c_str());
    CHECK_EQ((int)scene.Nodes.size(), 1);
    if (!scene.Nodes.empty()) {
        CHECK_EQ((int)scene.Nodes[0].Mesh.Vertices.size(), 3);
        CHECK_EQ((int)scene.Nodes[0].Mesh.Indices.size(), 3);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ============================================================================
//  НОРМАЛИЗАЦИЯ ГЕОМЕТРИИ: одна на все форматы
//
//  Форматы договариваются о разном, а движок обязан получать одно и то же.
//  Здесь проверяется то, чего раньше не делал НИ ОДИН импортёр статической
//  геометрии: касательные для карты нормалей и нормали там, где их нет в файле.
// ============================================================================
#include "sage/assets/import/MeshNormalize.h"

TEST(import_generates_tangents_when_the_format_has_none) {
    // Куб с развёрткой, но без касательных: ровно то, что отдают OBJ, FBX и
    // добрая половина glTF. Без касательных карта нормалей в шейдере ложится
    // вдоль (1,0,0) — то есть куда попало, и рельеф «почему-то странно блестит».
    sage::render::MeshData mesh = sage::render::BuildCube();
    for (Vertex& v : mesh.Vertices) v.Tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    CHECK_FALSE(HasTangents(mesh));

    NormalizeImportedMesh(mesh);
    CHECK_TRUE(HasTangents(mesh));

    // Касательная ОРТОГОНАЛЬНА нормали и единичная: иначе базис TBN в шейдере
    // перекошен, и рельеф едет тем сильнее, чем дальше от ортогональности.
    for (const Vertex& v : mesh.Vertices) {
        const glm::vec3 t(v.Tangent);
        CHECK_NEAR(glm::length(t), 1.0f, 1e-3f);
        CHECK_NEAR(glm::dot(t, v.Normal), 0.0f, 1e-3f);
        CHECK_TRUE(std::abs(v.Tangent.w) == 1.0f);
    }
}

TEST(import_keeps_the_tangents_the_file_brought) {
    // АВТОРСКИЕ ВАЖНЕЕ ПОСЧИТАННЫХ: экспортёр знает про швы развёртки и
    // зеркальные острова то, чего по треугольникам не вывести.
    sage::render::MeshData mesh = sage::render::BuildCube();
    for (Vertex& v : mesh.Vertices) v.Tangent = glm::vec4(0.0f, 0.0f, 1.0f, -1.0f);
    NormalizeImportedMesh(mesh);
    for (const Vertex& v : mesh.Vertices) {
        CHECK_NEAR(v.Tangent.z, 1.0f, 1e-5f);
        CHECK_NEAR(v.Tangent.w, -1.0f, 1e-5f);
    }
}

TEST(import_builds_normals_when_the_file_has_none) {
    // Модель без нормалей — обычное дело для OBJ, выгруженного из CAD. Нулевая
    // нормаль в шейдере даёт чёрный пиксель, а не «плоский свет».
    sage::render::MeshData mesh = sage::render::BuildCube();
    for (Vertex& v : mesh.Vertices) v.Normal = glm::vec3(0.0f);
    CHECK_FALSE(HasNormals(mesh));

    NormalizeImportedMesh(mesh);
    CHECK_TRUE(HasNormals(mesh));
    for (const Vertex& v : mesh.Vertices) CHECK_NEAR(glm::length(v.Normal), 1.0f, 1e-3f);

    // И они смотрят НАРУЖУ: у куба нормаль сонаправлена с направлением от
    // центра. Проверка ловит перепутанный порядок обхода в формуле.
    for (const Vertex& v : mesh.Vertices)
        CHECK_TRUE(glm::dot(v.Normal, glm::normalize(v.Position)) > 0.0f);
}

TEST(import_flips_winding_for_a_mirrored_node) {
    // ЗЕРКАЛЬНАЯ ПОЛОВИНА МОДЕЛИ. Симметричные модели делают масштабом -1 по
    // оси; отражение меняет направление обхода, и отсечение задних граней
    // съедает такую часть целиком. Со стороны — «половина модели пропала».
    ImportedScene scene;
    ImportedNode node;
    node.Name = "mirrored";
    node.Mesh = sage::render::BuildCube();
    node.Transform = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
    const std::vector<unsigned int> source = node.Mesh.Indices;
    scene.Nodes.push_back(std::move(node));

    const sage::render::MeshData flat = scene.Flatten();
    CHECK_EQ((int)flat.Indices.size(), (int)source.size());
    // Первый треугольник обойдён в обратную сторону.
    CHECK_EQ((int)flat.Indices[0], (int)source[0]);
    CHECK_EQ((int)flat.Indices[1], (int)source[2]);
    CHECK_EQ((int)flat.Indices[2], (int)source[1]);

    // И геометрически это снова «лицом наружу»: нормаль треугольника по обходу
    // совпадает с нормалью вершины.
    const glm::vec3 a = flat.Vertices[flat.Indices[0]].Position;
    const glm::vec3 b = flat.Vertices[flat.Indices[1]].Position;
    const glm::vec3 c = flat.Vertices[flat.Indices[2]].Position;
    const glm::vec3 faceNormal = glm::normalize(glm::cross(b - a, c - a));
    CHECK_TRUE(glm::dot(faceNormal, flat.Vertices[flat.Indices[0]].Normal) > 0.5f);
}

TEST(import_leaves_winding_alone_without_mirroring) {
    ImportedScene scene;
    ImportedNode node;
    node.Mesh = sage::render::BuildCube();
    node.Transform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 0.5f)); // неравномерный, но не зеркальный
    const std::vector<unsigned int> source = node.Mesh.Indices;
    scene.Nodes.push_back(std::move(node));

    const sage::render::MeshData flat = scene.Flatten();
    for (size_t i = 0; i < source.size(); ++i) CHECK_EQ((int)flat.Indices[i], (int)source[i]);
}

TEST(import_pipeline_normalizes_every_format_the_same_way) {
    // НОРМАЛИЗАЦИЯ ЖИВЁТ В РЕЕСТРЕ, а не в импортёре: формат отвечает за
    // чтение, а не за то, каким движок увидит меш. Проверяется на настоящем
    // файле, прошедшем весь путь.
    //
    // Квадрат смотрит вдоль +X — и это не случайный выбор: касательная по
    // умолчанию у вершины как раз (1,0,0), то есть СОВПАДАЕТ с нормалью.
    // Базис TBN из такой пары вырожден, и карта нормалей по нему ложится
    // произвольно. Пока касательных не считал никто, ровно это и получалось у
    // каждой стены, выгруженной из OBJ.
    const std::string obj =
        "v 0 0 0\nv 0 1 0\nv 0 1 1\nv 0 0 1\n"
        "vt 0 0\nvt 0 1\nvt 1 1\nvt 1 0\n"
        "vn 1 0 0\n"
        "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
    const std::string path = WriteTemp("sage_test_wall.obj", obj);

    ImportedScene scene;
    std::string err;
    const bool ok = ImporterRegistry::Instance().Import(path, scene, err);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    if (!ok) std::printf("       ошибка импорта: %s\n", err.c_str());
    if (!ok || scene.Nodes.empty()) return;

    for (const ImportedNode& node : scene.Nodes) {
        CHECK_TRUE(HasNormals(node.Mesh));
        CHECK_TRUE(HasTangents(node.Mesh));
        for (const Vertex& v : node.Mesh.Vertices) {
            const glm::vec3 t(v.Tangent);
            CHECK_NEAR(glm::length(t), 1.0f, 1e-3f);
            // ОРТОГОНАЛЬНА нормали: то, чего не даёт значение по умолчанию.
            CHECK_NEAR(glm::dot(t, v.Normal), 0.0f, 1e-3f);
        }
    }
}

TEST(import_error_names_the_file_the_importer_and_the_reason) {
    // «Не удалось загрузить» — ответ, с которым нечего делать. У человека три
    // десятка моделей, и ему надо знать, какая, чем разбиралась и что не так.
    const std::string path = WriteTemp("sage_test_broken.gltf", "{ это не json");
    ImportedScene scene;
    std::string err;
    const bool ok = ImporterRegistry::Instance().Import(path, scene, err);
    std::remove(path.c_str());
    CHECK_FALSE(ok);
    CHECK_TRUE(err.find("sage_test_broken.gltf") != std::string::npos);
    CHECK_TRUE(err.find("импортёр") != std::string::npos);
    CHECK_TRUE(err.find("причина") != std::string::npos);
}

// ============================================================================
//  КУБИЧЕСКАЯ АНИМАЦИЯ ИЗ ФАЙЛА — ОТ .gltf ДО КЛИПА
//
//  Проверка Sample (tests/test_animation.cpp) отвечает на вопрос «правильно ли
//  считается кривая». Здесь вопрос другой: доезжают ли до клипа ТЕ ЖЕ числа,
//  что лежат в файле. Ошибка была именно тут — разбор брал из тройки
//  «касательная, значение, касательная» первую.
// ============================================================================
#include "GltfCubicAnimModel.h"
#include "sage/render/ModelData.h"
#include "sage/render/SkinnedModel.h"

TEST(gltf_cubic_animation_reads_values_not_tangents) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_cubic_gltf";
    const std::string path = sage_test::WriteCubicAnimGltf(dir, "cubic");
    CHECK_TRUE(!path.empty());
    if (path.empty()) return;

    sage::render::ModelData data;
    try {
        data = sage::render::ParseSkinnedModelFile(path);
    } catch (const std::exception& e) {
        std::printf("       разбор не удался: %s\n", e.what());
        CHECK_TRUE(false);
        return;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    CHECK_EQ((int)data.Clips.size(), 1);
    if (data.Clips.empty()) return;
    const sage::anim::AnimationClip& clip = data.Clips[0];
    CHECK_EQ(clip.Name, std::string("Turn"));
    CHECK_NEAR(clip.Duration, 1.0f, 1e-4f);
    CHECK_EQ((int)clip.Channels.size(), 1);
    if (clip.Channels.empty()) return;

    const sage::anim::AnimChannel& ch = clip.Channels[0];
    CHECK_TRUE(ch.Interp == sage::anim::AnimInterp::CubicSpline);
    CHECK_EQ((int)ch.Times.size(), 2);          // ДВА ключа, а не шесть значений
    CHECK_EQ((int)ch.Values.size(), 2);
    CHECK_EQ((int)ch.InTangents.size(), 2);
    CHECK_EQ((int)ch.OutTangents.size(), 2);

    // Поза первого ключа — единичный поворот (значение), а НЕ (0.5,0.5,0.5,0.5)
    // (касательная на входе). Ровно это и читалось раньше.
    CHECK_NEAR(ch.Values[0].w, 1.0f, 1e-4f);
    CHECK_NEAR(ch.Values[0].x, 0.0f, 1e-4f);
    CHECK_NEAR(ch.InTangents[0].x, 0.5f, 1e-4f);   // а касательная — на своём месте

    // И второй ключ — поворот на 90° вокруг Y.
    glm::vec3 v(0.0f);
    glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
    ch.Sample(0.0f, v, q);
    CHECK_NEAR(glm::degrees(glm::angle(glm::normalize(q))), 0.0f, 0.5f);
    ch.Sample(1.0f, v, q);
    CHECK_NEAR(glm::degrees(glm::angle(glm::normalize(q))), 90.0f, 0.5f);
}

// ============================================================================
//  ПОИСК ТЕКСТУР: «модель загрузилась, но стала белой»
//
//  Ссылка в файле модели почти никогда не годится как путь: glTF кодирует
//  пробелы, экспортёры пишут абсолютные пути чужих машин и обратные слэши,
//  набор карт приезжает отдельной папкой, регистр имени не совпадает. На
//  каждом из этих случаев карта молча терялась — по одному разу в каждом
//  формате, потому что поиск был у каждого свой.
// ============================================================================
#include "sage/assets/import/TextureResolve.h"

namespace {

// Каталог с моделью и набором карт: то, как выглядит скачанный ассет.
struct TextureFixture {
    std::filesystem::path Dir;
    explicit TextureFixture(const char* name) {
        Dir = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(Dir, ec);
        std::filesystem::create_directories(Dir / "textures", ec);
    }
    ~TextureFixture() {
        std::error_code ec;
        std::filesystem::remove_all(Dir, ec);
    }
    void Put(const std::string& relative) const {
        const std::filesystem::path p = Dir / relative;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << "png";
    }
};

} // namespace

TEST(texture_resolve_decodes_percent_encoding) {
    // glTF ТРЕБУЕТ кодировать пробелы в URI. Файла «body%20normal.png» на
    // диске нет никогда — и карта терялась у каждой модели, где в имени есть
    // пробел.
    TextureFixture fx("sage_tex_uri");
    fx.Put("body normal.png");
    const std::string found = ResolveTexturePath(fx.Dir, "body%20normal.png");
    CHECK_TRUE(!found.empty());
    CHECK_TRUE(found.find("body normal.png") != std::string::npos);
    CHECK_EQ(DecodeUri("a%2Fb%20c"), std::string("a/b c"));
}

TEST(texture_resolve_takes_backslashes_and_foreign_absolute_paths) {
    // В .mtl из 3ds Max абсолютный путь чужой машины — норма. Имя файла в нём
    // верное, а дорога к нему — нет.
    TextureFixture fx("sage_tex_abs");
    fx.Put("body.png");
    CHECK_TRUE(!ResolveTexturePath(fx.Dir, "C:\\Users\\artist\\Desktop\\tex\\body.png").empty());
    CHECK_TRUE(!ResolveTexturePath(fx.Dir, "..\\..\\shared\\body.png").empty());
}

TEST(texture_resolve_looks_into_the_usual_side_folders) {
    // Набор приезжает папкой: модель в корне, карты в textures/.
    TextureFixture fx("sage_tex_side");
    fx.Put("textures/skin.png");
    CHECK_TRUE(!ResolveTexturePath(fx.Dir, "skin.png").empty());
    CHECK_TRUE(!ResolveTexturePath(fx.Dir, "maps/skin.png").empty());
}

TEST(texture_resolve_ignores_case_as_a_last_resort) {
    // «Body.PNG» против «body.png»: на Windows это один файл, на Linux разные.
    TextureFixture fx("sage_tex_case");
    fx.Put("Body_Normal.PNG");
    CHECK_TRUE(!ResolveTexturePath(fx.Dir, "body_normal.png").empty());
}

TEST(texture_resolve_says_what_it_could_not_find) {
    // Молчаливая потеря текстуры хуже отказа: «модель белая» человек видит, а
    // причину — нет.
    TextureFixture fx("sage_tex_missing");
    std::vector<std::string> warnings;
    CHECK_TRUE(ResolveTexturePath(fx.Dir, "no_such.png", &warnings).empty());
    CHECK_EQ((int)warnings.size(), 1);
    if (!warnings.empty()) CHECK_TRUE(warnings[0].find("no_such.png") != std::string::npos);

    // А встроенная картинка — не потеря и не предупреждение: файла на диске у
    // неё нет по определению.
    warnings.clear();
    CHECK_TRUE(ResolveTexturePath(fx.Dir, "data:image/png;base64,iVBORw0K", &warnings).empty());
    CHECK_TRUE(warnings.empty());
}
