// ===========================================================================
//  INTERFACE — структура интерфейса отдельным ресурсом (.sageui).
//
//  Проверяется то, ради чего ресурс и заведён: интерфейс переживает круг
//  «сцена -> файл -> сцена» целиком, вместе с компонентами и иерархией; один и
//  тот же ресурс разворачивается дважды и копии не знают друг о друге; чужие
//  объекты в него не попадают.
// ===========================================================================
#include "TestFramework.h"

#include "sage/scene/Scene.h"
#include "sage/ui/Interface.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/SceneSerializer.h"
#include <memory>

#include <string>
#include <vector>

using sage::ui::Interface;

namespace {

// Небольшой интерфейс в сцене: панель с надписью и картинкой внутри.
std::vector<entt::entity> BuildSample(Scene& scene) {
    GameObject panel = scene.CreateEmptyObject("Panel");
    sage::ui::Element box;
    box.Anchor = UIAnchor::Center;
    box.Position = {12.0f, 34.0f};
    box.Size = {400.0f, 220.0f};
    box.Rotation = 15.0f;
    box.Order = 3;
    box.Locked = true;
    scene.Registry().emplace<sage::ui::Element>(panel.Entity(), box);
    sage::ui::Fill fill;
    fill.Rounding = 18.0f;
    fill.Color = {0.2f, 0.3f, 0.4f, 0.9f};
    scene.Registry().emplace<sage::ui::Fill>(panel.Entity(), fill);

    GameObject label = scene.CreateEmptyObject("Caption");
    sage::ui::Element lb;
    lb.Size = {200.0f, 40.0f};
    scene.Registry().emplace<sage::ui::Element>(label.Entity(), lb);
    sage::ui::Label text;
    text.Text = "Инвентарь";
    text.Scale = 3.5f;
    scene.Registry().emplace<sage::ui::Label>(label.Entity(), text);
    scene.SetParent(label.Entity(), panel.Entity());

    GameObject icon = scene.CreateEmptyObject("Icon");
    sage::ui::Element ib;
    ib.Size = {64.0f, 64.0f};
    scene.Registry().emplace<sage::ui::Element>(icon.Entity(), ib);
    sage::ui::Image img;
    img.Path = "assets/ui/bag.png";
    img.SliceBorder = {8.0f, 8.0f, 8.0f, 8.0f};
    img.SliceEdgeFill = sage::ui::SliceFill::Tile;
    scene.Registry().emplace<sage::ui::Image>(icon.Entity(), img);
    scene.SetParent(icon.Entity(), panel.Entity());

    return {panel.Entity()};
}

GameObject FindByName(Scene& scene, const std::string& name) {
    for (auto e : scene.Registry().view<NameComponent>()) {
        if (scene.Registry().get<NameComponent>(e).Name == name)
            return GameObject(&scene.Registry(), e);
    }
    return GameObject{};
}

} // namespace

TEST(Interface_survives_scene_to_file_to_scene) {
    Scene source("S");
    const Interface ui = sage::ui::Capture(source, BuildSample(source));
    CHECK_EQ((int)ui.Roots.size(), 1);

    std::string err;
    Interface back;
    CHECK_TRUE(Interface::FromJsonString(ui.ToJsonString(), back, err));

    Scene target("T");
    const std::vector<entt::entity> roots = sage::ui::Instantiate(target, back);
    CHECK_EQ((int)roots.size(), 1);

    // Раскладка элемента дошла целиком — включая то, чего раньше не было вовсе.
    GameObject panel = FindByName(target, "Panel");
    CHECK_TRUE(panel.Valid());
    if (panel.Valid()) {
        const sage::ui::Element& box = target.Registry().get<sage::ui::Element>(panel.Entity());
        CHECK_TRUE(box.Anchor == UIAnchor::Center);
        CHECK_NEAR(box.Position.x, 12.0f, 0.001f);
        CHECK_NEAR(box.Size.y, 220.0f, 0.001f);
        CHECK_NEAR(box.Rotation, 15.0f, 0.001f);
        CHECK_EQ(box.Order, 3);
        CHECK_TRUE(box.Locked);
        const sage::ui::Fill& fill = target.Registry().get<sage::ui::Fill>(panel.Entity());
        CHECK_NEAR(fill.Rounding, 18.0f, 0.001f);
    }

    // Дети и их компоненты — тоже. Иерархия ресурса и есть иерархия сцены.
    GameObject caption = FindByName(target, "Caption");
    CHECK_TRUE(caption.Valid());
    if (caption.Valid()) {
        CHECK_TRUE(target.Registry().get<sage::ui::Label>(caption.Entity()).Text == "Инвентарь");
        const HierarchyComponent* h =
            target.Registry().try_get<HierarchyComponent>(caption.Entity());
        CHECK_TRUE(h && h->Parent == panel.Entity());
    }
    GameObject icon = FindByName(target, "Icon");
    CHECK_TRUE(icon.Valid());
    if (icon.Valid()) {
        const sage::ui::Image& img = target.Registry().get<sage::ui::Image>(icon.Entity());
        CHECK_TRUE(img.Path == "assets/ui/bag.png");
        CHECK_NEAR(img.SliceBorder.x, 8.0f, 0.001f);
        CHECK_TRUE(img.SliceEdgeFill == sage::ui::SliceFill::Tile);
    }
}

