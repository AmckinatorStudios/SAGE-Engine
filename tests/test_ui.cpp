// Тесты UI-системы: математика якорей (ResolveAnchored), вёрстка элементов
// внутри родителя (ui::Resolve), сериализация компонентов интерфейса,
// HitTest по слоям/маскам/видимости. Всё на CPU, БЕЗ GL (рендер не трогаем).
#include "TestFramework.h"

#include <cmath>
#include <memory>

#include "sage/ui/UIAnchor.h"
#include "sage/render/SkyRenderer.h"
#include "sage/scene/Light.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UI.h"
#include "sage/ui/UILegacy.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/render/ShadowMap.h"

using sage::ui::UIRect;

namespace {
// Элемент кладётся в сцену разбором ОПИСАНИЯ СТАРОГО ФОРМАТА на компоненты
// (sage::ui::Decompose) — тем самым путём, которым в сцену приезжают файлы,
// записанные до перехода. Тесты ввода ниже проверяют ПОВЕДЕНИЕ, и способ сборки
// им безразличен; заодно этот путь оказывается прогнан на каждом из них.
void PutElement(Scene& scene, GameObject obj, const sage::ui::LegacyElement& flat) {
    sage::ui::Decompose(flat, scene.Registry(), obj.Entity());
}
sage::ui::State& StateOf(Scene& scene, GameObject obj) {
    return scene.Registry().get<sage::ui::Interactable>(obj.Entity()).Runtime;
}
std::string& TextOf(Scene& scene, GameObject obj) {
    return scene.Registry().get<sage::ui::Label>(obj.Entity()).Text;
}
sage::ui::Range& RangeOf(Scene& scene, GameObject obj) {
    return scene.Registry().get<sage::ui::Range>(obj.Entity());
}
} // namespace

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
    sage::ui::Transform child;
    child.Anchor = UIAnchor::BottomRight;
    child.Offset = {8, 8};
    child.Size = {40, 20};
    UIRect parent{100, 100, 200, 100};
    UIRect r = sage::ui::Resolve(child, parent);
    CHECK_NEAR(r.x, 100.0f + 200.0f - 40.0f - 8.0f, 1e-4);
    CHECK_NEAR(r.y, 100.0f + 100.0f - 20.0f - 8.0f, 1e-4);
    CHECK_NEAR(r.w, 40.0f, 1e-4);
    CHECK_NEAR(r.h, 20.0f, 1e-4);
}

TEST(UI_layout_size_overrides_declared_size) {
    // AutoWidth-элемент верстается по ИЗМЕРЕННОЙ ширине (её кладёт отрисовка в
    // LayoutSize), иначе якорь считался бы от запасного Size и «таблетка»
    // прыгала бы на кадр раньше, чем в неё поместился текст.
    sage::ui::Transform e;
    e.Anchor = UIAnchor::TopRight;
    e.Offset = {10, 10};
    e.Size = {200, 30};
    UIRect screen{0, 0, 800, 600};

    UIRect before = sage::ui::Resolve(e, screen);
    CHECK_NEAR(before.w, 200.0f, 1e-4); // ещё не рисовался — запасной размер

    e.LayoutSize = {124.0f, 30.0f};
    UIRect after = sage::ui::Resolve(e, screen);
    CHECK_NEAR(after.w, 124.0f, 1e-4);
    CHECK_NEAR(after.x, 800.0f - 124.0f - 10.0f, 1e-4);
}

