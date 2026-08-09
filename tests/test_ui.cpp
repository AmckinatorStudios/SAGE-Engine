// Тесты UI-системы: математика якорей (ResolveAnchored), вёрстка элементов
// внутри родителя (ResolveElementRect), сериализация UIElementComponent,
// HitTest по слоям/маскам/видимости. Всё на CPU, БЕЗ GL (рендер не трогаем).
#include "TestFramework.h"

#include "sage/ui/UIAnchor.h"
#include "sage/render/SkyRenderer.h"
#include "sage/scene/Light.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UI.h"
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

TEST(ui_offset_for_top_left_inverts_anchor) {
    // Обратная задача к ResolveAnchored: по нужному положению получить Offset.
    // На ней держится вёрстка мышью — редактор тянет прямоугольник, а хранить
    // обязан якорь с отступом.
    //
    // Проверяем ВСЕ ДЕВЯТЬ якорей, потому что ошибиться можно ровно в них: у
    // правых и нижних Offset растёт в обратную сторону, у центральных зависит
    // ещё и от размера. Ошибка в знаке даёт элемент, который при перетаскивании
    // убегает от курсора — и заметна она только на тех якорях, где знак другой.
    const sage::ui::UIRect parent{40.0f, 30.0f, 800.0f, 600.0f};
    const glm::vec2 size(120.0f, 48.0f);

    const UIAnchor all[9] = {UIAnchor::TopLeft,    UIAnchor::TopCenter,    UIAnchor::TopRight,
                             UIAnchor::CenterLeft, UIAnchor::Center,       UIAnchor::CenterRight,
                             UIAnchor::BottomLeft, UIAnchor::BottomCenter, UIAnchor::BottomRight};

    const glm::vec2 targets[4] = {
        {100.0f, 90.0f}, {700.0f, 500.0f}, {40.0f, 30.0f}, {-25.0f, 610.0f}};

    for (UIAnchor a : all) {
        for (const glm::vec2& want : targets) {
            const glm::vec2 off = sage::ui::OffsetForTopLeft(a, want, size, parent);
            const glm::vec2 got = sage::ui::ResolveAnchored(a, off, size, parent);
            CHECK_NEAR(got.x, want.x, 1e-3f);
            CHECK_NEAR(got.y, want.y, 1e-3f);
        }
    }
}

TEST(ui_offset_for_top_left_respects_anchor_direction) {
    // Отдельно — САМО СВОЙСТВО, ради которого функция и нужна: у правого якоря
    // сдвиг элемента ВПРАВО обязан УМЕНЬШАТЬ Offset. Проверка выше прошла бы и
    // на функции, которая просто возвращает разность координат для всех якорей
    // одинаково, — а именно такая ошибка и ломает перетаскивание.
    const sage::ui::UIRect parent{0.0f, 0.0f, 1000.0f, 800.0f};
    const glm::vec2 size(100.0f, 40.0f);

    const glm::vec2 leftA = sage::ui::OffsetForTopLeft(UIAnchor::TopLeft, {200, 100}, size, parent);
    const glm::vec2 leftB = sage::ui::OffsetForTopLeft(UIAnchor::TopLeft, {260, 100}, size, parent);
    CHECK_TRUE(leftB.x > leftA.x);   // левый якорь: правее — больше отступ

    const glm::vec2 rightA =
        sage::ui::OffsetForTopLeft(UIAnchor::TopRight, {200, 100}, size, parent);
    const glm::vec2 rightB =
        sage::ui::OffsetForTopLeft(UIAnchor::TopRight, {260, 100}, size, parent);
    CHECK_TRUE(rightB.x < rightA.x); // правый якорь: правее — МЕНЬШЕ отступ

    const glm::vec2 botA =
        sage::ui::OffsetForTopLeft(UIAnchor::BottomLeft, {200, 100}, size, parent);
    const glm::vec2 botB =
        sage::ui::OffsetForTopLeft(UIAnchor::BottomLeft, {200, 160}, size, parent);
    CHECK_TRUE(botB.y < botA.y);     // нижний якорь: ниже — меньше отступ
}