TEST(Interface_can_be_instantiated_twice_without_the_copies_knowing) {
    // Два окна инвентаря рядом — это один ресурс, развёрнутый дважды. Правка
    // одной копии не имеет права задеть другую.
    Scene source("S");
    const Interface ui = sage::ui::Capture(source, BuildSample(source));

    Scene target("T");
    const std::vector<entt::entity> a = sage::ui::Instantiate(target, ui);
    const std::vector<entt::entity> b = sage::ui::Instantiate(target, ui);
    CHECK_EQ((int)a.size(), 1);
    CHECK_EQ((int)b.size(), 1);
    CHECK_TRUE(a[0] != b[0]);

    target.Registry().get<sage::ui::Element>(a[0]).Position = {999.0f, 999.0f};
    CHECK_NEAR(target.Registry().get<sage::ui::Element>(b[0]).Position.x, 12.0f, 0.001f);
}

TEST(Interface_does_not_swallow_scene_objects) {
    // 3D-объект, случайно оказавшийся внутри поддерева интерфейса, в ресурс не
    // едет: иначе .sageui начал бы возить с собой меши и материалы, которых в
    // нём быть не должно.
    Scene scene("S");
    const std::vector<entt::entity> roots = BuildSample(scene);
    GameObject box = scene.CreateObject("Ящик");   // обычный объект с мешем
    scene.SetParent(box.Entity(), roots[0]);

    const Interface ui = sage::ui::Capture(scene, roots);
    CHECK_EQ((int)ui.Roots.size(), 1);
    // У панели было двое детей-элементов; ящик третьим не стал.
    CHECK_EQ((int)ui.Roots[0].Children.size(), 2);
}

TEST(Interface_keeps_several_roots) {
    // HUD и меню паузы — два независимых дерева ОДНОГО интерфейса. Заворачивать
    // их в общий пустой узел ради одного корня значит добавить в дерево строку,
    // которая ничего не значит.
    Scene scene("S");
    std::vector<entt::entity> roots;
    for (const char* name : {"HUD", "PauseMenu"}) {
        GameObject r = scene.CreateEmptyObject(name);
        scene.Registry().emplace<sage::ui::Element>(r.Entity());
        roots.push_back(r.Entity());
    }
    const Interface ui = sage::ui::Capture(scene, roots);
    CHECK_EQ((int)ui.Roots.size(), 2);

    Scene target("T");
    CHECK_EQ((int)sage::ui::Instantiate(target, ui).size(), 2);
}

TEST(Interface_a_broken_file_reports_instead_of_throwing) {
    Interface out;
    std::string err;
    CHECK_FALSE(Interface::FromJsonString("{это не json", out, err));
    CHECK_FALSE(err.empty());
}

TEST(Interface_refuses_a_file_from_a_newer_engine) {
    // Половина полей такому движку незнакома, и молча потерять их хуже, чем
    // честно отказаться.
    Interface out;
    std::string err;
    CHECK_FALSE(Interface::FromJsonString(R"({"sage_interface_version":99,"roots":[]})", out, err));
    CHECK_FALSE(err.empty());
}

