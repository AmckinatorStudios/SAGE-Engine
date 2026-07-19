// Модульные тесты иерархии родитель/ребёнок: композиция мировых матриц,
// предотвращение циклов, рекурсивное удаление поддерева. Чистый ECS — без GL.
#include "TestFramework.h"

#include <glm/glm.hpp>

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/ecs/CameraView.h"

TEST(Hierarchy_child_inherits_parent_translation) {
    Scene scene("H");
    GameObject parent = scene.CreateObject("Parent");
    parent.GetTransform().Position = {10.0f, 0.0f, 0.0f};
    GameObject child = scene.CreateObject("Child");
    child.GetTransform().Position = {0.0f, 5.0f, 0.0f};
    scene.SetParent(child.Entity(), parent.Entity());

    // Мировая позиция ребёнка = родитель + локальная = (10,5,0).
    glm::vec4 wp = scene.WorldMatrix(child.Entity()) * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(wp.x, 10.0f, 1e-4);
    CHECK_NEAR(wp.y, 5.0f, 1e-4);
    CHECK_NEAR(wp.z, 0.0f, 1e-4);
}

TEST(Hierarchy_parent_rotation_affects_child) {
    Scene scene("H");
    GameObject parent = scene.CreateObject("Parent");
    parent.GetTransform().Rotation = {0.0f, 90.0f, 0.0f}; // поворот вокруг Y на 90°
    GameObject child = scene.CreateObject("Child");
    child.GetTransform().Position = {1.0f, 0.0f, 0.0f};   // локально по +X
    scene.SetParent(child.Entity(), parent.Entity());

    // Поворот родителя Y90 переносит локальную +X ребёнка в мировую -Z.
    glm::vec4 wp = scene.WorldMatrix(child.Entity()) * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(wp.x, 0.0f, 1e-4);
    CHECK_NEAR(wp.z, -1.0f, 1e-4);
}

TEST(Hierarchy_three_levels_compose) {
    Scene scene("H");
    GameObject a = scene.CreateObject("A"); a.GetTransform().Position = {1, 0, 0};
    GameObject b = scene.CreateObject("B"); b.GetTransform().Position = {0, 2, 0};
    GameObject c = scene.CreateObject("C"); c.GetTransform().Position = {0, 0, 3};
    scene.SetParent(b.Entity(), a.Entity());
    scene.SetParent(c.Entity(), b.Entity());
    // Мировая позиция C = (1,2,3) (сумма переносов по цепочке).
    glm::vec4 wp = scene.WorldMatrix(c.Entity()) * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(wp.x, 1.0f, 1e-4);
    CHECK_NEAR(wp.y, 2.0f, 1e-4);
    CHECK_NEAR(wp.z, 3.0f, 1e-4);
}

TEST(Hierarchy_cycle_is_prevented) {
    Scene scene("H");
    GameObject a = scene.CreateObject("A");
    GameObject b = scene.CreateObject("B");
    scene.SetParent(b.Entity(), a.Entity());   // B — ребёнок A
    scene.SetParent(a.Entity(), b.Entity());   // попытка сделать A ребёнком B — цикл, отклоняется

    // Родитель A должен остаться null (цикл не создан).
    CHECK_TRUE(scene.ParentOf(a.Entity()) == entt::null);
    CHECK_TRUE(scene.ParentOf(b.Entity()) == a.Entity());
}

TEST(Hierarchy_reparent_moves_between_parents) {
    Scene scene("H");
    GameObject p1 = scene.CreateObject("P1");
    GameObject p2 = scene.CreateObject("P2");
    GameObject c = scene.CreateObject("C");
    scene.SetParent(c.Entity(), p1.Entity());
    scene.SetParent(c.Entity(), p2.Entity()); // переносим к p2

    CHECK_TRUE(scene.ParentOf(c.Entity()) == p2.Entity());
    // p1 больше не считает c ребёнком.
    auto& reg = scene.Registry();
    const HierarchyComponent* h1 = reg.try_get<HierarchyComponent>(p1.Entity());
    bool inP1 = false;
    if (h1) for (auto k : h1->Children) if (k == c.Entity()) inP1 = true;
    CHECK_FALSE(inP1);
}

