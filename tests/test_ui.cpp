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
#include "sage/render/ShadowMap.h"

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
    u.Sprite = {11.0f, 59.0f, 26.0f, 28.0f};
    u.SliceBorder = {8.0f, 8.0f, 8.0f, 9.0f};
    u.PixelScale = 3.0f;
    u.PixelArt = true;
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
        CHECK_NEAR(r->Sprite.x, 11.0f, 1e-4);
        CHECK_NEAR(r->Sprite.w, 28.0f, 1e-4);
        CHECK_NEAR(r->SliceBorder.w, 9.0f, 1e-4);
        CHECK_NEAR(r->PixelScale, 3.0f, 1e-4);
        CHECK_TRUE(r->PixelArt);
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

// --- Растворение тени у предела дальности ---------------------------------
// Без него граница карты видна как ровная линия поперёк земли: у всех предметов
// ближе неё тени есть, дальше — ни у кого. Проверяем саму полосу: она обязана
// кончаться ровно на дальности и начинаться заметно раньше.
TEST(Shadow_fade_band_from_distance) {
    ShadowBinding b;
    // По умолчанию — «никогда»: карта без подгонки не должна гасить тени.
    CHECK_TRUE(b.FadeStart > 1e8f);

    b.SetFadeFromDistance(120.0f);
    CHECK_NEAR(b.FadeEnd, 120.0f, 1e-4);
    CHECK_TRUE(b.FadeStart < b.FadeEnd);       // полоса, а не точка
    CHECK_TRUE(b.FadeStart > 0.5f * b.FadeEnd); // но узкая: тени не съедаются заранее

    // Вырожденная дальность не должна давать отрицательную или нулевую полосу.
    b.SetFadeFromDistance(0.0f);
    CHECK_TRUE(b.FadeEnd >= 1.0f);
    CHECK_TRUE(b.FadeStart < b.FadeEnd);
}

// --- Интерактив: ввод текста, галка, ползунок, щелчок ----------------------
// UpdateSceneUI — чистая функция от состояния ввода, поэтому проверяется без
// окна и без GL. Ровно ради этого ввод и приходит структурой, а не опросом
// устройств внутри системы UI.
namespace {
UIElementComponent MakeInteractive(UIElementComponent::Kind kind, glm::vec2 pos, glm::vec2 size) {
    UIElementComponent e;
    e.Type = kind;
    e.Anchor = UIAnchor::TopLeft;
    e.Offset = pos;
    e.Size = size;
    e.Interactive = true;
    // Value по умолчанию 1 (это удобный дефолт для шкалы). Галке нужен явный
    // ноль, иначе тест проверял бы не то, что думает.
    if (kind == UIElementComponent::Kind::Checkbox) e.Value = 0.0f;
    return e;
}
sage::ui::UIInputState ClickAt(glm::vec2 p) {
    sage::ui::UIInputState in;
    in.Mouse = p;
    in.MouseDown = true;
    in.MousePressed = true;
    return in;
}
} // namespace

TEST(UI_input_field_typing_and_editing) {
    Scene scene("U");
    GameObject field = scene.CreateObject("Name");
    scene.Registry().emplace<UIElementComponent>(
        field.Entity(), MakeInteractive(UIElementComponent::Kind::Input, {10, 10}, {200, 40}));
    auto& e = scene.Registry().get<UIElementComponent>(field.Entity());

    // Пока не кликнули — текст не принимается: поле без фокуса не должно
    // воровать буквы у игры.
    sage::ui::UIInputState typing;
    typing.TypedText = "a";
    sage::ui::UpdateSceneUI(scene, typing, 800, 600);
    CHECK_EQ(e.Text, std::string(""));
    CHECK_FALSE(e.Focused);

    sage::ui::UpdateSceneUI(scene, ClickAt({50, 20}), 800, 600);
    CHECK_TRUE(e.Focused);

    // Кириллица: приходит символами UTF-8, курсор считается в байтах.
    sage::ui::UIInputState in;
    in.TypedText = "Пр";
    sage::ui::UIInputState res = in;
    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, res, 800, 600);
    CHECK_EQ(e.Text, std::string("Пр"));
    CHECK_TRUE(r.WantsKeyboard);
    CHECK_EQ(e.Caret, 4); // две кириллические буквы — четыре байта

    // Backspace удаляет ЦЕЛЫЙ символ, а не байт: иначе в поле остаётся битый UTF-8.
    sage::ui::UIInputState back;
    back.Backspace = true;
    sage::ui::UpdateSceneUI(scene, back, 800, 600);
    CHECK_EQ(e.Text, std::string("П"));
    CHECK_EQ(e.Caret, 2);

    // Предел длины считается в символах, а не в байтах.
    e.MaxLength = 2;
    sage::ui::UIInputState more;
    more.TypedText = "ивет";
    sage::ui::UpdateSceneUI(scene, more, 800, 600);
    CHECK_EQ(e.Text, std::string("П")); // не влезло целиком — не приняли вовсе

    // Enter снимает фокус.
    sage::ui::UIInputState enter;
    enter.Enter = true;
    sage::ui::UpdateSceneUI(scene, enter, 800, 600);
    CHECK_FALSE(e.Focused);
}