TEST(Interface_presets_build_from_elements_and_components) {
    // ЗАГОТОВКА — НЕ ТИП. «Кнопка» это элемент с заливкой, реакцией на мышь и
    // ребёнком-надписью; отдельного вида элемента за ней не стоит, и проверка
    // сторожит именно это: у результата обычные компоненты и обычные дети.
    const sage::ui::Preset* button = sage::ui::FindPreset("Button");
    CHECK_TRUE(button != nullptr);
    if (button) {
        CHECK_TRUE(button->HasFill);
        CHECK_TRUE(button->HasInteractable);
        CHECK_FALSE(button->HasLabel);             // надпись — ОТДЕЛЬНЫЙ объект
        CHECK_EQ((int)button->Children.size(), 1);
    }

    // Пустой — тоже заготовка, и без единого компонента: им собирают всё
    // остальное.
    const sage::ui::Preset* empty = sage::ui::FindPreset("Empty");
    CHECK_TRUE(empty != nullptr);
    if (empty) {
        CHECK_FALSE(empty->HasFill);
        CHECK_FALSE(empty->HasLabel);
        CHECK_FALSE(empty->HasImage);
        CHECK_TRUE(empty->Children.empty());
    }

    // Запрошенный набор Add/Create на месте.
    for (const char* name : {"Empty", "Text", "Image", "Button", "Input Field"}) {
        if (!sage::ui::FindPreset(name))
            sagetest::ReportFail(__FILE__, __LINE__, std::string("нет заготовки: ") + name);
    }
}

TEST(Interface_old_preset_names_still_resolve) {
    // Заготовки зовут по имени из меню, из скрипта и из самопроверки, и
    // переименование не имеет права молча превратить рабочий вызов в
    // «заготовка неизвестна».
    const sage::ui::Preset* text = sage::ui::FindPreset("Label");
    CHECK_TRUE(text != nullptr);
    if (text) CHECK_TRUE(text->Name == "Text");

    const sage::ui::Preset* input = sage::ui::FindPreset("Input");
    CHECK_TRUE(input != nullptr);
    if (input) CHECK_TRUE(input->Name == "Input Field");

    CHECK_TRUE(sage::ui::FindPreset("нет такой") == nullptr);
}

// ===========================================================================
//  Операции дерева: копирование поддерева и перенос между родителями.
//
//  Буфер обмена — это тот же ресурс .sageui, и проверяется здесь именно то,
//  ради чего он сделан описанием, а не списком сущностей: скопированное
//  переживает удаление оригинала.
// ===========================================================================

TEST(Interface_a_copied_subtree_outlives_the_original) {
    // Список сущностей — это буфер, который ломается ровно тогда, когда им
    // собирались воспользоваться: скопировал панель, удалил, вставил — и
    // вставлять нечего.
    Scene scene("S");
    const std::vector<entt::entity> roots = BuildSample(scene);
    const Interface copied = sage::ui::Capture(scene, roots);

    // Удаляем оригинал целиком.
    for (auto e : roots) {
        const IdComponent* id = scene.Registry().try_get<IdComponent>(e);
        if (id) scene.RemoveObject(id->Id);
    }
    CHECK_FALSE(FindByName(scene, "Panel").Valid());

    // И вставляем из описания.
    const std::vector<entt::entity> pasted = sage::ui::Instantiate(scene, copied);
    CHECK_EQ((int)pasted.size(), 1);
    CHECK_TRUE(FindByName(scene, "Panel").Valid());
    CHECK_TRUE(FindByName(scene, "Caption").Valid());
}

