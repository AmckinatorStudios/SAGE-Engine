// ПОРЯДОК ОБЪЕКТОВ И ПЕРЕТАСКИВАНИЕ ГРУППЫ.
//
// Список сцены показывал объекты только «по номеру создания», и переставить
// их было нечем: бросок на строку всегда делал её родителем. Выделить можно
// было много, а перетащить — одно. Здесь проверяется то, на чём стоит новое
// поведение: место среди братьев (Scene::MoveObjects, ChildrenOf), что оно
// переживает сохранение и копирование, и что группа едет в порядке экрана
// (dragset).
#include "TestFramework.h"

#include <string>
#include <vector>

#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

#include "../editor/src/DragSet.h"

namespace {

std::vector<std::string> Names(const Scene& scene, const std::vector<entt::entity>& list) {
    std::vector<std::string> out;
    for (entt::entity e : list) out.push_back(scene.Registry().get<NameComponent>(e).Name);
    return out;
}

std::vector<std::string> RootNames(const Scene& scene) { return Names(scene, scene.Roots()); }

using V = std::vector<std::string>;

} // namespace

TEST(Order_new_scene_keeps_creation_order) {
    Scene s;
    s.CreateObject("A");
    s.CreateObject("B");
    s.CreateObject("C");
    CHECK_TRUE(RootNames(s) == (V{"A", "B", "C"}));
}

TEST(Order_group_moves_before_an_object_in_given_order) {
    Scene s;
    const int a = s.CreateObject("A").Id();
    s.CreateObject("B");
    const int c = s.CreateObject("C").Id();
    const int d = s.CreateObject("D").Id();
    // «C, D» — перед A, именно в этом порядке.
    CHECK_EQ(s.MoveObjects({c, d}, -1, a, false), 2);
    CHECK_TRUE(RootNames(s) == (V{"C", "D", "A", "B"}));
    (void)d;
}

TEST(Order_move_after_and_to_end) {
    Scene s;
    const int a = s.CreateObject("A").Id();
    const int b = s.CreateObject("B").Id();
    const int c = s.CreateObject("C").Id();
    s.MoveObjects({a}, -1, b, true);
    CHECK_TRUE(RootNames(s) == (V{"B", "A", "C"}));
    s.MoveObjects({b}, -1);   // в конец
    CHECK_TRUE(RootNames(s) == (V{"A", "C", "B"}));
    (void)c;
}

TEST(Order_group_moves_inside_a_parent_and_keeps_order) {
    Scene s;
    GameObject folder = s.CreateFolder("Folder");
    const int x = s.CreateObject("X").Id();
    const int y = s.CreateObject("Y").Id();
    const int z = s.CreateObject("Z").Id();
    CHECK_EQ(s.MoveObjects({z, x, y}, folder.Id()), 3);
    CHECK_TRUE(Names(s, s.ChildrenOf(folder.Entity())) == (V{"Z", "X", "Y"}));
    CHECK_TRUE(RootNames(s) == (V{"Folder"}));
    // Children у родителя — в том же порядке (его обходят системы и скрипты).
    const auto& kids = s.Registry().get<HierarchyComponent>(folder.Entity()).Children;
    CHECK_TRUE(Names(s, kids) == (V{"Z", "X", "Y"}));
}

TEST(Order_object_cannot_go_into_itself_or_its_child) {
    Scene s;
    GameObject parent = s.CreateObject("P");
    GameObject child = s.CreateObject("C");
    s.SetParent(child.Entity(), parent.Entity());
    CHECK_EQ(s.MoveObjects({parent.Id()}, child.Id()), 0);
    CHECK_EQ(s.MoveObjects({parent.Id()}, parent.Id()), 0);
    CHECK_TRUE(s.ParentOf(child.Entity()) == parent.Entity());
}

TEST(Order_child_selected_with_its_parent_rides_along) {
    // Папка и её содержимое выделены вместе: содержимое НЕ вытряхивается.
    Scene s;
    GameObject parent = s.CreateObject("P");
    GameObject child = s.CreateObject("C");
    s.SetParent(child.Entity(), parent.Entity());
    GameObject target = s.CreateFolder("T");
    CHECK_EQ(s.MoveObjects({parent.Id(), child.Id()}, target.Id()), 1);
    CHECK_TRUE(s.ParentOf(parent.Entity()) == target.Entity());
    CHECK_TRUE(s.ParentOf(child.Entity()) == parent.Entity());
}