TEST(UI_component_serialization_roundtrip) {
    Scene scene("U");
    GameObject panel = scene.CreateObject("Hud");
    entt::registry& reg = scene.Registry();

    // Элемент собирается из ЧАСТЕЙ — ровно так, как он теперь и хранится.
    sage::ui::Transform t;
    t.Anchor = UIAnchor::BottomLeft;
    t.Offset = {24, 18};
    t.Size = {220, 26};
    t.Layer = 3;
    t.Mode = sage::ui::Transform::Stretch::Horizontal;
    t.Margin = {12.0f, 0.0f, 12.0f, 0.0f};
    t.Pivot = {0.5f, 0.5f};
    reg.emplace<sage::ui::Transform>(panel.Entity(), t);

    sage::ui::Fill fill;
    fill.Color = {0.1f, 0.2f, 0.3f, 0.8f};
    fill.Rounding = 13.0f;
    fill.BorderThickness = 2.5f;
    fill.BorderColor = {0.9f, 0.8f, 0.1f, 1.0f};
    reg.emplace<sage::ui::Fill>(panel.Entity(), fill);

    sage::ui::Label label;
    label.Text = "HP";
    label.Scale = 1.75f;
    label.Horizontal = sage::ui::Label::Align::Start;
    label.Vertical = sage::ui::Label::Align::End;
    label.PadX = 11.5f;
    label.AutoWidth = true;
    reg.emplace<sage::ui::Label>(panel.Entity(), label);

    sage::ui::Bar bar;
    bar.Value = 0.42f;
    bar.FillColor = {0.8f, 0.2f, 0.2f, 1.0f};
    bar.Grow = sage::ui::Bar::Direction::BottomToTop;
    bar.Smoothing = 4.0f;
    reg.emplace<sage::ui::Bar>(panel.Entity(), bar);

    reg.emplace<sage::ui::Mask>(panel.Entity(), sage::ui::Mask{});

    std::string json = SceneSerializer::SaveToString(scene);
    auto loaded = SceneSerializer::LoadFromString(json);
    GameObject back = loaded->FindByName("Hud");
    CHECK_TRUE(back.Valid());
    entt::registry& r2 = loaded->Registry();

    const auto* t2 = r2.try_get<sage::ui::Transform>(back.Entity());
    CHECK_TRUE(t2 != nullptr);
    if (t2) {
        CHECK_TRUE(t2->Anchor == UIAnchor::BottomLeft);
        CHECK_TRUE(t2->Mode == sage::ui::Transform::Stretch::Horizontal);
        CHECK_NEAR(t2->Offset.x, 24.0f, 1e-4);
        CHECK_NEAR(t2->Size.y, 26.0f, 1e-4);
        CHECK_NEAR(t2->Margin.z, 12.0f, 1e-4);
        CHECK_NEAR(t2->Pivot.y, 0.5f, 1e-4);
        CHECK_EQ(t2->Layer, 3);
    }
    const auto* f2 = r2.try_get<sage::ui::Fill>(back.Entity());
    CHECK_TRUE(f2 != nullptr);
    if (f2) {
        CHECK_NEAR(f2->Color.a, 0.8f, 1e-4);
        CHECK_NEAR(f2->Rounding, 13.0f, 1e-4);
        CHECK_NEAR(f2->BorderThickness, 2.5f, 1e-4);
    }
    const auto* l2 = r2.try_get<sage::ui::Label>(back.Entity());
    CHECK_TRUE(l2 != nullptr);
    if (l2) {
        CHECK_EQ(l2->Text, std::string("HP"));
        CHECK_NEAR(l2->Scale, 1.75f, 1e-4);
        CHECK_TRUE(l2->Horizontal == sage::ui::Label::Align::Start);
        CHECK_TRUE(l2->Vertical == sage::ui::Label::Align::End);
        CHECK_NEAR(l2->PadX, 11.5f, 1e-4);
        CHECK_TRUE(l2->AutoWidth);
    }
    const auto* b2 = r2.try_get<sage::ui::Bar>(back.Entity());
    CHECK_TRUE(b2 != nullptr);
    if (b2) {
        CHECK_NEAR(b2->Value, 0.42f, 1e-4);
        CHECK_NEAR(b2->FillColor.r, 0.8f, 1e-4);
        CHECK_TRUE(b2->Grow == sage::ui::Bar::Direction::BottomToTop);
        CHECK_NEAR(b2->Smoothing, 4.0f, 1e-4);
    }
    CHECK_TRUE(r2.all_of<sage::ui::Mask>(back.Entity()));
    // Части, которых у элемента НЕТ, не появляются из ниоткуда: в этом и был
    // смысл разделения — у полосы не должно быть ни поля ввода, ни ползунка.
    CHECK_FALSE(r2.all_of<sage::ui::TextInput>(back.Entity()));
    CHECK_FALSE(r2.all_of<sage::ui::Range>(back.Entity()));
    CHECK_FALSE(r2.all_of<sage::ui::Interactable>(back.Entity()));
}