TEST(Interface_pasting_into_an_element_makes_it_the_parent) {
    // «Скопировал кнопку, выбрал панель, вставил — кнопка внутри панели»: это
    // то, чего ждут, и проверка сторожит именно связь родителя.
    Scene scene("S");
    const std::vector<entt::entity> roots = BuildSample(scene);
    const Interface copied = sage::ui::Capture(scene, roots);

    GameObject host = scene.CreateEmptyObject("Host");
    scene.Registry().emplace<sage::ui::Element>(host.Entity());

    const std::vector<entt::entity> pasted = sage::ui::Instantiate(scene, copied, host.Entity());
    // Вставка под родителя не отдаёт корней наружу: корень теперь не корень.
    CHECK_TRUE(pasted.empty());
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(host.Entity());
    CHECK_TRUE(h != nullptr);
    if (h) CHECK_EQ((int)h->Children.size(), 1);
}

// ============================================================================
//  ИНТЕРФЕЙС КАК КОНТЕКСТ: несколько интерфейсов в одной сцене
// ============================================================================
//
// Жалоба звучала так: «создаю два объекта с интерфейсом, вхожу в режим вёрстки
// — элементы обоих смешаны в одном редакторе, и править один, не задевая
// другой, можно только пряча чужие объекты». Так и было: корнем считался любой
// прямоугольник без родителя-прямоугольника, то есть «интерфейс» существовал
// лишь как наблюдение «эти элементы лежат рядом».
//
// Теперь интерфейс — объект с InterfaceComponent, и элемент принадлежит
// ближайшему такому предку. Ниже проверяется именно это: границы есть, они не
// протекают, и по ним можно спросить «покажи только этот экран».

namespace {

// Интерфейс с одним корневым элементом внутри: то, что в редакторе получается
// кнопкой «создать интерфейс» и потом «создать элемент».
entt::entity MakeInterface(Scene& scene, const char* name, glm::vec2 pos, int sortOrder = 0) {
    GameObject iface = scene.CreateEmptyObject(name);
    sage::ui::InterfaceComponent info;
    info.SortOrder = sortOrder;
    scene.Registry().emplace<sage::ui::InterfaceComponent>(iface.Entity(), info);

    GameObject root = scene.CreateEmptyObject(std::string(name) + " Root");
    sage::ui::Element box;
    box.Anchor = UIAnchor::TopLeft;
    box.Position = pos;
    box.Size = {100.0f, 100.0f};
    scene.Registry().emplace<sage::ui::Element>(root.Entity(), box);
    scene.Registry().emplace<sage::ui::Fill>(root.Entity());
    scene.SetParent(root.Entity(), iface.Entity());
    return iface.Entity();
}

} // namespace

TEST(Interface_elements_belong_to_their_own_interface) {
    Scene scene("S");
    const entt::entity hud = MakeInterface(scene, "HUD", {10.0f, 10.0f});
    const entt::entity menu = MakeInterface(scene, "Menu", {300.0f, 10.0f});

    const std::vector<entt::entity> hudRoots = sage::ui::InterfaceRoots(scene, hud);
    const std::vector<entt::entity> menuRoots = sage::ui::InterfaceRoots(scene, menu);
    CHECK_EQ((int)hudRoots.size(), 1);
    CHECK_EQ((int)menuRoots.size(), 1);
    // И это РАЗНЫЕ элементы: смешаться им негде.
    CHECK_TRUE(hudRoots[0] != menuRoots[0]);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, hudRoots[0]) == hud);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, menuRoots[0]) == menu);

    // Вложенный элемент принадлежит тому же интерфейсу, что и его корень:
    // граница — ближайший предок-интерфейс, а не ближайший предок вообще.
    GameObject child = scene.CreateEmptyObject("Child");
    scene.Registry().emplace<sage::ui::Element>(child.Entity());
    scene.SetParent(child.Entity(), hudRoots[0]);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, child.Entity()) == hud);
    // А корнем он не становится: корень — тот, над кем нет ЭЛЕМЕНТА.
    CHECK_EQ((int)sage::ui::InterfaceRoots(scene, hud).size(), 1);
}