TEST(ui_offset_survives_resize_at_far_anchors) {
    // Изменение размера у правого/нижнего якоря: если тянуть за ЛЕВУЮ грань,
    // правая обязана остаться на месте. Это и есть то, что человек видит как
    // «ручка тянет не тот край».
    const sage::ui::UIRect parent{0.0f, 0.0f, 1000.0f, 800.0f};
    const glm::vec2 sizeBefore(100.0f, 40.0f);
    const glm::vec2 topLeftBefore(300.0f, 200.0f);

    // Тянем левую грань влево на 30: левый край уехал, правый остался.
    const glm::vec2 topLeftAfter(270.0f, 200.0f);
    const glm::vec2 sizeAfter(130.0f, 40.0f);

    for (UIAnchor a : {UIAnchor::TopRight, UIAnchor::BottomRight, UIAnchor::Center}) {
        const glm::vec2 off = sage::ui::OffsetForTopLeft(a, topLeftAfter, sizeAfter, parent);
        const glm::vec2 got = sage::ui::ResolveAnchored(a, off, sizeAfter, parent);
        CHECK_NEAR(got.x, topLeftAfter.x, 1e-3f);
        CHECK_NEAR(got.x + sizeAfter.x, topLeftBefore.x + sizeBefore.x, 1e-3f);
    }
}

TEST(sky_celestials_sun_points_where_light_comes_from) {
    // У направленного света хранится, КУДА он светит; солнце на небе надо
    // нарисовать там, ОТКУДА. Знак здесь перепутать проще всего, и ошибка даёт
    // солнце ровно в противоположной точке неба — то есть тени идут в одну
    // сторону, а светило висит в другой.
    LightingEnvironment env;
    env.Skybox.Celestials = true;

    // Свет падает вниз => солнце в зените.
    env.Sun.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
    SkyCelestials c = CelestialsFromEnvironment(env);
    CHECK_TRUE(c.Enabled);
    CHECK_NEAR(c.SunDir.y, 1.0f, 1e-4f);

    // Свет идёт с запада на восток => солнце на западе.
    env.Sun.Direction = glm::normalize(glm::vec3(1.0f, -0.2f, 0.0f));
    c = CelestialsFromEnvironment(env);
    CHECK_TRUE(c.SunDir.x < 0.0f);
    CHECK_TRUE(c.SunDir.y > 0.0f);
    CHECK_NEAR(glm::length(c.SunDir), 1.0f, 1e-4f);

    // Вырожденное направление не должно давать NaN: небо обязано рисоваться
    // даже у сцены, где солнце забыли настроить.
    env.Sun.Direction = glm::vec3(0.0f);
    c = CelestialsFromEnvironment(env);
    CHECK_NEAR(glm::length(c.SunDir), 1.0f, 1e-4f);

    // Выключенные светила остаются выключенными — старые сцены не должны вдруг
    // обзавестись солнцем там, где автор его не ставил.
    env.Skybox.Celestials = false;
    CHECK_FALSE(CelestialsFromEnvironment(env).Enabled);
}


