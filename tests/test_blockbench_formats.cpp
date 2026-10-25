#include "TestFramework.h"

#include <filesystem>
#include <string>

#include <glm/glm.hpp>

#include "sage/assets/import/Importer.h"
#include "sage/render/ModelData.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/render/SkinnedModel.h"

// ---------------------------------------------------------------------------
// ОДНА МОДЕЛЬ BLOCKBENCH В ЧЕТЫРЁХ ФОРМАТАХ (tests/data/blockbench_skin).
//
// Персонаж 64 x 64, выгруженный из Blockbench как .gltf, .glb (со скелетом),
// .fbx и .obj. На нём нашлось всё сразу: текстовый FBX не открывался,
// пиксельная текстура сглаживалась в мыло, у .glb с тремя скинами ноги
// сходились в одну точку под телом, а его статическая копия (рамка выбора,
// габариты) стояла ниже пола. Правильный ответ у всех четырёх один и тот же —
// он и проверяется.
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;
using namespace sage::assets;

namespace {

std::string Fixture(const char* name) {
    return (fs::path(SAGE_TEST_DATA_DIR) / "blockbench_skin" / name).string();
}

// Габариты собранной модели (узлы — со своими трансформами).
bool Bounds(const std::string& path, glm::vec3& lo, glm::vec3& hi, size_t* nodes = nullptr) {
    ImportedScene scene;
    std::string err;
    if (!ImporterRegistry::Instance().Import(path, scene, err)) return false;
    if (nodes) *nodes = scene.Nodes.size();
    const sage::render::MeshData flat = scene.Flatten();
    lo = glm::vec3(1e9f);
    hi = glm::vec3(-1e9f);
    for (const Vertex& v : flat.Vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
    return !flat.Vertices.empty();
}

// Так стоит модель в Blockbench: ступни на полу, макушка шляпы на 2.03.
const glm::vec3 kLo(-0.516f, -0.016f, -0.281f);
const glm::vec3 kHi(0.516f, 2.031f, 0.281f);

void CheckBounds(const glm::vec3& lo, const glm::vec3& hi) {
    for (int a = 0; a < 3; ++a) {
        CHECK_NEAR(lo[a], kLo[a], 2e-3);
        CHECK_NEAR(hi[a], kHi[a], 2e-3);
    }
}

} // namespace

TEST(blockbench_ascii_fbx_imports_like_gltf) {
    // Blockbench пишет FBX только текстом — раньше такой файл отвергался.
    glm::vec3 lo, hi;
    size_t nodes = 0;
    CHECK_TRUE(Bounds(Fixture("model.fbx"), lo, hi, &nodes));
    CHECK_EQ(nodes, (size_t)12);   // голова, тело, руки, ноги и их внешние слои
    CheckBounds(lo, hi);
}

TEST(blockbench_every_format_stands_the_same) {
    for (const char* f : {"model.gltf", "model.obj", "model.fbx", "model.glb"}) {
        glm::vec3 lo, hi;
        CHECK_TRUE(Bounds(Fixture(f), lo, hi));
        CheckBounds(lo, hi);
    }
}

TEST(blockbench_skinned_glb_keeps_each_root_offset) {
    // Три скина с тремя корнями: пояс и две ноги. Поза привязки по скелету
    // обязана поставить ноги по сторонам, а не в одну точку под поясом.
    sage::render::ModelData data = sage::render::ParseSkinnedModelFile(Fixture("model.glb"));
    const sage::anim::Skeleton& sk = data.Skeleton;
    std::vector<glm::mat4> palette((size_t)sk.Count(), glm::mat4(1.0f));
    for (int i = 0; i < sk.Count(); ++i) {
        glm::mat4 g = sk.Joints[(size_t)i].LocalMatrix();
        for (int p = sk.Joints[(size_t)i].Parent; p >= 0; p = sk.Joints[(size_t)p].Parent)
            g = sk.Joints[(size_t)p].LocalMatrix() * g;
        palette[(size_t)i] = sk.Root * g * sk.Joints[(size_t)i].InverseBind;
    }
    glm::vec3 lo(1e9f), hi(-1e9f);
    float leftLegX = 0.0f, rightLegX = 0.0f;
    for (const sage::render::ModelSubMeshData& sm : data.SubMeshes) {
        glm::vec3 c(0.0f);
        for (const sage::render::SkinnedVertex& v : sm.Vertices) {
            glm::mat4 m(0.0f);
            for (int k = 0; k < 4; ++k)
                if (v.Weights[k] > 0.0f) m += palette[(size_t)v.Joints[k]] * v.Weights[k];
            const glm::vec3 p = glm::vec3(m * glm::vec4(v.Position, 1.0f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
            c += p;
        }
        c /= (float)std::max<size_t>(1, sm.Vertices.size());
        if (sm.Vertices.size() == 48 && c.y < 0.5f) {   // ноги
            if (c.x > 0.0f) rightLegX = c.x;
            else leftLegX = c.x;
        }
    }
    CheckBounds(lo, hi);
    CHECK_NEAR(rightLegX, 0.119, 0.01);
    CHECK_NEAR(leftLegX, -0.119, 0.01);
}

TEST(blockbench_pixel_texture_is_taken_by_nearest) {
    // Скин 64 x 64 — рисунок по пикселям: сглаживание размывает его в мыло.
    // glTF говорит это сэмплером (NEAREST), .obj и FBX сэмплера не несут —
    // их узнаём по размеру картинки. Данные модели пишутся рядом с ней, поэтому
    // образцы копируются во временную папку.
    const fs::path dir = fs::temp_directory_path() / "sage_bb_nearest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const char* f : {"model.gltf", "model.obj", "materials.mtl", "model.fbx", "05119a8b74f4a93d.png"})
        fs::copy_file(Fixture(f), dir / f, fs::copy_options::overwrite_existing, ec);
    for (const char* f : {"model.gltf", "model.obj", "model.fbx"}) {
        const ModelLoader::ExtractedMaterialSet set = ModelLoader::ExtractMaterials((dir / f).string());
        CHECK_TRUE(set.Found());
        bool anyTextured = false;
        for (const ModelLoader::ExtractedMaterial& m : set.Materials) {
            if (m.AlbedoMap.empty()) continue;
            anyTextured = true;
            CHECK_TRUE(m.NearestFilter);
        }
        CHECK_TRUE(anyTextured);
    }
    fs::remove_all(dir, ec);
}

TEST(blockbench_tree_parts_add_up_to_the_whole_model) {
    // Дерево узлов (для постановки иерархией): части в СВОИХ координатах,
    // поставленные цепочкой локальных матриц, обязаны сложиться ровно в ту же
    // модель, что и целиком.
    for (const char* f : {"model.gltf", "model.fbx", "model.obj"}) {
        ImportedScene scene;
        glm::mat4 root(1.0f);
        std::string err;
        CHECK_TRUE(ModelLoader::LoadModelTree(Fixture(f), scene, root, err));
        CHECK_TRUE(scene.Groups.size() >= 12);
        bool head = false;
        for (const ImportedGroup& g : scene.Groups) head = head || g.Name == "Head" || g.Name == "Head_1";
        CHECK_TRUE(head);
        glm::vec3 lo(1e9f), hi(-1e9f);
        int parts = 0;
        for (int g = 0; g < (int)scene.Groups.size(); ++g) {
            const sage::render::MeshData part = scene.FlattenGroup(g);
            if (part.Empty()) continue;
            ++parts;
            glm::mat4 w = scene.GroupLocal(g);
            for (int p = scene.Groups[(size_t)g].Parent; p >= 0; p = scene.Groups[(size_t)p].Parent)
                w = scene.GroupLocal(p) * w;
            for (const Vertex& v : part.Vertices) {
                const glm::vec3 q = glm::vec3(root * w * glm::vec4(v.Position, 1.0f));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
        }
        CHECK_EQ(parts, 12);
        CheckBounds(lo, hi);
    }
}