TEST(Interface_scope_shows_one_interface_at_a_time) {
    // То, ради чего область и заведена: редактор вёрстки показывает ОДИН
    // интерфейс. Раньше показать один из двух было нечем — только спрятать
    // чужие объекты, то есть править сцену ради взгляда на неё.
    Scene scene("S");
    const entt::entity hud = MakeInterface(scene, "HUD", {10.0f, 10.0f});
    const entt::entity menu = MakeInterface(scene, "Menu", {300.0f, 10.0f});

    const auto all = sage::ui::SolveSceneRects(scene, 800, 600, /*includeHidden=*/true);
    CHECK_EQ((int)all.size(), 2);

    const auto onlyHud = sage::ui::SolveSceneRects(scene, 800, 600, true,
                                                   sage::ui::UIScope::Only(hud));
    CHECK_EQ((int)onlyHud.size(), 1);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, onlyHud[0].Entity) == hud);

    const auto onlyMenu = sage::ui::SolveSceneRects(scene, 800, 600, true,
                                                    sage::ui::UIScope::Only(menu));
    CHECK_EQ((int)onlyMenu.size(), 1);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, onlyMenu[0].Entity) == menu);

    // Щелчок по холсту тоже смотрит В ОБЛАСТЬ: элемент чужого экрана, который
    // сюда и не рисуется, выбираться не должен.
    const int inMenu = sage::ui::HitTest(scene, 320.0f, 30.0f, 800, 600);
    CHECK_TRUE(inMenu > 0);
    CHECK_EQ(sage::ui::HitTest(scene, 320.0f, 30.0f, 800, 600, sage::ui::UIScope::Only(hud)), -1);
    CHECK_EQ(sage::ui::HitTest(scene, 320.0f, 30.0f, 800, 600, sage::ui::UIScope::Only(menu)),
             inMenu);
}

TEST(Interface_hidden_interface_takes_all_its_elements_with_it) {
    // Спрятать экран целиком — одно поле, а не обход всех его элементов.
    Scene scene("S");
    const entt::entity hud = MakeInterface(scene, "HUD", {10.0f, 10.0f});
    MakeInterface(scene, "Menu", {300.0f, 10.0f});

    scene.Registry().get<sage::ui::InterfaceComponent>(hud).Visible = false;
    // В игре его нет...
    const auto shown = sage::ui::SolveSceneRects(scene, 800, 600, /*includeHidden=*/false);
    CHECK_EQ((int)shown.size(), 1);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, shown[0].Entity) != hud);
    // ...а в редакторе он виден: иначе выключенный интерфейс нельзя было бы ни
    // найти, ни включить обратно.
    CHECK_EQ((int)sage::ui::SolveSceneRects(scene, 800, 600, /*includeHidden=*/true).size(), 2);
}

TEST(Interface_order_between_interfaces_belongs_to_the_interface) {
    // HUD под меню паузы. Раньше это число жило в холсте КОРНЕВОГО ЭЛЕМЕНТА —
    // то есть у элемента, а не у экрана, и два корня одного интерфейса могли
    // спорить о том, каким он показывается.
    Scene scene("S");
    const entt::entity menu = MakeInterface(scene, "Menu", {0.0f, 0.0f}, /*sortOrder=*/10);
    const entt::entity hud = MakeInterface(scene, "HUD", {0.0f, 0.0f}, /*sortOrder=*/0);

    const std::vector<entt::entity> order = sage::ui::SortedInterfaces(scene);
    CHECK_EQ((int)order.size(), 2);
    CHECK_TRUE(order[0] == hud);   // рисуется раньше, значит лежит ниже
    CHECK_TRUE(order[1] == menu);

    // И в общем кадре элементы идут в том же порядке: меню поверх HUD.
    const auto rects = sage::ui::SolveSceneRects(scene, 800, 600, true);
    CHECK_EQ((int)rects.size(), 2);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, rects[0].Entity) == hud);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, rects[1].Entity) == menu);
}

TEST(Interface_resource_unfolds_into_an_interface_of_its_own) {
    // Ресурс .sageui, развёрнутый в сцену, приносит СВОЮ границу: две копии
    // одного окна инвентаря правятся по отдельности.
    Scene source("S");
    const Interface ui = sage::ui::Capture(source, BuildSample(source));

    Scene target("T");
    const std::vector<entt::entity> a = sage::ui::Instantiate(target, ui);
    const std::vector<entt::entity> b = sage::ui::Instantiate(target, ui);
    CHECK_EQ((int)a.size(), 1);
    CHECK_EQ((int)b.size(), 1);
    const entt::entity ia = sage::ui::InterfaceOf(target, a[0]);
    const entt::entity ib = sage::ui::InterfaceOf(target, b[0]);
    CHECK_TRUE(ia != entt::null);
    CHECK_TRUE(ib != entt::null);
    CHECK_TRUE(ia != ib);
    CHECK_EQ((int)sage::ui::SortedInterfaces(target).size(), 2);
}

