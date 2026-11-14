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
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scene/Prefab.h"
#include "sage/ui/InterfaceSource.h"
#include <filesystem>
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

namespace {

// Тот же пример, но внутри объекта-интерфейса: так он и лежит в сцене.
entt::entity SampleInterface(Scene& scene) {
    Interface blank;
    blank.Name = "Inventory";
    const entt::entity iface = sage::ui::Instantiate(scene, blank);
    const entt::entity main = sage::ui::MainContainer(scene, iface);
    for (entt::entity r : BuildSample(scene)) scene.SetParent(r, main);
    return iface;
}

entt::entity ParentOf(Scene& scene, const std::string& name) {
    GameObject o = FindByName(scene, name);
    return o.Valid() ? scene.ParentOf(o.Entity()) : entt::null;
}

} // namespace

TEST(Interface_survives_scene_to_file_to_scene) {
    Scene source("S");
    const Interface ui = sage::ui::Capture(source, SampleInterface(source));
    CHECK_TRUE(ui.Name == "Inventory");
    // Панель 400x220 главным контейнером не становится — она ложится В НЕГО.
    CHECK_EQ((int)ui.Root.Children.size(), 1);

    std::string err;
    Interface back;
    CHECK_TRUE(Interface::FromJsonString(ui.ToJsonString(), back, err));

    Scene target("T");
    const entt::entity iface = sage::ui::Instantiate(target, back);
    CHECK_TRUE(target.Registry().all_of<sage::ui::InterfaceComponent>(iface));
    const entt::entity main = sage::ui::MainContainer(target, iface);
    CHECK_TRUE(main != entt::null);
    CHECK_TRUE(ParentOf(target, "Panel") == main);

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
        CHECK_TRUE(ParentOf(target, "Caption") == panel.Entity());
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

TEST(Interface_canvas_travels_with_the_file) {
    // Опорное разрешение — часть вёрстки: экран верстали под него, и в другой
    // сцене он обязан открыться с тем же холстом.
    Interface ui;
    ui.Canvas.Mode = sage::ui::Canvas::Scale::ScaleWithSize;
    ui.Canvas.Reference = {1280.0f, 720.0f};
    std::string err;
    Interface back;
    CHECK_TRUE(Interface::FromJsonString(ui.ToJsonString(), back, err));
    CHECK_TRUE(back.Canvas.Mode == sage::ui::Canvas::Scale::ScaleWithSize);
    CHECK_NEAR(back.Canvas.Reference.x, 1280.0f, 0.001f);

    Scene scene("S");
    const entt::entity iface = sage::ui::Instantiate(scene, back);
    CHECK_NEAR(scene.Registry().get<sage::ui::InterfaceComponent>(iface).Canvas.Reference.y,
               720.0f, 0.001f);
}

TEST(Interface_can_be_instantiated_twice_without_the_copies_knowing) {
    // Два окна инвентаря рядом — это один ресурс, развёрнутый дважды. Правка
    // одной копии не имеет права задеть другую.
    Scene source("S");
    const Interface ui = sage::ui::Capture(source, SampleInterface(source));

    Scene target("T");
    const entt::entity a = sage::ui::Instantiate(target, ui);
    const entt::entity b = sage::ui::Instantiate(target, ui);
    CHECK_TRUE(a != b);
    const entt::entity ma = sage::ui::MainContainer(target, a);
    const entt::entity mb = sage::ui::MainContainer(target, b);
    CHECK_TRUE(ma != entt::null && mb != entt::null && ma != mb);

    const entt::entity pa = target.ChildrenOf(ma).front();
    const entt::entity pb = target.ChildrenOf(mb).front();
    target.Registry().get<sage::ui::Element>(pa).Position = {999.0f, 999.0f};
    CHECK_NEAR(target.Registry().get<sage::ui::Element>(pb).Position.x, 12.0f, 0.001f);
}

TEST(Interface_does_not_swallow_scene_objects) {
    // 3D-объект, случайно оказавшийся внутри поддерева интерфейса, в ресурс не
    // едет: иначе .sageui начал бы возить с собой меши и материалы, которых в
    // нём быть не должно.
    Scene scene("S");
    const entt::entity iface = SampleInterface(scene);
    GameObject box = scene.CreateObject("Ящик");   // обычный объект с мешем
    scene.SetParent(box.Entity(), FindByName(scene, "Panel").Entity());

    const Interface ui = sage::ui::Capture(scene, iface);
    CHECK_EQ((int)ui.Root.Children.size(), 1);
    // У панели было двое детей-элементов; ящик третьим не стал.
    CHECK_EQ((int)ui.Root.Children[0].Children.size(), 2);
}

TEST(Interface_old_file_format_is_refused) {
    // Файл первой версии (несколько корней) не читается: интерфейс того вида
    // создают заново. Отказ — с причиной, а не пустой экран.
    const char* v1 = R"({"sage_interface_version":1,"name":"Old","roots":[]})";
    Interface ui;
    std::string err;
    CHECK_FALSE(Interface::FromJsonString(v1, ui, err));
    CHECK_FALSE(err.empty());
}

TEST(Interface_only_containers_hold_children) {
    // Детей держат Container и Panel; кнопка или надпись — нет.
    Scene scene("S");
    const entt::entity iface = sage::ui::Instantiate(scene, Interface{});
    const entt::entity main = sage::ui::MainContainer(scene, iface);
    auto make = [&](const char* type) {
        GameObject o = scene.CreateEmptyObject(type);
        sage::ui::ApplyPreset(scene, o.Entity(), type);
        scene.SetParent(o.Entity(), main);
        return o.Entity();
    };
    const entt::entity panel = make("Panel");
    const entt::entity button = make("Button");
    const entt::entity text = make("Text");
    CHECK_TRUE(sage::ui::CanReparent(scene, text, panel));
    CHECK_FALSE(sage::ui::CanReparent(scene, text, button));
    CHECK_TRUE(sage::ui::HoldsChildren(scene.Registry(), main));
}

TEST(Interface_main_container_stays_the_root) {
    // Главный контейнер — область всего интерфейса: внутрь другого элемента он
    // не переносится, а «в корень» значит в него, а не под объект-интерфейс.
    Scene scene("S");
    const entt::entity iface = sage::ui::Instantiate(scene, Interface{});
    const entt::entity main = sage::ui::MainContainer(scene, iface);
    CHECK_TRUE(sage::ui::IsMainContainer(scene, main));

    GameObject panel = scene.CreateEmptyObject("Panel");
    scene.Registry().emplace<sage::ui::Element>(panel.Entity());
    scene.SetParent(panel.Entity(), main);
    GameObject inner = scene.CreateEmptyObject("Inner");
    scene.Registry().emplace<sage::ui::Element>(inner.Entity());
    scene.SetParent(inner.Entity(), panel.Entity());

    CHECK_FALSE(sage::ui::CanReparent(scene, main, panel.Entity()));
    CHECK_FALSE(sage::ui::ReparentElement(scene, main, entt::null));
    CHECK_TRUE(sage::ui::ReparentElement(scene, inner.Entity(), entt::null));
    CHECK_TRUE(scene.ParentOf(inner.Entity()) == main);
    // И главный остался единственным: корней у интерфейса не прибавилось.
    CHECK_TRUE(sage::ui::MainContainer(scene, iface) == main);
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

    // Container — невидимый держатель детей: только раскладка, ни одной
    // видимой части. Panel — то же с подложкой.
    const sage::ui::Preset* container = sage::ui::FindPreset("Container");
    CHECK_TRUE(container != nullptr);
    if (container) {
        CHECK_TRUE(container->Container);
        CHECK_TRUE(container->HasArrangement);
        CHECK_FALSE(container->HasFill);
        CHECK_FALSE(container->HasLabel);
        CHECK_TRUE(container->Children.empty());
    }
    const sage::ui::Preset* panel = sage::ui::FindPreset("Panel");
    CHECK_TRUE(panel != nullptr);
    if (panel) {
        CHECK_TRUE(panel->Container);
        CHECK_TRUE(panel->HasFill);
        CHECK_TRUE(panel->HasArrangement);
    }

    // Запрошенный набор Add/Create на месте.
    for (const char* name : {"Container", "Panel", "Text", "Image", "Button", "Input Field"}) {
        if (!sage::ui::FindPreset(name))
            sagetest::ReportFail(__FILE__, __LINE__, std::string("нет заготовки: ") + name);
    }
}

TEST(Interface_types_are_only_the_current_ones) {
    // Ряд, столбец, сетка, прокрутка, обрезка — режимы контейнера, а не типы;
    // прежних имён нет вовсе.
    for (const char* gone : {"Group", "Row", "Column", "Grid", "Wrap Row", "Scroll View", "Clip Area",
                             "Spacer", "Label", "Input", "Screen"})
        if (sage::ui::FindPreset(gone))
            sagetest::ReportFail(__FILE__, __LINE__, std::string("лишний тип: ") + gone);
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
    const sage::ui::InterfaceFragment copied = sage::ui::CaptureFragment(scene, roots);

    // Удаляем оригинал целиком.
    for (auto e : roots) {
        const IdComponent* id = scene.Registry().try_get<IdComponent>(e);
        if (id) scene.RemoveObject(id->Id);
    }
    CHECK_FALSE(FindByName(scene, "Panel").Valid());

    // И вставляем из описания.
    const std::vector<entt::entity> pasted = sage::ui::InstantiateFragment(scene, copied);
    CHECK_EQ((int)pasted.size(), 1);
    CHECK_TRUE(FindByName(scene, "Panel").Valid());
    CHECK_TRUE(FindByName(scene, "Caption").Valid());
}

TEST(Interface_pasting_into_an_element_makes_it_the_parent) {
    // «Скопировал кнопку, выбрал панель, вставил — кнопка внутри панели»: это
    // то, чего ждут, и проверка сторожит именно связь родителя.
    Scene scene("S");
    const std::vector<entt::entity> roots = BuildSample(scene);
    const sage::ui::InterfaceFragment copied = sage::ui::CaptureFragment(scene, roots);

    GameObject host = scene.CreateEmptyObject("Host");
    scene.Registry().emplace<sage::ui::Element>(host.Entity());

    const std::vector<entt::entity> pasted = sage::ui::InstantiateFragment(scene, copied, host.Entity());
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

TEST(Interface_old_scene_loses_its_inline_elements) {
    // Элементы прямо в сцене — старый вид. Интерфейс живёт в файле .sageui,
    // поэтому старые элементы при открытии сцены уходят целиком, вместе с
    // детьми; объект-интерфейс остаётся — ему назначают файл.
    const std::string old = R"({
      "name": "Old", "sage_scene_version": 18,
      "objects": [
        {"id": 1, "name": "Menu", "interface": {"visible": true}},
        {"id": 2, "name": "Panel", "parent": 1, "ui": {"element": {}}},
        {"id": 3, "name": "Caption", "parent": 2, "ui": {"element": {}}},
        {"id": 4, "name": "Box"}
      ]
    })";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(SceneSerializer::MigrateSceneJson(old));
    CHECK_TRUE(scene != nullptr);
    if (!scene) return;
    CHECK_FALSE(FindByName(*scene, "Panel").Valid());
    CHECK_FALSE(FindByName(*scene, "Caption").Valid());
    CHECK_TRUE(FindByName(*scene, "Box").Valid());
    CHECK_EQ((int)sage::ui::SortedInterfaces(*scene).size(), 1);
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

// ============================================================================
//  ИНТЕРФЕЙС ИЗ ФАЙЛА: в сцене — один объект со ссылкой на .sageui
// ============================================================================

namespace {

// Файл меню во временной папке: главный контейнер с кнопкой «Play».
std::string WriteMenuFile(const char* fileName, const char* buttonName) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_interface_source_test";
    fs::create_directories(dir);
    const std::string path = (dir / fileName).string();
    Scene doc("Doc");
    const entt::entity iface = sage::ui::Instantiate(doc, Interface{});
    GameObject play = doc.CreateEmptyObject(buttonName);
    doc.Registry().emplace<sage::ui::Element>(play.Entity());
    doc.SetParent(play.Entity(), sage::ui::MainContainer(doc, iface));
    Interface ui = sage::ui::Capture(doc, iface);
    ui.Name = "MainMenu";
    std::string err;
    CHECK_TRUE(ui.SaveFile(path, err));
    return path;
}

entt::entity SourcedInterface(Scene& scene, const std::string& path) {
    GameObject obj = scene.CreateEmptyObject("MainMenu");
    sage::ui::InterfaceComponent ic;
    ic.Source = path;
    scene.Registry().emplace<sage::ui::InterfaceComponent>(obj.Entity(), ic);
    return obj.Entity();
}

} // namespace