TEST(UI_presets_are_the_same_everywhere) {
    // «Кнопка» — это не вид элемента, а набор значений: панель с Interactive,
    // надписью и рамкой. Знание об этом жило в функции РЕДАКТОРА, то есть
    // кнопку можно было получить только мышью: скрипт, собирающий интерфейс на
    // лету, повторял те же присваивания у себя, а игра без редактора не имела
    // к ним доступа вовсе. Теперь таблица одна на движок — и проверяется здесь,
    // а не «на глаз в редакторе».
    UIElementComponent button;
    CHECK_TRUE(sage::ui::ApplyPreset(button, "Button"));
    CHECK_TRUE(button.Interactive);           // иначе это просто прямоугольник
    CHECK_TRUE(!button.Text.empty());         // кнопка без надписи не читается
    CHECK_TRUE(button.Size.x > 0.0f && button.Size.y > 0.0f);

    // Полоса заполнена наполовину: пустая неотличима от панели, и человек
    // решает, что элемент не создался.
    UIElementComponent bar;
    CHECK_TRUE(sage::ui::ApplyPreset(bar, "Bar"));
    CHECK_TRUE(bar.Type == UIElementComponent::Kind::Bar);
    CHECK_TRUE(bar.Value > 0.0f && bar.Value < 1.0f);

    // Поле ввода: текст влево (по центру набирать непривычно) и подсказка.
    UIElementComponent input;
    CHECK_TRUE(sage::ui::ApplyPreset(input, "Input"));
    CHECK_FALSE(input.TextCentered);
    CHECK_TRUE(!input.Placeholder.empty());

    // Неизвестное имя — честный отказ, а не молча пустой элемент.
    UIElementComponent unknown;
    CHECK_FALSE(sage::ui::ApplyPreset(unknown, "Соврёшь"));

    // Каждая заготовка из списка применяется и даёт ВИДИМЫЙ элемент: нулевой
    // размер означал бы «создал и не увидел ничего».
    for (const std::string& name : sage::ui::PresetNames()) {
        UIElementComponent e;
        CHECK_TRUE(sage::ui::ApplyPreset(e, name));
        CHECK_TRUE(e.Size.x > 0.0f && e.Size.y > 0.0f);
    }
}

// ===========================================================================
//  Новая система интерфейса: раскладка, растяжение, маски, холст
//  (sage/ui/UI.h — компоненты вместо одного компонента на всё)
// ===========================================================================

TEST(UI2_stretch_follows_the_parent) {
    // Растяжения не было вовсе: элемент имел фиксированный размер, и «панель во
    // всю ширину с отступом 24» приходилось пересчитывать скриптом на каждое
    // изменение окна. Якорь держал угол, ширину не держал никто.
    sage::ui::Transform t;
    t.Mode = sage::ui::Transform::Stretch::Horizontal;
    t.Margin = {24.0f, 10.0f, 24.0f, 0.0f};
    t.Size = {100.0f, 40.0f};

    const UIRect screen{0, 0, 800, 600};
    UIRect r = sage::ui::Resolve(t, screen, sage::ui::ResolveSize(t, screen));
    CHECK_NEAR(r.x, 24.0f, 1e-4f);
    CHECK_NEAR(r.w, 752.0f, 1e-4f); // 800 - 24 - 24
    CHECK_NEAR(r.h, 40.0f, 1e-4f);  // по вертикали не растягивали

    // Родитель другого размера — тот же элемент, другая ширина, без единой
    // правки данных.
    const UIRect narrow{0, 0, 400, 600};
    r = sage::ui::Resolve(t, narrow, sage::ui::ResolveSize(t, narrow));
    CHECK_NEAR(r.w, 352.0f, 1e-4f);

    // Поля больше родителя не дают отрицательной ширины: вывернутый наизнанку
    // прямоугольник рисуется мусором и ловит мышь там, где его не видно.
    const UIRect tiny{0, 0, 30, 30};
    r = sage::ui::Resolve(t, tiny, sage::ui::ResolveSize(t, tiny));
    CHECK_TRUE(r.w >= 0.0f);
}