TEST(Interface_migration_gives_every_old_root_its_own_interface) {
    // Сцены, собранные до появления границ: два корня россыпью. Каждый обязан
    // получить свой интерфейс — иначе меню и HUD остались бы одним экраном, то
    // есть ровно тем, на что и жаловались.
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 12,
      "objects": [
        {"id": 1, "name": "HUD",
         "ui": {"transform": {"size": {"x": 200, "y": 40}},
                "canvas": {"mode": 1, "sortOrder": 5,
                           "reference": {"x": 1280, "y": 720}}}},
        {"id": 2, "name": "Menu", "ui": {"transform": {"size": {"x": 100, "y": 100}}}},
        {"id": 3, "name": "Label", "parent": 1, "ui": {"transform": {}}}
      ]
    })";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(
        SceneSerializer::MigrateSceneJson(old));
    CHECK_TRUE(scene != nullptr);
    if (!scene) return;

    CHECK_EQ((int)sage::ui::SortedInterfaces(*scene).size(), 2);
    GameObject hud = FindByName(*scene, "HUD");
    GameObject menu = FindByName(*scene, "Menu");
    GameObject label = FindByName(*scene, "Label");
    CHECK_TRUE(hud.Valid() && menu.Valid() && label.Valid());
    if (!hud.Valid() || !menu.Valid() || !label.Valid()) return;

    const entt::entity hudIface = sage::ui::InterfaceOf(*scene, hud.Entity());
    const entt::entity menuIface = sage::ui::InterfaceOf(*scene, menu.Entity());
    CHECK_TRUE(hudIface != entt::null);
    CHECK_TRUE(menuIface != entt::null);
    CHECK_TRUE(hudIface != menuIface);
    // Ребёнок уехал вместе со своим корнем, а не остался в чужом экране.
    CHECK_TRUE(sage::ui::InterfaceOf(*scene, label.Entity()) == hudIface);

    // Холст корня переехал в интерфейс целиком: это его свойство.
    const sage::ui::InterfaceComponent& info =
        scene->Registry().get<sage::ui::InterfaceComponent>(hudIface);
    CHECK_EQ(info.SortOrder, 5);
    CHECK_TRUE(info.Canvas.Mode == sage::ui::Canvas::Scale::ScaleWithSize);
    CHECK_NEAR(info.Canvas.Reference.x, 1280.0f, 0.001f);
}

TEST(Interface_component_survives_a_scene_round_trip) {
    Scene scene("S");
    const entt::entity hud = MakeInterface(scene, "HUD", {10.0f, 10.0f}, /*sortOrder=*/7);
    sage::ui::InterfaceComponent& info = scene.Registry().get<sage::ui::InterfaceComponent>(hud);
    info.Visible = false;
    info.ReceivesInput = false;
    info.Canvas.Mode = sage::ui::Canvas::Scale::ScaleWithSize;
    info.Canvas.Reference = {1280.0f, 720.0f};

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    const std::vector<entt::entity> all = sage::ui::SortedInterfaces(*back);
    CHECK_EQ((int)all.size(), 1);
    if (all.empty()) return;
    const sage::ui::InterfaceComponent& loaded =
        back->Registry().get<sage::ui::InterfaceComponent>(all[0]);
    CHECK_FALSE(loaded.Visible);
    CHECK_FALSE(loaded.ReceivesInput);
    CHECK_EQ(loaded.SortOrder, 7);
    CHECK_NEAR(loaded.Canvas.Reference.y, 720.0f, 0.001f);
    // И элементы приехали внутрь своего интерфейса, а не в корень сцены.
    CHECK_EQ((int)sage::ui::InterfaceRoots(*back, all[0]).size(), 1);
}