TEST(UI_checkbox_and_click_need_press_and_release) {
    Scene scene("U");
    GameObject box = scene.CreateObject("Chk");
    scene.Registry().emplace<UIElementComponent>(
        box.Entity(), MakeInteractive(UIElementComponent::Kind::Checkbox, {10, 10}, {30, 30}));
    auto& e = scene.Registry().get<UIElementComponent>(box.Entity());
    CHECK_NEAR(e.Value, 0.0f, 1e-4);

    // Одного нажатия мало — щелчок это нажать И отпустить НА элементе.
    sage::ui::UpdateSceneUI(scene, ClickAt({20, 20}), 800, 600);
    CHECK_TRUE(e.Pressed);
    CHECK_NEAR(e.Value, 0.0f, 1e-4);

    sage::ui::UIInputState up;
    up.Mouse = {20, 20};
    up.MouseReleased = true;
    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, up, 800, 600);
    CHECK_TRUE(e.Clicked);
    CHECK_NEAR(e.Value, 1.0f, 1e-4);
    CHECK_EQ(r.ClickedId, box.Id());

    // Флаг живёт ровно один кадр — иначе игра сработает на него дважды.
    sage::ui::UIInputState idle;
    idle.Mouse = {20, 20};
    sage::ui::UpdateSceneUI(scene, idle, 800, 600);
    CHECK_FALSE(e.Clicked);

    // Увести курсор с кнопки и отпустить — общепринятый способ передумать.
    sage::ui::UpdateSceneUI(scene, ClickAt({20, 20}), 800, 600);
    sage::ui::UIInputState away;
    away.Mouse = {400, 400};
    away.MouseReleased = true;
    sage::ui::UpdateSceneUI(scene, away, 800, 600);
    CHECK_FALSE(e.Clicked);
    CHECK_NEAR(e.Value, 1.0f, 1e-4); // значение не изменилось
}

TEST(UI_slider_drags_and_converts_to_game_units) {
    Scene scene("U");
    GameObject sld = scene.CreateObject("Vol");
    UIElementComponent s = MakeInteractive(UIElementComponent::Kind::Slider, {100, 10}, {200, 30});
    s.MinValue = 0.0f;
    s.MaxValue = 100.0f;
    scene.Registry().emplace<UIElementComponent>(sld.Entity(), s);
    auto& e = scene.Registry().get<UIElementComponent>(sld.Entity());

    // Нажали в середине дорожки.
    sage::ui::UpdateSceneUI(scene, ClickAt({200, 25}), 800, 600);
    CHECK_NEAR(e.Value, 0.5f, 1e-3);

    // Тянем ЗА пределы элемента, не отпуская: значение обязано доходить до края,
    // а не срываться от того, что курсор ушёл вбок.
    sage::ui::UIInputState drag;
    drag.Mouse = {1000, 400};
    drag.MouseDown = true;
    sage::ui::UpdateSceneUI(scene, drag, 800, 600);
    CHECK_NEAR(e.Value, 1.0f, 1e-3);

    drag.Mouse = {-50, 400};
    sage::ui::UpdateSceneUI(scene, drag, 800, 600);
    CHECK_NEAR(e.Value, 0.0f, 1e-3);
}

TEST(UI_disabled_element_ignores_mouse) {
    Scene scene("U");
    GameObject btn = scene.CreateObject("Quit");
    UIElementComponent b = MakeInteractive(UIElementComponent::Kind::Panel, {10, 10}, {100, 40});
    b.Enabled = false;
    scene.Registry().emplace<UIElementComponent>(btn.Entity(), b);
    auto& e = scene.Registry().get<UIElementComponent>(btn.Entity());

    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, ClickAt({50, 20}), 800, 600);
    CHECK_FALSE(e.Hovered);
    CHECK_FALSE(e.Pressed);
    CHECK_FALSE(r.WantsMouse);
}
