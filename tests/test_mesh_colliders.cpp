#include "TestFramework.h"

#include <cmath>
#include <filesystem>

#include "sage/assets/import/Importer.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/render/ModelLoader.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// КОЛЛАЙДЕРЫ УРОВНЯ: несколько Mesh Collider, вложенность, масштаб, центр.
//
// Жалоба: «работает только один Mesh Collider, остальные не сталкиваются, а
// форма не совпадает с мешем». Две причины, обе здесь проверены числами:
//   • коллайдер без Rigid Body в физику не попадал вовсе (стены и полы уровня
//     обычно именно такие) — теперь это неподвижная геометрия, как в Unity;
//   • части одной модели (объекты иерархии из импорта) получали геометрию той
//     части, что первой попала в кэш, — у всех одна и та же форма.
// ---------------------------------------------------------------------------

using namespace sage::physics;

namespace {
constexpr float kDt = 1.0f / 60.0f;

GameObject MeshBlock(Scene& scene, const glm::vec3& pos, const glm::vec3& scale, bool withBody) {
    GameObject g = scene.CreateObject("Block");
    g.EnsureRenderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    g.GetTransform().Position = pos;
    g.GetTransform().Scale = scale;
    if (withBody) scene.Registry().emplace<RigidBodyComponent>(g.Entity(), RigidBodyComponent{BodyType::Static});
    ColliderComponent c;
    c.Shape = ShapeType::Mesh;
    scene.Registry().emplace<ColliderComponent>(g.Entity(), c);
    return g;
}

GameObject Crate(Scene& scene, const glm::vec3& pos) {
    GameObject b = scene.CreateObject("Crate");
    b.GetTransform().Position = pos;
    scene.Registry().emplace<RigidBodyComponent>(b.Entity(), RigidBodyComponent{BodyType::Dynamic, 5.0f});
    scene.Registry().emplace<ColliderComponent>(b.Entity());
    return b;
}

void Run(PhysicsScene& physics, Scene& scene, int frames) {
    for (int k = 0; k < frames; ++k) physics.Step(scene, kDt);
}
} // namespace

// Три пола из Mesh Collider на разной высоте — каждый держит свой ящик. С
// телом и без: коллайдер без Rigid Body — неподвижная стена мира.
TEST(MeshColliders_several_mesh_floors_each_hold_their_crate) {
    if (!PhysicsWorld::HasJolt()) return;
    for (int withBody = 0; withBody < 2; ++withBody) {
        Scene scene("MeshFloors");
        GameObject crates[3];
        for (int i = 0; i < 3; ++i) {
            const float x = -6.0f + 6.0f * i;
            MeshBlock(scene, {x, 1.0f * i, 0.0f}, {4.0f, 1.0f, 4.0f}, withBody != 0);
            crates[i] = Crate(scene, {x, 1.0f * i + 3.0f, 0.0f});
        }
        PhysicsScene physics(Backend::Jolt, scene);
        CHECK_EQ(physics.BodyCount(), 6);
        Run(physics, scene, 240);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(crates[i].GetTransform().Position.y, 1.0f * i + 1.0f, 0.02f);
    }
}

// Коллайдер без тела в Play не дописывает в объект Rigid Body: сцена после
// Stop остаётся той, что сохранена.
TEST(MeshColliders_implicit_static_body_does_not_change_the_scene) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Implicit");
    GameObject wall = MeshBlock(scene, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
    PhysicsScene physics(Backend::Jolt, scene);
    CHECK_EQ(physics.BodyCount(), 1);
    CHECK_FALSE(scene.Registry().all_of<RigidBodyComponent>(wall.Entity()));
    // Сняли коллайдер — неявное тело уходит вместе с ним.
    scene.Registry().remove<ColliderComponent>(wall.Entity());
    physics.Step(scene, kDt);
    CHECK_EQ(physics.BodyCount(), 0);
}

