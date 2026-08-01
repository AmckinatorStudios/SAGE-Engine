// Тесты UI-системы: математика якорей (ResolveAnchored), вёрстка элементов
// внутри родителя (ResolveElementRect), сериализация UIElementComponent,
// HitTest по слоям/маскам/видимости. Всё на CPU, БЕЗ GL (рендер не трогаем).
#include "TestFramework.h"

#include "sage/ui/UIAnchor.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UIIcons.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

using sage::ui::UIRect;

TEST(UI_anchor_corners_and_center) {
    UIRect screen{0, 0, 1000, 500};
    glm::vec2 size{100, 50};

    glm::vec2 tl = sage::ui::ResolveAnchored(UIAnchor::TopLeft, {16, 16}, size, screen);
    CHECK_NEAR(tl.x, 16.0f, 1e-4); CHECK_NEAR(tl.y, 16.0f, 1e-4);

    // Правые якоря: Offset.x идёт ВЛЕВО от правого края.
    glm::vec2 tr = sage::ui::ResolveAnchored(UIAnchor::TopRight, {16, 16}, size, screen);
    CHECK_NEAR(tr.x, 1000.0f - 100.0f - 16.0f, 1e-4); CHECK_NEAR(tr.y, 16.0f, 1e-4);

    glm::vec2 br = sage::ui::ResolveAnchored(UIAnchor::BottomRight, {16, 16}, size, screen);
    CHECK_NEAR(br.x, 884.0f, 1e-4); CHECK_NEAR(br.y, 500.0f - 50.0f - 16.0f, 1e-4);

    glm::vec2 c = sage::ui::ResolveAnchored(UIAnchor::Center, {0, 0}, size, screen);
    CHECK_NEAR(c.x, 450.0f, 1e-4); CHECK_NEAR(c.y, 225.0f, 1e-4);

    glm::vec2 bc = sage::ui::ResolveAnchored(UIAnchor::BottomCenter, {0, 10}, size, screen);
    CHECK_NEAR(bc.x, 450.0f, 1e-4); CHECK_NEAR(bc.y, 440.0f, 1e-4);
}

TEST(UI_element_rect_nested_in_parent) {
    // Ребёнок якорится к прямоугольнику РОДИТЕЛЯ, а не к экрану.
    UIElementComponent child;
    child.Anchor = UIAnchor::BottomRight;
    child.Offset = {8, 8};
    child.Size = {40, 20};
    UIRect parent{100, 100, 200, 100};
    UIRect r = sage::ui::ResolveElementRect(child, parent);
    CHECK_NEAR(r.x, 100.0f + 200.0f - 40.0f - 8.0f, 1e-4);
    CHECK_NEAR(r.y, 100.0f + 100.0f - 20.0f - 8.0f, 1e-4);
    CHECK_NEAR(r.w, 40.0f, 1e-4);
    CHECK_NEAR(r.h, 20.0f, 1e-4);
}

TEST(UI_layout_size_overrides_declared_size) {
    // AutoWidth-элемент верстается по ИЗМЕРЕННОЙ ширине (её кладёт отрисовка в
    // LayoutSize), иначе якорь считался бы от запасного Size и «таблетка»
    // прыгала бы на кадр раньше, чем в неё поместился текст.
    UIElementComponent e;
    e.Anchor = UIAnchor::TopRight;
    e.Offset = {10, 10};
    e.Size = {200, 30};
    e.AutoWidth = true;
    UIRect screen{0, 0, 800, 600};

    UIRect before = sage::ui::ResolveElementRect(e, screen);
    CHECK_NEAR(before.w, 200.0f, 1e-4); // ещё не рисовался — запасной размер

    e.LayoutSize = {124.0f, 30.0f};
    UIRect after = sage::ui::ResolveElementRect(e, screen);
    CHECK_NEAR(after.w, 124.0f, 1e-4);
    CHECK_NEAR(after.x, 800.0f - 124.0f - 10.0f, 1e-4);
}