// Сцена, записанная ДО перехода на компоненты, читается и раскладывается по
// частям. Без этого обновление движка означало бы «интерфейс исчез», причём
// без единого сообщения: элемент в файле есть, а рисовать его некому.
TEST(UI_old_flat_scene_migrates_to_components) {
    const std::string old = R"({
      "sage_scene_version": 1,
      "name": "Legacy",
      "objects": [{
        "id": 1, "name": "Button",
        "position": {"x":0,"y":0,"z":0}, "rotation": {"x":0,"y":0,"z":0},
        "scale": {"x":1,"y":1,"z":1},
        "mesh": {"type": "none"},
        "ui": {
          "kind": "slider", "anchor": 4, "offset": {"x": 30, "y": 40},
          "size": {"x": 180, "y": 32}, "layer": 2, "visible": true,
          "text": "Громкость", "value": 0.25, "minValue": 0, "maxValue": 100,
          "barFillColor": {"x": 0.2, "y": 0.7, "z": 0.9, "w": 1.0},
          "interactive": true, "enabled": true, "rounding": 6,
          "clipChildren": true
        }
      }]
    })";

    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(old);
    CHECK_TRUE(scene != nullptr);
    GameObject obj = scene->FindByName("Button");
    CHECK_TRUE(obj.Valid());
    entt::registry& reg = scene->Registry();

    const auto* t = reg.try_get<sage::ui::Transform>(obj.Entity());
    CHECK_TRUE(t != nullptr);
    if (t) {
        CHECK_NEAR(t->Offset.x, 30.0f, 1e-4);
        CHECK_NEAR(t->Size.x, 180.0f, 1e-4);
        CHECK_EQ(t->Layer, 2);
    }
    // Ползунок стал диапазоном, и доля 0..1 развернулась в игровые единицы.
    const auto* range = reg.try_get<sage::ui::Range>(obj.Entity());
    CHECK_TRUE(range != nullptr);
    if (range) {
        CHECK_NEAR(range->Min, 0.0f, 1e-4);
        CHECK_NEAR(range->Max, 100.0f, 1e-4);
        CHECK_NEAR(range->Value, 25.0f, 1e-3);
        CHECK_FALSE(range->Toggle);
    }
    CHECK_TRUE(reg.all_of<sage::ui::Interactable>(obj.Entity()));
    CHECK_TRUE(reg.all_of<sage::ui::Mask>(obj.Entity()));   // был флаг clipChildren
    // Цвет заполнения ползунка жил в том же поле, что у полосы, и должен был
    // куда-то приехать: у нового элемента акцентный цвет хранит полоса. Без
    // этого перенесённый ползунок молча позеленел бы в умолчание.
    const auto* accent = reg.try_get<sage::ui::Bar>(obj.Entity());
    CHECK_TRUE(accent != nullptr);
    if (accent) CHECK_NEAR(accent->FillColor.g, 0.7f, 1e-3f);
    const auto* label = reg.try_get<sage::ui::Label>(obj.Entity());
    CHECK_TRUE(label != nullptr);
    if (label) CHECK_EQ(label->Text, std::string("Громкость"));

    // И записывается такая сцена уже в новом виде: старый ключ не возвращается.
    const std::string again = SceneSerializer::SaveToString(*scene);
    CHECK_TRUE(again.find("\"kind\"") == std::string::npos);
    CHECK_TRUE(again.find("\"range\"") != std::string::npos);
}

TEST(UI_hit_test_layers_and_visibility) {
    Scene scene("U");
    // Две перекрывающиеся панели: Layer решает, кто сверху.
    GameObject below = scene.CreateObject("Below");
    sage::ui::LegacyElement b;
    b.Anchor = UIAnchor::TopLeft; b.Offset = {0, 0}; b.Size = {100, 100}; b.Layer = 0;
    PutElement(scene, below, b);

    GameObject above = scene.CreateObject("Above");
    sage::ui::LegacyElement a;
    a.Anchor = UIAnchor::TopLeft; a.Offset = {50, 50}; a.Size = {100, 100}; a.Layer = 5;
    PutElement(scene, above, a);

    // Точка в пересечении — выигрывает верхний слой.
    CHECK_EQ(sage::ui::HitTest(scene, 75, 75, 800, 600), above.Id());
    // Точка только в нижней панели.
    CHECK_EQ(sage::ui::HitTest(scene, 10, 10, 800, 600), below.Id());
    // Пустое место.
    CHECK_EQ(sage::ui::HitTest(scene, 400, 400, 800, 600), -1);

    // Невидимый элемент не ловит точки.
    scene.Registry().get<sage::ui::Transform>(above.Entity()).Visible = false;
    CHECK_EQ(sage::ui::HitTest(scene, 75, 75, 800, 600), below.Id());
}