TEST(Order_survives_save_and_load) {
    Scene s;
    GameObject folder = s.CreateFolder("F");
    const int a = s.CreateObject("A").Id();
    const int b = s.CreateObject("B").Id();
    const int c = s.CreateObject("C").Id();
    s.MoveObjects({c, a, b}, folder.Id());
    s.MoveObjects({folder.Id()}, -1);   // корень: папка в конец
    const std::string text = SceneSerializer::SaveToString(s);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    CHECK_TRUE(RootNames(*back) == (V{"F"}));
    const entt::entity f = back->Roots()[0];
    CHECK_TRUE(Names(*back, back->ChildrenOf(f)) == (V{"C", "A", "B"}));
}

TEST(Order_untouched_scene_file_has_no_order_keys) {
    // Порядок по номеру — файл не меняется ни на байт (никаких "order": 0).
    Scene s;
    s.CreateObject("A");
    s.CreateObject("B");
    CHECK_TRUE(SceneSerializer::SaveToString(s).find("\"order\"") == std::string::npos);
}

TEST(Order_copy_keeps_children_order) {
    Scene s;
    GameObject root = s.CreateObject("Root");
    const int a = s.CreateObject("A").Id();
    const int b = s.CreateObject("B").Id();
    s.MoveObjects({b, a}, root.Id());
    GameObject copy = sage::scene::CopySubtree(s, root.Entity(), s, entt::null);
    CHECK_TRUE(copy.Valid());
    CHECK_TRUE(Names(s, s.ChildrenOf(copy.Entity())) == (V{"B", "A"}));
}

TEST(DragSet_drags_whole_selection_in_screen_order) {
    // Выделяли снизу вверх (5, 2, 9) — едут в порядке строк (2, 5, 9).
    const std::vector<int> visible = {1, 2, 3, 5, 7, 9};
    CHECK_TRUE(dragset::Ordered(5, std::vector<int>{5, 2, 9}, visible) == (std::vector<int>{2, 5, 9}));
    // Взялись за невыделенное — тащат его одно.
    CHECK_TRUE(dragset::Ordered(3, std::vector<int>{5, 2, 9}, visible) == (std::vector<int>{3}));
    // Невидимые (в свёрнутой ветке) — следом, в порядке набора.
    CHECK_TRUE(dragset::Ordered(2, std::vector<int>{4, 2}, visible) == (std::vector<int>{2, 4}));
}

TEST(DragSet_group_belongs_to_its_payload_only) {
    dragset::BeginEntities(7, {3, 7, 8});
    CHECK_TRUE(dragset::EntitiesFor(7) == (std::vector<int>{3, 7, 8}));
    // Тащат что-то другое (слот, другой список) — группа из одного.
    CHECK_TRUE(dragset::EntitiesFor(4) == (std::vector<int>{4}));
    dragset::BeginAssets("a.png", {"a.png", "b.png"});
    CHECK_EQ(dragset::AssetsFor("a.png").size(), (size_t)2);
    CHECK_EQ(dragset::AssetsFor("c.png").size(), (size_t)1);
}

TEST(DragSet_row_zones_before_inside_after) {
    CHECK_TRUE(dragset::PlaceAt(101.0f, 100.0f, 120.0f) == dragset::Place::Before);
    CHECK_TRUE(dragset::PlaceAt(110.0f, 100.0f, 120.0f) == dragset::Place::Inside);
    CHECK_TRUE(dragset::PlaceAt(119.0f, 100.0f, 120.0f) == dragset::Place::After);
}

TEST(DragSet_click_on_a_selected_item_of_a_group_waits_for_release) {
    // Взялись за одну из пяти выделенных — набор не схлопывается на нажатии.
    CHECK_TRUE(dragset::DeferSinglePick(true, 5, false));
    // Один выделенный, невыделенный, Ctrl/Shift — выбор сразу, как раньше.
    CHECK_FALSE(dragset::DeferSinglePick(true, 1, false));
    CHECK_FALSE(dragset::DeferSinglePick(false, 5, false));
    CHECK_FALSE(dragset::DeferSinglePick(true, 5, true));
}