TEST(UI2_pivot_moves_the_element_by_its_own_size) {
    // Pivot (0.5,0.5) — «якорь держит СЕРЕДИНУ элемента». Без него подпись,
    // растущая от центра, требовала пересчёта Offset при каждой смене текста.
    const UIRect screen{0, 0, 1000, 500};
    sage::ui::Transform t;
    t.Anchor = UIAnchor::TopLeft;
    t.Offset = {100.0f, 50.0f};
    t.Size = {200.0f, 40.0f};

    UIRect a = sage::ui::Resolve(t, screen, t.Size);
    CHECK_NEAR(a.x, 100.0f, 1e-4f);

    t.Pivot = {0.5f, 0.5f};
    UIRect b = sage::ui::Resolve(t, screen, t.Size);
    CHECK_NEAR(b.x, 0.0f, 1e-4f);   // 100 - 200*0.5
    CHECK_NEAR(b.y, 30.0f, 1e-4f);  // 50 - 40*0.5
}

TEST(UI2_layout_lays_children_out_by_itself) {
    // Меню из пяти кнопок раскладывалось вручную: каждому ребёнку свой Offset,
    // посчитанный на бумаге. Шестая кнопка означала пересчитать пять чужих
    // отступов.
    sage::ui::Layout column;
    column.Direction = sage::ui::Layout::Flow::Vertical;
    column.Spacing = 10.0f;
    column.Padding = {8.0f, 8.0f, 8.0f, 8.0f};

    std::vector<sage::ui::LayoutSlot> slots(3);
    for (auto& s : slots) s.Size = {100.0f, 40.0f};

    const UIRect box{0, 0, 200, 300};
    const glm::vec2 used = sage::ui::ApplyLayout(column, box, slots);

    CHECK_NEAR(slots[0].Pos.y, 8.0f, 1e-4f);
    CHECK_NEAR(slots[1].Pos.y, 58.0f, 1e-4f);  // 8 + 40 + 10
    CHECK_NEAR(slots[2].Pos.y, 108.0f, 1e-4f);
    CHECK_NEAR(used.y, 140.0f, 1e-4f);         // 3*40 + 2*10
    // StretchCross по умолчанию: ширина детей = ширине контейнера без полей.
    CHECK_NEAR(slots[0].Size.x, 184.0f, 1e-4f);

    // Ряд с «раздать свободное место между детьми»: первый прижат влево,
    // последний — вправо. Именно так выглядит строка «Назад ... Далее».
    sage::ui::Layout row;
    row.Direction = sage::ui::Layout::Flow::Horizontal;
    row.Justify = sage::ui::Layout::Align::SpaceBetween;
    row.Padding = {0.0f, 0.0f, 0.0f, 0.0f};
    row.StretchCross = false;
    std::vector<sage::ui::LayoutSlot> two(2);
    two[0].Size = {100.0f, 30.0f};
    two[1].Size = {100.0f, 30.0f};
    sage::ui::ApplyLayout(row, UIRect{0, 0, 500, 100}, two);
    CHECK_NEAR(two[0].Pos.x, 0.0f, 1e-4f);
    CHECK_NEAR(two[1].Pos.x, 400.0f, 1e-4f);

    // Сетка: перенос по столбцам.
    sage::ui::Layout grid;
    grid.Direction = sage::ui::Layout::Flow::Grid;
    grid.Columns = 2;
    grid.Spacing = 4.0f;
    grid.Padding = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<sage::ui::LayoutSlot> cells(4);
    for (auto& c : cells) c.Size = {50.0f, 50.0f};
    sage::ui::ApplyLayout(grid, UIRect{0, 0, 104, 200}, cells);
    CHECK_NEAR(cells[0].Pos.y, 0.0f, 1e-4f);
    CHECK_NEAR(cells[2].Pos.y, 54.0f, 1e-4f); // вторая строка
    CHECK_NEAR(cells[1].Pos.x, cells[3].Pos.x, 1e-4f); // один столбец
}