TEST(Interface_from_file_unfolds_under_one_scene_object) {
    sage::ui::ClearInterfaceSourceCache();
    const std::string path = WriteMenuFile("menu_a.sageui", "Play");
    Scene scene("S");
    const entt::entity iface = SourcedInterface(scene, path);
    sage::ui::SyncInterfaceSources(scene);

    // Кнопка из файла есть в сцене — её рисует и нажимает та же система.
    GameObject play = FindByName(scene, "Play");
    CHECK_TRUE(play.Valid());
    if (!play.Valid()) return;
    CHECK_TRUE(sage::ui::InterfaceOf(scene, play.Entity()) == iface);
    CHECK_TRUE(sage::ui::IsInstancePart(scene, play.Entity()));
    // Под объектом — ровно главный контейнер, без россыпи корней.
    CHECK_TRUE(sage::ui::MainContainer(scene, iface) != entt::null);

    // Повторная синхронизация ничего не разворачивает второй раз.
    sage::ui::SyncInterfaceSources(scene);
    CHECK_EQ((int)scene.ChildrenOf(iface).size(), 1);
}

TEST(Interface_from_file_is_not_written_into_the_scene) {
    // Описание экрана одно — в его файле. В сцену пишется только ссылка, а
    // развёрнутая копия появляется заново при чтении.
    sage::ui::ClearInterfaceSourceCache();
    const std::string path = WriteMenuFile("menu_b.sageui", "Play");
    Scene scene("S");
    SourcedInterface(scene, path);
    sage::ui::SyncInterfaceSources(scene);

    const std::string saved = SceneSerializer::SaveToString(scene);
    CHECK_TRUE(saved.find("\"Play\"") == std::string::npos);
    CHECK_TRUE(saved.find("menu_b.sageui") != std::string::npos);

    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(saved);
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    // Сразу после чтения, без кадра отрисовки: скрипт, стартующий со сценой,
    // ищет кнопки по имени.
    CHECK_TRUE(FindByName(*back, "Play").Valid());
    const std::vector<entt::entity> all = sage::ui::SortedInterfaces(*back);
    CHECK_EQ((int)all.size(), 1);
    if (!all.empty())
        CHECK_TRUE(back->Registry().get<sage::ui::InterfaceComponent>(all[0]).Source == path);
}