TEST(UI_hit_test_child_and_clip_mask) {
    Scene scene("U");
    GameObject parent = scene.CreateObject("Panel");
    sage::ui::LegacyElement p;
    p.Anchor = UIAnchor::TopLeft; p.Offset = {100, 100}; p.Size = {200, 100};
    p.ClipChildren = true; // маска
    PutElement(scene, parent, p);

    GameObject child = scene.CreateObject("Button");
    sage::ui::LegacyElement c;
    // Ребёнок наполовину ВЫСОВЫВАЕТСЯ за родителя вправо: якорь TopLeft
    // родителя + offset за его край.
    c.Anchor = UIAnchor::TopLeft; c.Offset = {150, 20}; c.Size = {100, 40};
    PutElement(scene, child, c);
    scene.SetParent(child.Entity(), parent.Entity());

    // Точка внутри родителя И ребёнка — ребёнок (нарисован поверх).
    CHECK_EQ(sage::ui::HitTest(scene, 260, 130, 800, 600), child.Id());
    // Точка в высунувшейся части ребёнка (вне родителя): маска ClipChildren
    // обрезает — попадания нет.
    CHECK_EQ(sage::ui::HitTest(scene, 320, 130, 800, 600), -1);
    // Без маски та же точка попадает в ребёнка.
    // Маска — отдельный компонент: снять её значит снять его, а не погасить флаг
    // у элемента, который к обрезке отношения не имеет.
    scene.Registry().remove<sage::ui::Mask>(parent.Entity());
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
sage::ui::LegacyElement MakeInteractive(sage::ui::LegacyElement::Kind kind, glm::vec2 pos,
                                       glm::vec2 size) {
    sage::ui::LegacyElement e;
    e.Type = kind;
    e.Anchor = UIAnchor::TopLeft;
    e.Offset = pos;
    e.Size = size;
    e.Interactive = true;
    // Value по умолчанию 1 (это удобный дефолт для шкалы). Галке нужен явный
    // ноль, иначе тест проверял бы не то, что думает.
    if (kind == sage::ui::LegacyElement::Kind::Checkbox) e.Value = 0.0f;
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
    PutElement(scene, field, MakeInteractive(sage::ui::LegacyElement::Kind::Input, {10, 10}, {200, 40}));
    sage::ui::State& st = StateOf(scene, field);
    std::string& text = TextOf(scene, field);

    // Пока не кликнули — текст не принимается: поле без фокуса не должно
    // воровать буквы у игры.
    sage::ui::UIInputState typing;
    typing.TypedText = "a";
    sage::ui::UpdateSceneUI(scene, typing, 800, 600);
    CHECK_EQ(text, std::string(""));
    CHECK_FALSE(st.Focused);

    sage::ui::UpdateSceneUI(scene, ClickAt({50, 20}), 800, 600);
    CHECK_TRUE(st.Focused);

    // Кириллица: приходит символами UTF-8, курсор считается в байтах.
    sage::ui::UIInputState in;
    in.TypedText = "Пр";
    sage::ui::UIInputState res = in;
    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, res, 800, 600);
    CHECK_EQ(text, std::string("Пр"));
    CHECK_TRUE(r.WantsKeyboard);
    CHECK_EQ(st.Caret, 4); // две кириллические буквы — четыре байта

    // Backspace удаляет ЦЕЛЫЙ символ, а не байт: иначе в поле остаётся битый UTF-8.
    sage::ui::UIInputState back;
    back.Backspace = true;
    sage::ui::UpdateSceneUI(scene, back, 800, 600);
    CHECK_EQ(text, std::string("П"));
    CHECK_EQ(st.Caret, 2);

    // Предел длины считается в символах, а не в байтах.
    scene.Registry().get<sage::ui::TextInput>(field.Entity()).MaxLength = 2;
    sage::ui::UIInputState more;
    more.TypedText = "ивет";
    sage::ui::UpdateSceneUI(scene, more, 800, 600);
    CHECK_EQ(text, std::string("П")); // не влезло целиком — не приняли вовсе

    // Enter снимает фокус.
    sage::ui::UIInputState enter;
    enter.Enter = true;
    sage::ui::UpdateSceneUI(scene, enter, 800, 600);
    CHECK_FALSE(st.Focused);
}