// Вложенный, повёрнутый и масштабированный родителем коллайдер стоит там и
// такого размера, как нарисован.
TEST(MeshColliders_nested_under_scaled_rotated_parent_match_the_drawing) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Nested");
    GameObject parent = scene.CreateObject("Root");
    parent.GetTransform().Position = {10.0f, 2.0f, 0.0f};
    parent.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    parent.GetTransform().Rotation = {0.0f, 45.0f, 0.0f};
    GameObject child = MeshBlock(scene, {0.0f, 0.0f, 0.0f}, {3.0f, 0.5f, 3.0f}, false);
    scene.SetParent(child.Entity(), parent.Entity());
    GameObject c = Crate(scene, {10.0f, 6.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    Run(physics, scene, 240);
    // Верх плиты: 2 + 0.25 * 2 (масштаб родителя) = 2.5; центр ящика — на 0.5 выше.
    CHECK_NEAR(c.GetTransform().Position.y, 3.0f, 0.02f);

    // Родителя растянули во время игры — форма ребёнка растёт вместе с ним
    // (ключ пересборки — по мировому масштабу, не по своему).
    parent.GetTransform().Scale = {2.0f, 4.0f, 2.0f};
    c.GetTransform().Position = {10.0f, 8.0f, 0.0f};
    physics.TeleportEntity(scene, c.Entity());
    Run(physics, scene, 240);
    CHECK_NEAR(c.GetTransform().Position.y, 2.0f + 0.25f * 4.0f + 0.5f, 0.02f);
}

// Неподвижный коллайдер, который переставили во время игры, сталкивается на
// новом месте, а не на старом.
TEST(MeshColliders_moved_static_collider_collides_where_it_now_is) {
    if (!PhysicsWorld::HasJolt()) return;
    for (int withBody = 0; withBody < 2; ++withBody) {
        Scene scene("Moved");
        GameObject floor = MeshBlock(scene, {0.0f, -0.5f, 0.0f}, {4.0f, 1.0f, 4.0f}, withBody != 0);
        PhysicsScene physics(Backend::Jolt, scene);
        physics.Step(scene, kDt);
        floor.GetTransform().Position = {20.0f, -0.5f, 0.0f};
        GameObject crate = Crate(scene, {20.0f, 3.0f, 0.0f});
        Run(physics, scene, 240);
        CHECK_NEAR(crate.GetTransform().Position.y, 0.5f, 0.02f);
    }
}

// Центр коллайдера сдвигает форму, не сдвигая объект (и умножается на масштаб).
TEST(MeshColliders_box_center_moves_the_shape_not_the_object) {
    if (!PhysicsWorld::HasJolt()) return;
    Scene scene("Center");
    GameObject step = scene.CreateObject("Step");
    step.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    ColliderComponent col;
    col.Center = {0.0f, 0.5f, 0.0f};   // коробка 1×1×1 стоит на опорной точке, а не вокруг неё
    scene.Registry().emplace<ColliderComponent>(step.Entity(), col);
    GameObject crate = Crate(scene, {0.0f, 5.0f, 0.0f});
    PhysicsScene physics(Backend::Jolt, scene);
    Run(physics, scene, 240);
    // Верх формы: (0.5 + 0.5) * 2 = 2; центр ящика — 2.5.
    CHECK_NEAR(crate.GetTransform().Position.y, 2.5f, 0.02f);
    CHECK_NEAR(step.GetTransform().Position.y, 0.0f, 1e-6f);
}

// Части одной модели — каждая своей формой. Раньше все получали геометрию
// той части, что первой попала в кэш.
TEST(MeshColliders_parts_of_one_model_get_their_own_geometry) {
    if (!PhysicsWorld::HasJolt()) return;
    const std::string file =
        (std::filesystem::path(SAGE_TEST_DATA_DIR) / "blockbench_skin" / "model.gltf").string();
    sage::assets::ImportedScene tree;
    glm::mat4 root(1.0f);
    std::string err;
    CHECK_TRUE(ModelLoader::LoadModelTree(file, tree, root, err));
    std::vector<int> parts;
    for (int g = 0; g < (int)tree.Groups.size() && parts.size() < 2; ++g)
        if (!tree.FlattenGroup(g).Empty()) parts.push_back(g);
    CHECK_EQ(parts.size(), size_t(2));
    if (parts.size() < 2) return;

    Scene scene("Parts");
    GameObject obj[2];
    for (int i = 0; i < 2; ++i) {
        obj[i] = scene.CreateObject("Part");
        obj[i].EnsureRenderer().Ref = MeshRef{MeshRef::Type::Model, file, parts[(size_t)i]};
        ColliderComponent c;
        c.Shape = ShapeType::Mesh;
        scene.Registry().emplace<ColliderComponent>(obj[i].Entity(), c);
    }
    PhysicsScene physics(Backend::Jolt, scene);
    CHECK_EQ(physics.BodyCount(), 2);
    for (int i = 0; i < 2; ++i) {
        const MeshGeometryPtr g = physics.ColliderGeometry(scene, obj[i].Entity(), glm::vec3(1.0f));
        CHECK_TRUE(g != nullptr);
        if (!g) continue;
        // Габарит формы — габарит СВОЕЙ части.
        const sage::render::MeshData d = tree.FlattenGroup(parts[(size_t)i]);
        glm::vec3 lo(1e9f), hi(-1e9f), glo(1e9f), ghi(-1e9f);
        for (const Vertex& v : d.Vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
        for (const glm::vec3& p : g->Points) { glo = glm::min(glo, p); ghi = glm::max(ghi, p); }
        CHECK_NEAR(glm::length(lo - glo) + glm::length(hi - ghi), 0.0f, 1e-3f);
    }
    CHECK_TRUE(physics.ColliderGeometry(scene, obj[0].Entity(), glm::vec3(1.0f)) !=
               physics.ColliderGeometry(scene, obj[1].Entity(), glm::vec3(1.0f)));
}
