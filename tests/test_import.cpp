// Реестр импортёров и разбор чужих форматов.
//
// Blockbench проверяется по геометрии, а не по «функция вернула true»:
// координаты углов, число граней, влияние поворота и то, что грань без текстуры
// не рисуется. Всё это можно посчитать на бумаге, поэтому и проверяется точно.
#include "TestFramework.h"

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
    CHECK_FALSE(reg.CanImport(".fbx"));

    // Отказ обязан перечислить, что движок умеет: человеку с .fbx нужно знать,
    // во что пересохранить, а не только что его не приняли.
    ImportedScene scene;
    std::string err;
    CHECK_FALSE(reg.Import("nothing.fbx", scene, err));
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