TEST(UI_component_serialization_roundtrip) {
    Scene scene("U");
    GameObject panel = scene.CreateObject("Hud");
    UIElementComponent u;
    u.Type = UIElementComponent::Kind::Bar;
    u.Anchor = UIAnchor::BottomLeft;
    u.Offset = {24, 18};
    u.Size = {220, 26};
    u.Layer = 3;
    u.ClipChildren = true;
    u.Color = {0.1f, 0.2f, 0.3f, 0.8f};
    u.Rounding = 13.0f;
    u.BorderThickness = 2.5f;
    u.BorderColor = {0.9f, 0.8f, 0.1f, 1.0f};
    u.Text = "HP";
    u.TextScale = 1.75f;
    u.TextCentered = false;
    u.Value = 0.42f;
    u.BarFillColor = {0.8f, 0.2f, 0.2f, 1.0f};
    u.PadX = 11.5f;
    u.AutoWidth = true;
    scene.Registry().emplace<UIElementComponent>(panel.Entity(), u);

    std::string json = SceneSerializer::SaveToString(scene);
    auto loaded = SceneSerializer::LoadFromString(json);
    GameObject back = loaded->FindByName("Hud");
    CHECK_TRUE(back.Valid());
    const UIElementComponent* r = loaded->Registry().try_get<UIElementComponent>(back.Entity());
    CHECK_TRUE(r != nullptr);
    if (r) {
        CHECK_TRUE(r->Type == UIElementComponent::Kind::Bar);
        CHECK_TRUE(r->Anchor == UIAnchor::BottomLeft);
        CHECK_NEAR(r->Offset.x, 24.0f, 1e-4);
        CHECK_NEAR(r->Size.y, 26.0f, 1e-4);
        CHECK_EQ(r->Layer, 3);
        CHECK_TRUE(r->ClipChildren);
        CHECK_NEAR(r->Color.a, 0.8f, 1e-4);
        CHECK_NEAR(r->Rounding, 13.0f, 1e-4);
        CHECK_NEAR(r->BorderThickness, 2.5f, 1e-4);
        CHECK_EQ(r->Text, std::string("HP"));
        CHECK_NEAR(r->TextScale, 1.75f, 1e-4);
        CHECK_FALSE(r->TextCentered);
        CHECK_NEAR(r->Value, 0.42f, 1e-4);
        CHECK_NEAR(r->BarFillColor.r, 0.8f, 1e-4);
        CHECK_NEAR(r->PadX, 11.5f, 1e-4);
        CHECK_TRUE(r->AutoWidth);
    }
}

TEST(UI_hit_test_layers_and_visibility) {
    Scene scene("U");
    // Две перекрывающиеся панели: Layer решает, кто сверху.
    GameObject below = scene.CreateObject("Below");
    UIElementComponent b;
    b.Anchor = UIAnchor::TopLeft; b.Offset = {0, 0}; b.Size = {100, 100}; b.Layer = 0;
    scene.Registry().emplace<UIElementComponent>(below.Entity(), b);

    GameObject above = scene.CreateObject("Above");
    UIElementComponent a;
    a.Anchor = UIAnchor::TopLeft; a.Offset = {50, 50}; a.Size = {100, 100}; a.Layer = 5;
    scene.Registry().emplace<UIElementComponent>(above.Entity(), a);

    // Точка в пересечении — выигрывает верхний слой.
    CHECK_EQ(sage::ui::HitTest(scene, 75, 75, 800, 600), above.Id());
    // Точка только в нижней панели.
    CHECK_EQ(sage::ui::HitTest(scene, 10, 10, 800, 600), below.Id());
    // Пустое место.
    CHECK_EQ(sage::ui::HitTest(scene, 400, 400, 800, 600), -1);

    // Невидимый элемент не ловит точки.
    scene.Registry().get<UIElementComponent>(above.Entity()).Visible = false;
    CHECK_EQ(sage::ui::HitTest(scene, 75, 75, 800, 600), below.Id());
}

