// Многоматериальная модель: границы частей и материал у каждой свой.
//
// ЧТО ИМЕННО ЗДЕСЬ ПРОВЕРЯЕТСЯ. Не «файл загрузился» — он загружался и раньше,
// в том-то и была беда: модель с четырнадцатью материалами приезжала целой, но
// красилась одним. Поэтому проверяются ГРАНИЦЫ и СООТВЕТСТВИЕ: сколько частей,
// какие у них отрезки индексного буфера, какой материал у каждой и совпадает ли
// его номер с местом в списке материалов файла. Ошибка в любом из этих чисел не
// видна ни по какому «загрузилось/не загрузилось» — модель просто выглядит не
// так, как в редакторе, где её сделали.
#include "TestFramework.h"

#include "sage/assets/import/Importer.h"
#include "sage/render/MeshData.h"
#include "sage/render/MeshSimplify.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "rhi/null/NullDevice.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Material.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
using namespace sage::assets;

namespace {

fs::path TempDir(const char* name) {
    fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Два узла, два примитива, два РАЗНЫХ материала — минимальная модель, на
// которой видна разница между «одним материалом на всё» и разбором по частям.
// Второй узел ещё и сдвинут: заодно проверяется, что матрица узла доезжает.
const char* kTwoMaterialGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [
    {"name": "body", "mesh": 0},
    {"name": "glass", "mesh": 1, "translation": [10, 0, 0]}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 1}]}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "geom.bin"}],
  "materials": [
    {"name": "Corpus", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1],
                                                "metallicFactor": 1.0}},
    {"name": "Glass", "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 1, 0.3],
                                               "roughnessFactor": 0.1}}
  ]
})";

void WriteGeomBin(const fs::path& p) {
    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const unsigned short idx[3] = {0, 1, 2};
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)verts, sizeof(verts));
    f.write((const char*)idx, sizeof(idx));
}

// .obj с двумя материалами: первые две грани одним, третья другим.
const char* kTwoMaterialObj = R"(mtllib box.mtl
v 0 0 0
v 1 0 0
v 0 1 0
v 1 1 0
usemtl paint
f 1 2 3
f 2 4 3
usemtl chrome
f 1 2 4
)";

const char* kTwoMaterialMtl = R"(newmtl paint
Kd 0.9 0.1 0.1

newmtl chrome
Kd 0.5 0.5 0.5
Pm 1.0
)";

} // namespace

TEST(gltf_import_keeps_nodes_instead_of_one_lump) {
    const fs::path dir = TempDir("sage_test_submesh_nodes");
    WriteText(dir / "car.gltf", kTwoMaterialGltf);
    WriteGeomBin(dir / "geom.bin");

    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportGltf((dir / "car.gltf").string(), scene, err));
    CHECK_EQ(err, std::string());

    // Узел на примитив — с ИМЕНЕМ из файла и своим материалом. Именно этого не
    // было: ImportGltf сводился к ImportObj и отдавал один безымянный узел.
    CHECK_EQ(scene.Nodes.size(), (size_t)2);
    CHECK_EQ(scene.Nodes[0].Name, std::string("body"));
    CHECK_EQ(scene.Nodes[1].Name, std::string("glass"));
    CHECK_EQ(scene.Nodes[0].MaterialIndex, 0);
    CHECK_EQ(scene.Nodes[1].MaterialIndex, 1);

    // Материалы — все и в порядке файла: по этому порядку части и красятся.
    CHECK_EQ(scene.Materials.size(), (size_t)2);
    CHECK_EQ(scene.Materials[0].Name, std::string("Corpus"));
    CHECK_EQ(scene.Materials[1].Name, std::string("Glass"));
    CHECK_NEAR(scene.Materials[1].Opacity, 0.3f, 1e-4f);

    // Матрица узла лежит в узле, а не запечена в вершины: иначе от структуры
    // файла не осталось бы ничего, кроме координат.
    CHECK_NEAR(scene.Nodes[1].Transform[3][0], 10.0f, 1e-4f);
    CHECK_NEAR(scene.Nodes[0].Mesh.Vertices[0].Position.x, 0.0f, 1e-4f);
}

TEST(flatten_groups_nodes_by_material_into_submeshes) {
    const fs::path dir = TempDir("sage_test_submesh_flatten");
    WriteText(dir / "car.gltf", kTwoMaterialGltf);
    WriteGeomBin(dir / "geom.bin");

    ImportedScene scene;
    std::string err;
    CHECK_TRUE(ImportGltf((dir / "car.gltf").string(), scene, err));

    const sage::render::MeshData data = scene.Flatten();
    CHECK_EQ(data.Submeshes.size(), (size_t)2);
    // Отрезки идут подряд и покрывают весь буфер: дыра означала бы кусок
    // модели, который не нарисует никто.
    CHECK_EQ(data.Submeshes[0].FirstIndex, 0u);
    CHECK_EQ(data.Submeshes[0].IndexCount, 3u);
    CHECK_EQ(data.Submeshes[1].FirstIndex, 3u);
    CHECK_EQ(data.Submeshes[1].IndexCount, 3u);
    CHECK_EQ(data.Indices.size(), (size_t)6);
    // Номер материала — индекс в списке материалов файла.
    CHECK_EQ(data.Submeshes[0].Material, 0);
    CHECK_EQ(data.Submeshes[1].Material, 1);
    CHECK_EQ(data.Submeshes[0].Name, std::string("body"));

    // Матрица узла запекается ИМЕННО ЗДЕСЬ — при склейке в один меш.
    const unsigned int second = data.Indices[3];
    CHECK_NEAR(data.Vertices[second].Position.x, 10.0f, 1e-4f);
}

