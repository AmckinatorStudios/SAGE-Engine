// ===========================================================================
//  ПРОКРУТКА И АДАПТИВНОЕ РАЗМЕЩЕНИЕ (этап 5 пространства «Интерфейс»).
//
//  Полоса прокрутки, перетаскивание содержимого, границы содержимого;
//  пределы размера, пропорции и безопасная область. Проверяется по настоящему
//  решению сцены и настоящему вводу, а не по полям компонентов.
// ===========================================================================
#include "TestFramework.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIScroll.h"
#include "sage/ui/UISerialize.h"

#include <nlohmann/json.hpp>

using sage::ui::UIRect;

namespace {

GameObject Box(Scene& scene, glm::vec2 pos, glm::vec2 size, entt::entity parent = entt::null) {
    GameObject o = scene.CreateEmptyObject("Элемент");
    sage::ui::Element el;
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    scene.Registry().emplace<sage::ui::Element>(o.Entity(), el);
    if (parent != entt::null) scene.SetParent(o.Entity(), parent);
    return o;
}

UIRect RectOf(Scene& scene, entt::entity e, int w = 800, int h = 600) {
    for (const sage::ui::ElementRect& r : sage::ui::SolveSceneRects(scene, w, h, true))
        if (r.Entity == e) return r.Rect;
    return UIRect{};
}

sage::ui::UIInputState Mouse(glm::vec2 p, bool down, bool pressed = false, bool released = false) {
    sage::ui::UIInputState in;
    in.Mouse = p;
    in.MouseDown = down;
    in.MousePressed = pressed;
    in.MouseReleased = released;
    return in;
}

// Список 200x100 в углу, внутри — ребёнок по якорю высотой 400.
struct ListFixture {
    Scene scene{"scroll"};
    GameObject list, content;
    ListFixture() {
        list = Box(scene, {0, 0}, {200, 100});
        sage::ui::Scroll sc;
        scene.Registry().emplace<sage::ui::Scroll>(list.Entity(), sc);
        content = Box(scene, {0, 0}, {180, 400}, list.Entity());
        RectOf(scene, list.Entity());   // первый кадр узнаёт размер содержимого
    }
    sage::ui::Scroll& S() { return scene.Registry().get<sage::ui::Scroll>(list.Entity()); }
};

} // namespace

TEST(UIScroll_children_by_anchor_give_the_content_size) {
    // Без раскладки содержимое — там, где кончается дальний ребёнок. Раньше
    // размер не считался вовсе, и предел «не дальше краёв» не пускал никуда.
    ListFixture f;
    f.S().Offset = {0, 1000};
    RectOf(f.scene, f.list.Entity());
    CHECK_NEAR(f.S().Offset.y, 300.0f, 0.5f);
}

TEST(UIScroll_content_size_overrides_the_children) {
    ListFixture f;
    f.S().ContentSize = {0, 250};
    f.S().Offset = {0, 1000};
    RectOf(f.scene, f.list.Entity());
    RectOf(f.scene, f.list.Entity());
    CHECK_NEAR(f.S().Offset.y, 150.0f, 0.5f);
}

TEST(UIScroll_scrollbar_thumb_shows_the_visible_part) {
    ListFixture f;
    UIRect track, thumb;
    const UIRect r{0, 0, 200, 100};
    CHECK_TRUE(sage::ui::ScrollbarRects(f.S(), r, 1.0f, 1, track, thumb));
    CHECK_NEAR(track.x, 194.0f, 0.01f);           // у правого края, ширина 6
    CHECK_NEAR(thumb.h, 25.0f, 0.01f);            // видно четверть
    CHECK_NEAR(thumb.y, 0.0f, 0.01f);
    f.S().Offset.y = 300.0f;                      // в самом низу
    sage::ui::ScrollbarRects(f.S(), r, 1.0f, 1, track, thumb);
    CHECK_NEAR(thumb.y + thumb.h, 100.0f, 0.01f);
    // Горизонтальной оси нет — и полосы нет; выключенная полоса не рисуется.
    CHECK_FALSE(sage::ui::ScrollbarRects(f.S(), r, 1.0f, 0, track, thumb));
    f.S().Scrollbar = false;
    CHECK_FALSE(sage::ui::ScrollbarRects(f.S(), r, 1.0f, 1, track, thumb));
}

TEST(UIScroll_dragging_the_thumb_scrolls) {
    ListFixture f;
    sage::ui::UpdateSceneUI(f.scene, Mouse({197, 10}, true, true), 800, 600);
    // Бегунку есть куда ехать 75 пикселей, содержимому — 300: вчетверо.
    sage::ui::UpdateSceneUI(f.scene, Mouse({197, 40}, true), 800, 600);
    CHECK_NEAR(f.S().Offset.y, 120.0f, 0.5f);
    sage::ui::UpdateSceneUI(f.scene, Mouse({197, 40}, false, false, true), 800, 600);
    CHECK_TRUE(f.S().Dragging == sage::ui::Scroll::Drag::None);
}

TEST(UIScroll_click_on_the_track_pages) {
    ListFixture f;
    sage::ui::UpdateSceneUI(f.scene, Mouse({197, 90}, true, true), 800, 600);
    CHECK_NEAR(f.S().Offset.y, 100.0f, 0.5f);     // на высоту окна
}