TEST(Interface_from_file_follows_the_file) {
    // Сохранили интерфейс в его пространстве — каждая сцена, где он стоит,
    // показывает новый вариант сама, без пересборки сцены.
    sage::ui::ClearInterfaceSourceCache();
    const std::string path = WriteMenuFile("menu_c.sageui", "Play");
    Scene scene("S");
    SourcedInterface(scene, path);
    sage::ui::SyncInterfaceSources(scene);
    CHECK_TRUE(FindByName(scene, "Play").Valid());

    WriteMenuFile("menu_c.sageui", "Continue");
    sage::ui::ClearInterfaceSourceCache();   // не ждать полсекунды проверки диска
    sage::ui::SyncInterfaceSources(scene);
    CHECK_FALSE(FindByName(scene, "Play").Valid());
    CHECK_TRUE(FindByName(scene, "Continue").Valid());
}

TEST(Interface_copy_of_the_object_unfolds_its_own_copy) {
    // Дубликат объекта-интерфейса не должен ни унести чужую копию, ни показать
    // кнопки дважды: у каждого объекта — своя развёрнутая копия файла.
    sage::ui::ClearInterfaceSourceCache();
    const std::string path = WriteMenuFile("menu_d.sageui", "Play");
    Scene scene("S");
    const entt::entity a = SourcedInterface(scene, path);
    sage::ui::SyncInterfaceSources(scene);
    const GameObject copy = sage::scene::CopySubtree(scene, a, scene, entt::null);
    sage::ui::SyncInterfaceSources(scene);

    int plays = 0;
    for (auto e : scene.Registry().view<NameComponent>())
        if (scene.Registry().get<NameComponent>(e).Name == "Play") ++plays;
    CHECK_EQ(plays, 2);
    CHECK_EQ((int)scene.ChildrenOf(a).size(), 1);
    CHECK_EQ((int)scene.ChildrenOf(copy.Entity()).size(), 1);
}

TEST(Interface_cleared_source_removes_the_copy) {
    sage::ui::ClearInterfaceSourceCache();
    const std::string path = WriteMenuFile("menu_e.sageui", "Play");
    Scene scene("S");
    const entt::entity iface = SourcedInterface(scene, path);
    sage::ui::SyncInterfaceSources(scene);
    scene.Registry().get<sage::ui::InterfaceComponent>(iface).Source.clear();
    sage::ui::SyncInterfaceSources(scene);
    CHECK_FALSE(FindByName(scene, "Play").Valid());
}