TEST(obj_faces_are_split_by_their_material) {
    const fs::path dir = TempDir("sage_test_submesh_obj");
    WriteText(dir / "box.obj", kTwoMaterialObj);
    WriteText(dir / "box.mtl", kTwoMaterialMtl);

    std::vector<ImportedMaterial> materials;
    const sage::render::MeshData data =
        ModelLoader::LoadObjData((dir / "box.obj").string(), &materials);

    CHECK_EQ(materials.size(), (size_t)2);
    CHECK_EQ(materials[0].Name, std::string("paint"));
    CHECK_EQ(materials[1].Name, std::string("chrome"));

    // Две грани одним материалом, одна другим — 6 и 3 индекса.
    CHECK_EQ(data.Submeshes.size(), (size_t)2);
    CHECK_EQ(data.Submeshes[0].IndexCount, 6u);
    CHECK_EQ(data.Submeshes[1].IndexCount, 3u);
    CHECK_EQ(data.Submeshes[0].Material, 0);
    CHECK_EQ(data.Submeshes[1].Material, 1);
    CHECK_EQ(data.Submeshes[1].Name, std::string("chrome"));
}

TEST(single_material_model_has_no_markup_to_show) {
    // Одна часть без материала — это «геометрия как есть», а не разметка из
    // одного элемента: иначе инспектор показывал бы слот-пустышку у каждой
    // модели без .mtl.
    const fs::path dir = TempDir("sage_test_submesh_bare");
    WriteText(dir / "bare.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    const sage::render::MeshData data = ModelLoader::LoadObjData((dir / "bare.obj").string());
    CHECK_TRUE(data.Submeshes.empty());
    // Но потребителю всё равно достаётся один подмеш на всю геометрию.
    CHECK_EQ(data.SubmeshesOrWhole().size(), (size_t)1);
    CHECK_EQ(data.SubmeshesOrWhole()[0].IndexCount, 3u);
}

TEST(model_material_extract_returns_every_material) {
    const fs::path dir = TempDir("sage_test_submesh_materials");
    WriteText(dir / "car.gltf", kTwoMaterialGltf);
    WriteGeomBin(dir / "geom.bin");

    const ModelLoader::ExtractedMaterialSet set =
        ModelLoader::ExtractMaterials((dir / "car.gltf").string());
    CHECK_TRUE(set.Found());
    CHECK_EQ(set.Materials.size(), (size_t)2);
    // Порядок — порядок файла: по нему части модели и находят свой материал.
    CHECK_EQ(set.Materials[0].Name, std::string("Corpus"));
    CHECK_NEAR(set.Materials[0].Metallic, 1.0f, 1e-4f);
    CHECK_EQ(set.Materials[1].Name, std::string("Glass"));
    CHECK_NEAR(set.Materials[1].Roughness, 0.1f, 1e-4f);
    CHECK_NEAR(set.Materials[1].Opacity, 0.3f, 1e-4f);

    // Старая однорезультатная дверь осталась и отдаёт ПЕРВЫЙ материал.
    const ModelLoader::ExtractedMaterial first =
        ModelLoader::ExtractMaterial((dir / "car.gltf").string());
    CHECK_TRUE(first.Found);
    CHECK_EQ(first.Name, std::string("Corpus"));
}

TEST(simplify_carries_the_markup_over_to_coarse_levels) {
    // Сетка 2x2 квадратов: у грубого уровня треугольников меньше, но границы
    // частей обязаны остаться — иначе переход на LOD перекрасил бы модель
    // целиком, а это самый заметный из возможных артефактов подмены.
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    for (int i = 0; i < 12; ++i) {
        Vertex v{};
        v.Position = glm::vec3((float)(i % 4) * 0.01f, (float)(i / 4), 0.0f);
        v.Normal = glm::vec3(0, 0, 1);
        verts.push_back(v);
    }
    for (unsigned int i = 0; i + 2 < 12; ++i) { idx.push_back(i); idx.push_back(i + 1); idx.push_back(i + 2); }

    std::vector<sage::render::Submesh> markup = {
        {"a", 0u, 15u, 0},
        {"b", 15u, (unsigned int)idx.size() - 15u, 1},
    };
    const sage::render::SimplifyResult r =
        sage::render::SimplifyByClustering(verts, idx, 2, markup);

    CHECK_TRUE(r.Triangles < r.SourceTriangles);   // упрощение и правда случилось
    CHECK_EQ(r.Submeshes.size(), (size_t)2);
    CHECK_EQ(r.Submeshes[0].Material, 0);
    CHECK_EQ(r.Submeshes[1].Material, 1);
    // Отрезки по-прежнему подряд и покрывают ровно то, что осталось.
    CHECK_EQ(r.Submeshes[0].FirstIndex, 0u);
    CHECK_EQ(r.Submeshes[0].IndexCount + r.Submeshes[1].IndexCount,
             (unsigned int)r.Indices.size());
    CHECK_EQ(r.Submeshes[1].FirstIndex, r.Submeshes[0].IndexCount);
}

TEST(simplify_drops_markup_it_cannot_carry_honestly) {
    // Разметка с дырой: перенести её нельзя, и лучше отдать грубый уровень БЕЗ
    // разметки, чем с границами, поехавшими на пару треугольников — это красило
    // бы части модели чужими материалами.
    std::vector<Vertex> verts(6);
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i].Position = glm::vec3((float)i, 0.0f, 0.0f);
        verts[i].Normal = glm::vec3(0, 0, 1);
    }
    const std::vector<unsigned int> idx = {0, 1, 2, 3, 4, 5};
    const std::vector<sage::render::Submesh> gappy = {{"a", 0u, 3u, 0}};  // хвост не покрыт

    const sage::render::SimplifyResult r =
        sage::render::SimplifyByClustering(verts, idx, 64, gappy);
    CHECK_TRUE(r.Submeshes.empty());
}