TEST(UIScroll_dragging_the_content_does_not_click) {
    ListFixture f;
    f.S().DragContent = true;
    entt::registry& reg = f.scene.Registry();
    reg.emplace<sage::ui::Interactable>(f.content.Entity());

    // Нажал и отпустил на месте — это щелчок по кнопке внутри списка.
    sage::ui::UpdateSceneUI(f.scene, Mouse({50, 50}, true, true), 800, 600);
    auto r = sage::ui::UpdateSceneUI(f.scene, Mouse({50, 50}, false, false, true), 800, 600);
    CHECK_EQ(r.ClickedId, f.content.Id());
    CHECK_NEAR(f.S().Offset.y, 0.0f, 0.01f);

    // Повёл пальцем вверх дальше порога — это прокрутка, и отпускание не щёлкает.
    sage::ui::UpdateSceneUI(f.scene, Mouse({50, 80}, true, true), 800, 600);
    sage::ui::UpdateSceneUI(f.scene, Mouse({50, 40}, true), 800, 600);
    CHECK_NEAR(f.S().Offset.y, 40.0f, 0.01f);
    r = sage::ui::UpdateSceneUI(f.scene, Mouse({50, 40}, false, false, true), 800, 600);
    CHECK_EQ(r.ClickedId, -1);
}

TEST(UIAdaptive_min_max_and_aspect_constrain_the_size) {
    sage::ui::Element el;
    el.MaxSize = {300, 0};
    el.MinSize = {0, 50};
    CHECK_NEAR(sage::ui::ConstrainSize(el, {800, 20}).x, 300.0f, 0.01f);
    CHECK_NEAR(sage::ui::ConstrainSize(el, {800, 20}).y, 50.0f, 0.01f);
    sage::ui::Element sq;
    sq.AspectRatio = 1.0f;   // квадрат ВПИСЫВАЕТСЯ в место 400x200
    const glm::vec2 s = sage::ui::ConstrainSize(sq, {400, 200});
    CHECK_NEAR(s.x, 200.0f, 0.01f);
    CHECK_NEAR(s.y, 200.0f, 0.01f);
}

TEST(UIAdaptive_stretched_panel_with_max_size_stays_centred) {
    Scene scene("max");
    GameObject panel = Box(scene, {0, 0}, {10, 10});
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(panel.Entity());
    el.Mode = sage::ui::Element::Stretch::Both;
    el.MaxSize = {400, 0};
    const UIRect r = RectOf(scene, panel.Entity());
    CHECK_NEAR(r.w, 400.0f, 0.01f);
    CHECK_NEAR(r.x, 200.0f, 0.01f);   // (800 - 400) / 2
    CHECK_NEAR(r.h, 600.0f, 0.01f);
}

TEST(UIAdaptive_fill_child_stops_at_its_max_size) {
    Scene scene("row");
    GameObject row = Box(scene, {0, 0}, {600, 50});
    sage::ui::Arrangement arr;
    arr.Mode = sage::ui::Arrangement::Kind::Horizontal;
    arr.Spacing = 0.0f;
    scene.Registry().emplace<sage::ui::Arrangement>(row.Entity(), arr);
    GameObject a = Box(scene, {0, 0}, {50, 50}, row.Entity());
    GameObject b = Box(scene, {0, 0}, {50, 50}, row.Entity());
    scene.Registry().get<sage::ui::Element>(a.Entity()).Grow = 1.0f;
    scene.Registry().get<sage::ui::Element>(a.Entity()).MaxSize = {120, 0};
    scene.Registry().get<sage::ui::Element>(b.Entity()).Grow = 1.0f;
    CHECK_NEAR(RectOf(scene, a.Entity()).w, 120.0f, 0.5f);
    CHECK_TRUE(RectOf(scene, b.Entity()).w > 200.0f);
}

TEST(UIAdaptive_safe_area_moves_anchored_elements_in) {
    Scene scene("safe");
    GameObject corner = Box(scene, {0, 0}, {40, 40});
    scene.Registry().get<sage::ui::Element>(corner.Entity()).SafeArea = true;
    GameObject plain = Box(scene, {0, 0}, {40, 40});
    sage::ui::SetSafeArea({0.1f, 0.1f, 0.1f, 0.1f});
    const UIRect r = RectOf(scene, corner.Entity());
    const UIRect p = RectOf(scene, plain.Entity());
    sage::ui::SetSafeArea(glm::vec4(0.0f));   // глобальное — вернуть до проверок
    CHECK_NEAR(r.x, 80.0f, 0.01f);
    CHECK_NEAR(r.y, 60.0f, 0.01f);
    CHECK_NEAR(p.x, 0.0f, 0.01f);             // без флага — от края экрана
}

TEST(UIAdaptive_settings_survive_saving) {
    Scene scene("save");
    GameObject o = Box(scene, {0, 0}, {40, 40});
    entt::registry& reg = scene.Registry();
    sage::ui::Element& el = reg.get<sage::ui::Element>(o.Entity());
    el.MinSize = {10, 20};
    el.MaxSize = {300, 400};
    el.AspectRatio = 1.5f;
    el.SafeArea = true;
    nlohmann::json j;
    CHECK_TRUE(sage::ui::SaveElement(j, reg, o.Entity()));
    GameObject back = scene.CreateEmptyObject("Копия");
    sage::ui::LoadElement(j, reg, back.Entity());
    const sage::ui::Element& b = reg.get<sage::ui::Element>(back.Entity());
    CHECK_NEAR(b.MinSize.y, 20.0f, 0.01f);
    CHECK_NEAR(b.MaxSize.x, 300.0f, 0.01f);
    CHECK_NEAR(b.AspectRatio, 1.5f, 0.01f);
    CHECK_TRUE(b.SafeArea);
}