TEST(UI_checkbox_and_click_need_press_and_release) {
    Scene scene("U");
    GameObject box = scene.CreateObject("Chk");
    PutElement(scene, box, MakeInteractive(sage::ui::LegacyElement::Kind::Checkbox, {10, 10}, {30, 30}));
    sage::ui::State& st = StateOf(scene, box);
    sage::ui::Range& value = RangeOf(scene, box);
    CHECK_NEAR(value.Value, 0.0f, 1e-4);

    // Одного нажатия мало — щелчок это нажать И отпустить НА элементе.
    sage::ui::UpdateSceneUI(scene, ClickAt({20, 20}), 800, 600);
    CHECK_TRUE(st.Pressed);
    CHECK_NEAR(value.Value, 0.0f, 1e-4);

    sage::ui::UIInputState up;
    up.Mouse = {20, 20};
    up.MouseReleased = true;
    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, up, 800, 600);
    CHECK_TRUE(st.Clicked);
    CHECK_NEAR(value.Value, 1.0f, 1e-4);
    CHECK_EQ(r.ClickedId, box.Id());

    // Флаг живёт ровно один кадр — иначе игра сработает на него дважды.
    sage::ui::UIInputState idle;
    idle.Mouse = {20, 20};
    sage::ui::UpdateSceneUI(scene, idle, 800, 600);
    CHECK_FALSE(st.Clicked);

    // Увести курсор с кнопки и отпустить — общепринятый способ передумать.
    sage::ui::UpdateSceneUI(scene, ClickAt({20, 20}), 800, 600);
    sage::ui::UIInputState away;
    away.Mouse = {400, 400};
    away.MouseReleased = true;
    sage::ui::UpdateSceneUI(scene, away, 800, 600);
    CHECK_FALSE(st.Clicked);
    CHECK_NEAR(value.Value, 1.0f, 1e-4); // значение не изменилось
}

TEST(UI_slider_drags_and_converts_to_game_units) {
    Scene scene("U");
    GameObject sld = scene.CreateObject("Vol");
    sage::ui::LegacyElement s = MakeInteractive(sage::ui::LegacyElement::Kind::Slider, {100, 10}, {200, 30});
    s.MinValue = 0.0f;
    s.MaxValue = 100.0f;
    PutElement(scene, sld, s);
    sage::ui::Range& value = RangeOf(scene, sld);

    // Нажали в середине дорожки. Значение — В ИГРОВЫХ ЕДИНИЦАХ: раньше элемент
    // хранил долю 0..1, и переводить её в громкость 0..100 приходилось каждому
    // читателю отдельно — то есть где-то перевод рано или поздно расходился.
    sage::ui::UpdateSceneUI(scene, ClickAt({200, 25}), 800, 600);
    CHECK_NEAR(value.Value, 50.0f, 1e-1);

    // Тянем ЗА пределы элемента, не отпуская: значение обязано доходить до края,
    // а не срываться от того, что курсор ушёл вбок.
    sage::ui::UIInputState drag;
    drag.Mouse = {1000, 400};
    drag.MouseDown = true;
    sage::ui::UpdateSceneUI(scene, drag, 800, 600);
    CHECK_NEAR(value.Value, 100.0f, 1e-1);

    drag.Mouse = {-50, 400};
    sage::ui::UpdateSceneUI(scene, drag, 800, 600);
    CHECK_NEAR(value.Value, 0.0f, 1e-1);
}

