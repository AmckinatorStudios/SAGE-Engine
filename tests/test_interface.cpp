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