TEST(Hierarchy_destroy_parent_removes_subtree) {
    Scene scene("H");
    GameObject a = scene.CreateObject("A");
    GameObject b = scene.CreateObject("B");
    GameObject c = scene.CreateObject("C");
    scene.SetParent(b.Entity(), a.Entity());
    scene.SetParent(c.Entity(), b.Entity());
    int aId = a.Id(), bId = b.Id(), cId = c.Id();

    CHECK_EQ((int)scene.Count(), 3);
    scene.RemoveObject(aId); // удаляет A вместе с B и C
    CHECK_EQ((int)scene.Count(), 0);
    CHECK_FALSE(scene.Get(bId).Valid());
    CHECK_FALSE(scene.Get(cId).Valid());
}

TEST(Hierarchy_unparent_to_root) {
    Scene scene("H");
    GameObject p = scene.CreateObject("P");
    GameObject c = scene.CreateObject("C");
    scene.SetParent(c.Entity(), p.Entity());
    scene.SetParent(c.Entity(), entt::null); // открепить в корень
    CHECK_TRUE(scene.ParentOf(c.Entity()) == entt::null);
}

TEST(Camera_frame_none_when_no_primary) {
    Scene scene("C");
    scene.CreateObject("JustACube");
    sage::ecs::CameraFrame f = sage::ecs::PrimaryCameraFrame(scene, 16.0f / 9.0f);
    CHECK_FALSE(f.HasPrimary); // нет камеры — вызывающий покажет fallback/подсказку
}

TEST(Camera_frame_uses_world_transform_of_parented_camera) {
    // Ключ анти-«фальшивости»: кадр камеры берётся из МИРОВОЙ матрицы. Раньше
    // рантайм брал ЛОКАЛЬНЫЙ Transform — вложенная камера в игре смотрела не
    // туда, что показывало превью. Теперь и превью, и игра идут через этот хелпер.
    Scene scene("C");
    GameObject rig = scene.CreateObject("Rig");
    rig.GetTransform().Position = {10.0f, 0.0f, 0.0f};
    GameObject cam = scene.CreateObject("Cam");
    cam.GetTransform().Position = {0.0f, 2.0f, 5.0f}; // ЛОКАЛЬНО относительно rig
    scene.Registry().emplace<CameraComponent>(cam.Entity());
    scene.SetParent(cam.Entity(), rig.Entity());

    sage::ecs::CameraFrame f = sage::ecs::PrimaryCameraFrame(scene, 16.0f / 9.0f);
    CHECK_TRUE(f.HasPrimary);
    // Мировая позиция = родитель + локальная = (10, 2, 5), НЕ локальная (0,2,5).
    CHECK_NEAR(f.Position.x, 10.0f, 1e-3);
    CHECK_NEAR(f.Position.y, 2.0f, 1e-3);
    CHECK_NEAR(f.Position.z, 5.0f, 1e-3);
    // Камера без поворота смотрит вдоль -Z мира: точка на 5 впереди (world z=0)
    // в видовом пространстве имеет отрицательный z (перед камерой).
    glm::vec4 vp = f.View * glm::vec4(10.0f, 2.0f, 0.0f, 1.0f);
    CHECK_TRUE(vp.z < 0.0f);
}

TEST(Hierarchy_compute_world_matrices_matches_recursive) {
    // Мемоизированный проход ComputeWorldMatrices (оптимизация рендера) обязан
    // давать в точности те же матрицы, что рекурсивный WorldMatrix per-entity.
    Scene scene("H");
    GameObject root = scene.CreateObject("Root");
    root.GetTransform().Position = {1.0f, 2.0f, 3.0f};
    root.GetTransform().Rotation = {0.0f, 45.0f, 0.0f};
    GameObject mid = scene.CreateObject("Mid");
    mid.GetTransform().Position = {0.0f, 1.0f, 0.0f};
    mid.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    GameObject leaf = scene.CreateObject("Leaf");
    leaf.GetTransform().Position = {0.5f, 0.0f, -0.5f};
    GameObject lone = scene.CreateObject("Lone"); // без родителя
    lone.GetTransform().Position = {-4.0f, 0.0f, 7.0f};
    scene.SetParent(mid.Entity(), root.Entity());
    scene.SetParent(leaf.Entity(), mid.Entity());

    std::unordered_map<entt::entity, glm::mat4> memo;
    scene.ComputeWorldMatrices(memo);

    for (GameObject o : {root, mid, leaf, lone}) {
        auto it = memo.find(o.Entity());
        CHECK_TRUE(it != memo.end());
        glm::mat4 expect = scene.WorldMatrix(o.Entity());
        const glm::mat4& got = it->second;
        for (int c2 = 0; c2 < 4; ++c2)
            for (int r = 0; r < 4; ++r)
                CHECK_NEAR(got[c2][r], expect[c2][r], 1e-4);
    }
}