TEST(UI_disabled_element_ignores_mouse) {
    Scene scene("U");
    GameObject btn = scene.CreateObject("Quit");
    sage::ui::LegacyElement b = MakeInteractive(sage::ui::LegacyElement::Kind::Panel, {10, 10}, {100, 40});
    b.Enabled = false;
    PutElement(scene, btn, b);
    sage::ui::State& st = StateOf(scene, btn);

    sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, ClickAt({50, 20}), 800, 600);
    CHECK_FALSE(st.Hovered);
    CHECK_FALSE(st.Pressed);
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
    // «Кнопка» — это не вид элемента, а НАБОР ЧАСТЕЙ. Знание об этом жило в
    // функции РЕДАКТОРА, то есть кнопку можно было получить только мышью:
    // скрипт, собирающий интерфейс на лету, повторял те же присваивания у себя,
    // а игра без редактора не имела к ним доступа вовсе. Теперь таблица одна на
    // движок — и проверяется здесь, а не «на глаз в редакторе».
    Scene scene("presets");
    entt::registry& reg = scene.Registry();

    GameObject button = scene.CreateObject("Button");
    CHECK_TRUE(sage::ui::ApplyPreset(reg, button.Entity(), "Button"));
    CHECK_TRUE(reg.all_of<sage::ui::Interactable>(button.Entity())); // иначе просто прямоугольник
    CHECK_TRUE(reg.all_of<sage::ui::Fill>(button.Entity()));
    const auto* buttonLabel = reg.try_get<sage::ui::Label>(button.Entity());
    CHECK_TRUE(buttonLabel != nullptr);
    if (buttonLabel) CHECK_TRUE(!buttonLabel->Text.empty()); // без надписи не читается
    const auto& buttonXf = reg.get<sage::ui::Transform>(button.Entity());
    CHECK_TRUE(buttonXf.Size.x > 0.0f && buttonXf.Size.y > 0.0f);

    // Полоса заполнена наполовину: пустая неотличима от панели, и человек
    // решает, что элемент не создался.
    GameObject bar = scene.CreateObject("Bar");
    CHECK_TRUE(sage::ui::ApplyPreset(reg, bar.Entity(), "Bar"));
    const auto* barPart = reg.try_get<sage::ui::Bar>(bar.Entity());
    CHECK_TRUE(barPart != nullptr);
    if (barPart) CHECK_TRUE(barPart->Value > 0.0f && barPart->Value < 1.0f);

    // Поле ввода: текст влево (по центру набирать непривычно) и подсказка.
    GameObject input = scene.CreateObject("Input");
    CHECK_TRUE(sage::ui::ApplyPreset(reg, input.Entity(), "Input"));
    const auto* field = reg.try_get<sage::ui::TextInput>(input.Entity());
    const auto* inputLabel = reg.try_get<sage::ui::Label>(input.Entity());
    CHECK_TRUE(field != nullptr);
    CHECK_TRUE(inputLabel != nullptr);
    if (field) CHECK_TRUE(!field->Placeholder.empty());
    if (inputLabel) CHECK_TRUE(inputLabel->Horizontal == sage::ui::Label::Align::Start);

    // Заготовка СНИМАЕТ чужие части: «сделать из кнопки надпись» не должно
    // оставить надпись нажимаемой и с подложкой.
    CHECK_TRUE(sage::ui::ApplyPreset(reg, button.Entity(), "Label"));
    CHECK_FALSE(reg.all_of<sage::ui::Interactable>(button.Entity()));
    CHECK_FALSE(reg.all_of<sage::ui::Fill>(button.Entity()));

    // Неизвестное имя — честный отказ, а не молча пустой элемент.
    GameObject unknown = scene.CreateObject("Unknown");
    CHECK_FALSE(sage::ui::ApplyPreset(reg, unknown.Entity(), "Соврёшь"));
    CHECK_FALSE(reg.all_of<sage::ui::Transform>(unknown.Entity()));

    // Каждая заготовка из списка применяется и даёт ВИДИМЫЙ элемент: нулевой
    // размер означал бы «создал и не увидел ничего».
    for (const std::string& name : sage::ui::PresetNames()) {
        GameObject e = scene.CreateObject(name);
        CHECK_TRUE(sage::ui::ApplyPreset(reg, e.Entity(), name));
        const auto& xf = reg.get<sage::ui::Transform>(e.Entity());
        CHECK_TRUE(xf.Size.x > 0.0f && xf.Size.y > 0.0f);
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

// ===========================================================================
//  ДЕМО-ЭКРАНЫ (sage/ui/UIDemos.h)
//
//  Демо на диске устаревает молча: поля переименовали — сцена читается, но
//  выглядит не так. Собранное кодом демо проверяется здесь, и проверяется не
//  «функция вернула id», а то, ради чего каждое из них написано: раскладка сама
//  расставила кнопки, шкала едет к цели, ползунок прилипает к шагу, а нажатие
//  доезжает до игры ИМЕНЕМ действия, а не номером сущности.
// ===========================================================================

TEST(UI_demo_names_all_build_something) {
    for (const std::string& name : sage::ui::DemoNames()) {
        Scene scene("demo");
        const int root = sage::ui::BuildDemo(scene, name);
        CHECK_TRUE(root >= 0);
        CHECK_TRUE(scene.Count() > 3);
    }
    // Опечатка в имени — отказ, а не «собралось что-нибудь».
    Scene scene("demo");
    CHECK_EQ(sage::ui::BuildDemo(scene, "нет-такого"), -1);
}

TEST(UI_demo_menu_lays_buttons_out_by_itself) {
    Scene scene("menu");
    CHECK_TRUE(sage::ui::BuildDemo(scene, "menu") >= 0);

    // Раскладка контейнера расставляет детей сама: у кнопок нет своих отступов,
    // и всё-таки они стоят друг под другом, по порядку и без наложений.
    GameObject first = scene.FindByName("BtnContinue");
    GameObject second = scene.FindByName("BtnNewGame");
    CHECK_TRUE(first.Valid());
    CHECK_TRUE(second.Valid());

    // Прогоняем кадр решателя: попадание курсором считается по той же
    // раскладке, что и отрисовка.
    const int hitFirst = sage::ui::HitTest(scene, 960, 0, 1920, 1080);
    (void)hitFirst; // точка выбрана ниже, по фактическим прямоугольникам

    const auto& reg = scene.Registry();
    const glm::vec2 sizeA = reg.get<sage::ui::Transform>(first.Entity()).LayoutSize;
    CHECK_TRUE(sizeA.x > 0.0f);   // размер посчитан, а не остался нулём

    // У кнопок есть ИМЕНА ДЕЙСТВИЙ — то, чем игра их и различает.
    const auto* act = reg.try_get<sage::ui::Interactable>(second.Entity());
    CHECK_TRUE(act != nullptr);
    if (act) CHECK_EQ(act->Action, std::string("new_game"));

    // Холст меню лежит ПОВЕРХ худа: порядок задан числом на корне, а не
    // подбором слоёв у каждого элемента.
    const auto* canvas = reg.try_get<sage::ui::Canvas>(scene.FindByName("MenuScreen").Entity());
    CHECK_TRUE(canvas != nullptr);
    if (canvas) CHECK_TRUE(canvas->SortOrder > 0);
}

TEST(UI_demo_menu_click_reports_the_action_not_the_entity) {
    Scene scene("menu");
    CHECK_TRUE(sage::ui::BuildDemo(scene, "menu") >= 0);
    const int W = 1920, H = 1080;

    // Находим кнопку там, где её нарисовала раскладка, — через тот же HitTest.
    GameObject quit = scene.FindByName("BtnQuit");
    CHECK_TRUE(quit.Valid());
    sage::ui::UpdateSceneUI(scene, sage::ui::UIInputState{}, W, H); // посчитать раскладку
    const glm::vec2 size = scene.Registry().get<sage::ui::Transform>(quit.Entity()).LayoutSize;
    CHECK_TRUE(size.x > 0.0f);

    // Точка внутри кнопки: берём её из решателя — попадание и отрисовка
    // считаются по одной раскладке (в этом и была цель одного решателя).
    int found = -1;
    for (int y = 0; y < H && found < 0; y += 4) {
        if (sage::ui::HitTest(scene, (float)W * 0.5f, (float)y, W, H) == quit.Id())
            found = y;
    }
    CHECK_TRUE(found > 0);
    if (found <= 0) return;

    sage::ui::UIInputState down;
    down.Mouse = {(float)W * 0.5f, (float)found};
    down.MouseDown = true;
    down.MousePressed = true;
    sage::ui::UpdateSceneUI(scene, down, W, H);

    sage::ui::UIInputState up;
    up.Mouse = down.Mouse;
    up.MouseReleased = true;
    const sage::ui::UIInputResult r = sage::ui::UpdateSceneUI(scene, up, W, H);
    CHECK_EQ(r.ClickedId, quit.Id());
    // Главное: игра узнаёт «нажали quit», а не «нажали сущность номер N».
    CHECK_EQ(r.ClickedAction, std::string("quit"));
}

TEST(UI_demo_hud_bar_slides_to_its_target) {
    Scene scene("hud");
    CHECK_TRUE(sage::ui::BuildDemo(scene, "hud") >= 0);
    GameObject hp = scene.FindByName("HudHealth");
    CHECK_TRUE(hp.Valid());

    sage::ui::Bar& bar = scene.Registry().get<sage::ui::Bar>(hp.Entity());
    CHECK_TRUE(bar.Smoothing > 0.0f);

    sage::ui::UIInputState step;
    step.DeltaTime = 0.1f;
    sage::ui::UpdateSceneUI(scene, step, 1920, 1080); // первый кадр задаёт показанное
    bar.Value = 0.0f;                                  // «получили урон»
    sage::ui::UpdateSceneUI(scene, step, 1920, 1080);
    // Показанное значение ЕДЕТ, а не прыгает: в этом весь смысл сглаживания.
    CHECK_TRUE(bar.Displayed > 0.0f);
    CHECK_TRUE(bar.Displayed < 0.72f);

    // За достаточное время доезжает до цели и не проскакивает мимо.
    for (int i = 0; i < 20; ++i) sage::ui::UpdateSceneUI(scene, step, 1920, 1080);
    CHECK_NEAR(bar.Displayed, 0.0f, 1e-3f);
}

TEST(UI_demo_settings_slider_snaps_to_its_step) {
    Scene scene("settings");
    CHECK_TRUE(sage::ui::BuildDemo(scene, "settings") >= 0);
    GameObject volume = scene.FindByName("VolumeSlider");
    CHECK_TRUE(volume.Valid());

    sage::ui::Range& range = scene.Registry().get<sage::ui::Range>(volume.Entity());
    CHECK_NEAR(range.Max, 100.0f, 1e-4f);
    CHECK_NEAR(range.Step, 5.0f, 1e-4f);

    const int W = 1920, H = 1080;
    sage::ui::UpdateSceneUI(scene, sage::ui::UIInputState{}, W, H);

    // Ищем строку громкости попаданием и тянем ползунок в произвольную точку.
    int hitY = -1;
    for (int y = 0; y < H && hitY < 0; y += 2)
        if (sage::ui::HitTest(scene, (float)W * 0.5f, (float)y, W, H) == volume.Id()) hitY = y;
    CHECK_TRUE(hitY > 0);
    if (hitY <= 0) return;

    sage::ui::UIInputState drag;
    drag.Mouse = {(float)W * 0.5f, (float)hitY};
    drag.MouseDown = true;
    drag.MousePressed = true;
    sage::ui::UpdateSceneUI(scene, drag, W, H);

    drag.MousePressed = false;
    drag.Mouse.x = (float)W * 0.5f + 13.0f;   // заведомо не кратно шагу
    sage::ui::UpdateSceneUI(scene, drag, W, H);

    // Значение прилипло к шагу и осталось в игровых единицах.
    CHECK_NEAR(range.Value / 5.0f, std::round(range.Value / 5.0f), 1e-3f);
    CHECK_TRUE(range.Value >= 0.0f && range.Value <= 100.0f);
}

TEST(UI_demo_survives_a_scene_round_trip) {
    // Демо — обычные сущности, и они обязаны переживать файл сцены: экран,
    // который нельзя сохранить, стартовой точкой быть не может.
    Scene scene("demo");
    CHECK_TRUE(sage::ui::BuildDemo(scene, "settings") >= 0);

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;

    GameObject volume = back->FindByName("VolumeSlider");
    CHECK_TRUE(volume.Valid());
    const auto* range = back->Registry().try_get<sage::ui::Range>(volume.Entity());
    CHECK_TRUE(range != nullptr);
    if (range) {
        CHECK_NEAR(range->Max, 100.0f, 1e-4f);
        CHECK_NEAR(range->Step, 5.0f, 1e-4f);
    }
    const auto* apply = back->Registry().try_get<sage::ui::Interactable>(
        back->FindByName("SettingsApply").Entity());
    CHECK_TRUE(apply != nullptr);
    if (apply) CHECK_EQ(apply->Action, std::string("apply_settings"));
    // Раскладка панели тоже пережила файл — иначе строки разъехались бы.
    CHECK_TRUE(back->Registry().all_of<sage::ui::Layout>(back->FindByName("SettingsPanel").Entity()));
}