TEST(UI_hit_test_child_and_clip_mask) {
    Scene scene("U");
    GameObject parent = scene.CreateObject("Panel");
    UIElementComponent p;
    p.Anchor = UIAnchor::TopLeft; p.Offset = {100, 100}; p.Size = {200, 100};
    p.ClipChildren = true; // маска
    scene.Registry().emplace<UIElementComponent>(parent.Entity(), p);

    GameObject child = scene.CreateObject("Button");
    UIElementComponent c;
    // Ребёнок наполовину ВЫСОВЫВАЕТСЯ за родителя вправо: якорь TopLeft
    // родителя + offset за его край.
    c.Anchor = UIAnchor::TopLeft; c.Offset = {150, 20}; c.Size = {100, 40};
    scene.Registry().emplace<UIElementComponent>(child.Entity(), c);
    scene.SetParent(child.Entity(), parent.Entity());

    // Точка внутри родителя И ребёнка — ребёнок (нарисован поверх).
    CHECK_EQ(sage::ui::HitTest(scene, 260, 130, 800, 600), child.Id());
    // Точка в высунувшейся части ребёнка (вне родителя): маска ClipChildren
    // обрезает — попадания нет.
    CHECK_EQ(sage::ui::HitTest(scene, 320, 130, 800, 600), -1);
    // Без маски та же точка попадает в ребёнка.
    scene.Registry().get<UIElementComponent>(parent.Entity()).ClipChildren = false;
    CHECK_EQ(sage::ui::HitTest(scene, 320, 130, 800, 600), child.Id());
}

// --- Боевой сложный интерфейс (инвентарь + дерево навыков): плотная вёрстка,
//     вложенные контейнеры, слои, клип-маска — стресс-тест тулкита без GL. ---
TEST(UI_showcase_builds_dense_layout) {
    Scene scene("UI");
    int rootId = sage::ui::BuildShowcase(scene);
    CHECK_TRUE(rootId != -1);
    // Плотный экран: десятки элементов (панели/иконки/счётчики/узлы/провода).
    CHECK_TRUE(scene.Count() > 50);
    CHECK_TRUE(scene.FindByName("Inventory").Valid());
    CHECK_TRUE(scene.FindByName("SkillTree").Valid());
    CHECK_TRUE(scene.FindByName("Slot0").Valid());
    CHECK_TRUE(scene.FindByName("Skill_Ultima").Valid());
}

TEST(UI_showcase_hittest_respects_clip_mask) {
    Scene scene("UI");
    sage::ui::BuildShowcase(scene);
    const int W = 1920, H = 1080;

    // Инвентарь якорится TopRight {24,24} размером {372,470}: слева-верх на
    // экране = (1920-24-372, 24) = (1524, 24). Сетка внутри {16,58} h=250 —
    // маска по y в [82, 332). Слот0 (12,12) виден, слот20 (12,268) — обрезан.
    int visible = sage::ui::HitTest(scene, 1524 + 16 + 40, 24 + 58 + 40, W, H); // центр слота0
    CHECK_TRUE(visible >= 0); // попали во что-то (слот/иконку) внутри маски

    int clipped = sage::ui::HitTest(scene, 1524 + 16 + 40, 24 + 58 + 290, W, H); // где был бы слот20
    int slot20 = scene.FindByName("Slot20").Id();
    CHECK_TRUE(clipped != slot20); // обрезанный слот не кликается

    // Точка далеко от обеих панелей (низ-центр экрана) — мимо всего.
    CHECK_EQ(sage::ui::HitTest(scene, W / 2, H - 40, W, H), -1);
}

// ===========================================================================
//  Иконки интерфейса
//
//  Иконки рисуются кодом, а не грузятся картинками (см. ui/UIIcons.h), поэтому
//  «есть ли иконка» — вопрос к реестру, а не к файловой системе, и его можно
//  задавать без GL-контекста.
// ===========================================================================
TEST(UI_icon_registry_knows_its_names) {
    const auto& names = sage::ui::IconNames();
    CHECK_TRUE(names.size() >= 20); // набор для игрового HUD, а не три штуки

    // Имена отсортированы — редактор показывает их списком, и порядок не должен
    // прыгать от запуска к запуску.
    for (size_t i = 1; i < names.size(); ++i) CHECK_TRUE(names[i - 1] < names[i]);

    // Опорные иконки, на которые опирается интерфейс игр.
    for (const char* n : {"heart", "drop", "flame", "plank", "fish", "lantern"}) {
        CHECK_TRUE(sage::ui::HasIcon(n));
    }
    CHECK_FALSE(sage::ui::HasIcon("нет-такой-иконки"));
}