TEST(UI2_masks_intersect_and_never_go_negative) {
    // Вложенные маски режут друг друга: список внутри окна виден только там,
    // где окно и список пересекаются.
    const UIRect a{0, 0, 100, 100};
    const UIRect b{50, 50, 100, 100};
    UIRect i = sage::ui::Intersect(a, b);
    CHECK_NEAR(i.x, 50.0f, 1e-4f);
    CHECK_NEAR(i.w, 50.0f, 1e-4f);

    // Непересекающиеся окна дают ПУСТОЙ прямоугольник, а не отрицательный:
    // отрицательная ширина ниже по коду означала бы «обрезки нет», то есть
    // содержимое маски вылезло бы на весь экран.
    UIRect none = sage::ui::Intersect(UIRect{0, 0, 10, 10}, UIRect{100, 100, 10, 10});
    CHECK_TRUE(none.w == 0.0f && none.h == 0.0f);

    // Поля маски сжимают окно внутрь.
    sage::ui::Mask m;
    m.Padding = {4.0f, 4.0f, 4.0f, 4.0f};
    UIRect w = sage::ui::MaskWindow(m, UIRect{0, 0, 100, 100});
    CHECK_NEAR(w.x, 4.0f, 1e-4f);
    CHECK_NEAR(w.w, 92.0f, 1e-4f);
}

TEST(UI2_canvas_scales_symmetrically) {
    // Интерфейс жил в пикселях экрана: кнопка 200x52, выставленная на 1920x1080,
    // на 4K занимала четверть прежнего места. Холст задаёт опорное разрешение.
    sage::ui::Canvas c;
    c.Mode = sage::ui::Canvas::Scale::ScaleWithSize;
    c.Reference = {1920.0f, 1080.0f};

    CHECK_NEAR(sage::ui::CanvasScale(c, {1920.0f, 1080.0f}), 1.0f, 1e-4f);
    CHECK_NEAR(sage::ui::CanvasScale(c, {3840.0f, 2160.0f}), 2.0f, 1e-3f);
    CHECK_NEAR(sage::ui::CanvasScale(c, {960.0f, 540.0f}), 0.5f, 1e-3f);

    // Симметрия: вдвое уже и вдвое шире дают взаимно обратные множители.
    // При линейном смешивании это не так, и «сузили» с «расширили» на одну и ту
    // же долю меняли размер по-разному.
    c.MatchWidthOrHeight = 0.5f;
    const float wide = sage::ui::CanvasScale(c, {3840.0f, 1080.0f});
    const float narrow = sage::ui::CanvasScale(c, {960.0f, 1080.0f});
    CHECK_NEAR(wide * narrow, 1.0f, 1e-3f);

    // Режим «в пикселях» ничего не масштабирует — старые сцены не должны вдруг
    // поехать.
    sage::ui::Canvas pixels;
    CHECK_NEAR(sage::ui::CanvasScale(pixels, {800.0f, 600.0f}), 1.0f, 1e-6f);
}

TEST(UI2_presets_build_real_elements) {
    // Заготовка — данные, а не код редактора: одинаковую кнопку обязаны
    // собирать и меню редактора, и скрипт, и игра без редактора.
    const sage::ui::Preset* button = sage::ui::FindPreset("Button");
    CHECK_TRUE(button != nullptr);
    if (button) {
        CHECK_TRUE(button->HasFill && button->HasLabel && button->HasInteractable);
        CHECK_TRUE(!button->LabelStyle.Text.empty());
    }

    // Список — это маска плюс раскладка: то, что раньше собиралось вручную из
    // пяти сущностей и отдельного скрипта прокрутки.
    const sage::ui::Preset* list = sage::ui::FindPreset("Vertical List");
    CHECK_TRUE(list != nullptr);
    if (list) CHECK_TRUE(list->HasMask && list->HasLayout);

    // Полноэкранная подложка растягивается, а не задана числом.
    const sage::ui::Preset* screen = sage::ui::FindPreset("Screen");
    CHECK_TRUE(screen != nullptr);
    if (screen) CHECK_TRUE(screen->Xf.Mode == sage::ui::Transform::Stretch::Both);

    CHECK_TRUE(sage::ui::FindPreset("нет такой") == nullptr);
    CHECK_TRUE(sage::ui::PresetNames().size() >= 8);
}