// --- Отрисовка ----------------------------------------------------------------
//
// Здесь и заканчивается вся цепочка: части модели должны уйти на видеокарту
// РАЗНЫМИ вызовами, каждая со своим материалом. Проверяется через Null-бэкенд —
// без GL, но тем же самым проходом RenderBatch, каким рисуется настоящая сцена.
TEST(render_batch_draws_a_call_per_submesh) {
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    CHECK_TRUE(device != nullptr);
    device->Init(nullptr);
    sage::rhi::GraphicsDevice::SetCurrent(device.get());

    // Два треугольника — по одному на часть.
    std::vector<Vertex> verts(6);
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i].Position = glm::vec3((float)(i % 3), (float)(i / 3), 0.0f);
        verts[i].Normal = glm::vec3(0, 0, 1);
    }
    const std::vector<unsigned int> idx = {0, 1, 2, 3, 4, 5};
    const std::vector<sage::render::Submesh> markup = {{"body", 0u, 3u, 0}, {"glass", 3u, 3u, 1}};

    Scene scene("draw");
    GameObject obj = scene.CreateObject("Car");
    MeshRendererComponent& mr = obj.Renderer();
    mr.MeshPtr = std::make_shared<Mesh>(verts, idx, markup);

    auto red = std::make_shared<Material>();
    red->Albedo = {1.0f, 0.0f, 0.0f};
    auto blue = std::make_shared<Material>();
    blue->Albedo = {0.0f, 0.0f, 1.0f};
    mr.Slots.resize(2);
    mr.Slots[0].Ptr = red;
    mr.Slots[1].Ptr = blue;

    sage::ecs::RenderBatch batch;
    LightingEnvironment env;
    ShadowBinding shadows;
    const glm::mat4 view(1.0f);
    const glm::mat4 proj = glm::perspective(1.0f, 1.0f, 0.1f, 100.0f);

    const sage::ecs::RenderStats stats =
        batch.RenderColor(scene, view, proj, glm::vec3(0.0f, 0.0f, 5.0f), env, shadows, 0);

    // Одна сущность — но два вызова отрисовки: по одному на часть. Пока
    // подмешей не было, здесь стояла бы единица, и вторая часть красилась бы
    // материалом первой.
    CHECK_EQ(stats.Drawn, 1);
    CHECK_EQ(stats.Batches, 2);
    CHECK_EQ(stats.Triangles, (long long)2);

    // Контроль: тот же меш БЕЗ разметки — один вызов, как было всегда.
    Scene plain("draw-plain");
    GameObject one = plain.CreateObject("Crate");
    one.Renderer().MeshPtr = std::make_shared<Mesh>(verts, idx);
    one.Renderer().MaterialPtr = red;
    const sage::ecs::RenderStats plainStats =
        batch.RenderColor(plain, view, proj, glm::vec3(0.0f, 0.0f, 5.0f), env, shadows, 0);
    CHECK_EQ(plainStats.Batches, 1);
}

// Слот, оставленный пустым, — это «красить материалом объекта», а не «не
// рисовать». Модель, которой прописали материал только для головы, обязана
// остаться целой.
TEST(empty_slot_falls_back_to_the_object_material) {
    MeshRendererComponent mr;
    auto skin = std::make_shared<Material>();
    auto jacket = std::make_shared<Material>();
    mr.MaterialPtr = skin;
    mr.Slots.resize(2);
    mr.Slots[1].Ptr = jacket;

    CHECK_TRUE(MaterialForSubmesh(mr, 0) == skin.get());   // слот пуст — материал объекта
    CHECK_TRUE(MaterialForSubmesh(mr, 1) == jacket.get());
    // Часть за пределами слотов — тоже материал объекта: меш могли
    // переимпортировать, и частей стало больше, чем слотов.
    CHECK_TRUE(MaterialForSubmesh(mr, 7) == skin.get());
}
