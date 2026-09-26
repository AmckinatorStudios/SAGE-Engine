// Тесты переделки интерфейса: свои виды (Look) у всех частей и состояний,
// файлы стилей, контейнеры отдельно от элементов, девятина один к одному,
// перенос старых сцен и перетаскивание в дереве без потерь. Всё на CPU.
#include "TestFramework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/ui/ImageFit.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UISerialize.h"
#include "sage/ui/UIStyle.h"

namespace fs = std::filesystem;
using sage::ui::UIRect;

namespace {

entt::entity MakeInterface(Scene& scene) {
    GameObject iface = scene.CreateEmptyObject("HUD");
    scene.Registry().emplace<sage::ui::InterfaceComponent>(iface.Entity());
    return iface.Entity();
}

GameObject MakeElement(Scene& scene, const char* name, entt::entity parent, glm::vec2 pos = {0, 0},
                       glm::vec2 size = {100, 40}) {
    GameObject obj = scene.CreateEmptyObject(name);
    sage::ui::Element el;
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    scene.Registry().emplace<sage::ui::Element>(obj.Entity(), el);
    if (parent != entt::null) scene.SetParent(obj.Entity(), parent);
    return obj;
}

const sage::ui::ElementRect* RectOf(const std::vector<sage::ui::ElementRect>& rects, entt::entity e) {
    for (const auto& r : rects)
        if (r.Entity == e) return &r;
    return nullptr;
}

} // namespace

// --- Иерархия: перетаскивание не теряет элемент ------------------------------------

TEST(UISkin_reparent_refuses_own_descendant_and_keeps_the_interface) {
    Scene scene("drag");
    const entt::entity hud = MakeInterface(scene);
    GameObject a = MakeElement(scene, "A", hud);
    GameObject b = MakeElement(scene, "B", a.Entity());
    GameObject c = MakeElement(scene, "C", b.Entity());

    // В самого себя и в своего потомка — отказ, и ничего не сдвинулось.
    CHECK_FALSE(sage::ui::CanReparent(scene, a.Entity(), a.Entity()));
    CHECK_FALSE(sage::ui::CanReparent(scene, a.Entity(), c.Entity()));
    CHECK_FALSE(sage::ui::ReparentElement(scene, a.Entity(), c.Entity()));
    CHECK_TRUE(scene.ParentOf(a.Entity()) == hud);

    // «В корень» — это корень СВОЕГО интерфейса, а не пустота вне всех
    // интерфейсов, где элемент пропадал из дерева редактора.
    CHECK_TRUE(sage::ui::ReparentElement(scene, c.Entity(), entt::null));
    CHECK_TRUE(scene.ParentOf(c.Entity()) == hud);
    CHECK_TRUE(sage::ui::InterfaceOf(scene, c.Entity()) == hud);
    const std::vector<entt::entity> roots = sage::ui::InterfaceRoots(scene, hud);
    CHECK_TRUE(std::find(roots.begin(), roots.end(), c.Entity()) != roots.end());

    // Обычный перенос работает.
    CHECK_TRUE(sage::ui::ReparentElement(scene, c.Entity(), a.Entity()));
    CHECK_TRUE(scene.ParentOf(c.Entity()) == a.Entity());
}

// --- Девятина: углы своего размера при любой высоте ---------------------------------

TEST(UISkin_nine_slice_corners_do_not_follow_the_element_height) {
    // Без заданного размера пикселя угол девятины — один к одному (умноженный
    // только на масштаб холста), а не «по высоте элемента»: раньше рамка 48x48
    // на кнопке высотой 120 раздувала углы в два с половиной раза.
    CHECK_NEAR(sage::ui::SlicedPixelScale(0.0f, false, 1.0f), 1.0f, 1e-6f);
    CHECK_NEAR(sage::ui::SlicedPixelScale(0.0f, false, 2.0f), 2.0f, 1e-6f);
    CHECK_NEAR(sage::ui::SlicedPixelScale(3.0f, false, 1.0f), 3.0f, 1e-6f);
    CHECK_NEAR(sage::ui::SlicedPixelScale(0.0f, true, 1.5f), 1.0f, 1e-6f);

    // И по раскладке кусков: угол 8 пикселей остаётся 8 и у низкой, и у
    // высокой кнопки.
    sage::ui::NineSlice slice;
    slice.SetBorder({8, 8, 8, 8});
    for (float h : {40.0f, 120.0f, 400.0f}) {
        sage::ui::SliceRequest req;
        req.SrcW = 48; req.SrcH = 48;
        req.DstW = 300; req.DstH = h;
        req.Scale = sage::ui::SlicedPixelScale(0.0f, false, 1.0f);
        const auto quads = sage::ui::Solve(slice, req);
        CHECK_EQ((int)quads.size(), 9);
        if (!quads.empty()) {
            CHECK_NEAR(quads[0].DstW, 8.0f, 1e-4f);
            CHECK_NEAR(quads[0].DstH, 8.0f, 1e-4f);
        }
    }
}

// --- Вид (Look): запись, чтение, старые ключи ---------------------------------------

TEST(UISkin_looks_survive_save_and_load) {
    Scene scene("looks");
    GameObject slider = scene.CreateEmptyObject("Slider");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, slider.Entity(), "Slider"));
    auto& reg = scene.Registry();
    sage::ui::Range& r = reg.get<sage::ui::Range>(slider.Entity());
    r.Track.Texture = "ui/track.png";
    r.Track.Fit = (int)sage::ui::Image::Mode::NineSlice;
    r.Track.SliceBorder = {4, 2, 4, 2};
    r.Knob.Texture = "ui/knob.png";
    r.KnobSize = 0.8f;

    GameObject button = scene.CreateEmptyObject("Button");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, button.Entity(), "Button"));
    sage::ui::Interactable& act = reg.get<sage::ui::Interactable>(button.Entity());
    act.UsePressedLook = true;
    act.PressedLook.Texture = "ui/button_down.png";
    act.PressedLook.Fit = (int)sage::ui::Image::Mode::NineSlice;
    act.PressedLook.SliceBorder = {6, 6, 6, 6};
    reg.get<sage::ui::Fill>(button.Entity()).Texture = "ui/button.png";

    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    const auto& br = back->Registry();
    const sage::ui::Range& r2 = br.get<sage::ui::Range>(back->FindByName("Slider").Entity());
    CHECK_TRUE(r2.Track.Texture == "ui/track.png");
    CHECK_EQ(r2.Track.Fit, (int)sage::ui::Image::Mode::NineSlice);
    CHECK_NEAR(r2.Track.SliceBorder.x, 4.0f, 1e-5f);
    CHECK_TRUE(r2.Knob.Texture == "ui/knob.png");
    CHECK_NEAR(r2.KnobSize, 0.8f, 1e-5f);
    const entt::entity b2 = back->FindByName("Button").Entity();
    const sage::ui::Interactable& a2 = br.get<sage::ui::Interactable>(b2);
    CHECK_TRUE(a2.UsePressedLook);
    CHECK_TRUE(a2.PressedLook.Texture == "ui/button_down.png");
    CHECK_NEAR(a2.PressedLook.SliceBorder.w, 6.0f, 1e-5f);
    CHECK_TRUE(br.get<sage::ui::Fill>(b2).Texture == "ui/button.png");
}

TEST(UISkin_old_range_bar_and_layout_keys_are_read) {
    // Сцена, записанная до видов: у ползунка «цвет дорожки» и «акцент», у
    // полосы один цвет заполнения, у раскладки галка «растягивать поперёк».
    const char* text = R"({
      "sage_scene_version": 16, "name": "old", "objects": [
        {"id": 1, "name": "Vol", "noMesh": true, "ui": {
          "element": {"type": "Slider", "size": {"x": 200, "y": 30}},
          "range": {"value": 0.5, "trackColor": {"x": 0.1, "y": 0.2, "z": 0.3, "w": 1},
                    "accentColor": {"x": 0.9, "y": 0.5, "z": 0.1, "w": 1},
                    "borderThickness": 2, "borderColor": {"x": 1, "y": 1, "z": 1, "w": 1}}}},
        {"id": 2, "name": "HP", "noMesh": true, "ui": {
          "element": {"type": "Bar"}, "fill": {},
          "bar": {"value": 0.3, "fillColor": {"x": 0.8, "y": 0.1, "z": 0.1, "w": 1}}}},
        {"id": 3, "name": "Col", "noMesh": true, "ui": {
          "element": {"type": ""}, "layout": {"direction": 1, "stretchCross": false}}}
      ]})";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(scene != nullptr);
    if (!scene) return;
    auto& reg = scene->Registry();
    const sage::ui::Range& r = reg.get<sage::ui::Range>(scene->FindByName("Vol").Entity());
    CHECK_NEAR(r.Track.Color.z, 0.3f, 1e-5f);
    CHECK_NEAR(r.Knob.Color.x, 0.9f, 1e-5f);
    CHECK_NEAR(r.Filled.Color.y, 0.5f, 1e-5f);
    CHECK_NEAR(r.Track.BorderThickness, 2.0f, 1e-5f);

    const entt::entity hp = scene->FindByName("HP").Entity();
    CHECK_NEAR(reg.get<sage::ui::Bar>(hp).Filled.Color.x, 0.8f, 1e-5f);
    // Старое имя типа — к нынешнему.
    CHECK_TRUE(reg.get<sage::ui::Element>(hp).Type == "Progress Bar");

    const entt::entity col = scene->FindByName("Col").Entity();
    CHECK_TRUE(reg.get<sage::ui::Stack>(col).Cross == sage::ui::Stack::CrossAlign::Start);
    CHECK_TRUE(reg.get<sage::ui::Element>(col).Type == "Column");
}

TEST(UISkin_look_fields_show_only_what_acts) {
    // Поля девятины видны только в режиме девятины, а режим — только когда
    // выбрана картинка: цепочкой, а не одним условием.
    sage::ui::Fill fill;
    const sage::ui::PartType* part = sage::ui::FindPart("fill");
    CHECK_TRUE(part != nullptr);
    if (!part) return;
    const auto fields = sage::ui::EditableFields(*part->Fields);
    auto visible = [&](const char* key) {
        for (const auto& f : fields)
            if (std::string(f.Key) == key) return sage::ui::FieldVisible(fields, f, &fill);
        return false;
    };
    fill.Fit = (int)sage::ui::Image::Mode::NineSlice;
    CHECK_FALSE(visible("fit"));          // картинки нет — и режима нет
    CHECK_FALSE(visible("sliceBorder"));  // хотя режим «девятина»
    CHECK_TRUE(visible("rounding"));      // у плашки цветом — скругление
    fill.Texture = "panel.png";
    CHECK_TRUE(visible("fit"));
    CHECK_TRUE(visible("sliceBorder"));
    CHECK_FALSE(visible("rounding"));     // форму задаёт картинка
    fill.Fit = (int)sage::ui::Image::Mode::Normal;
    CHECK_FALSE(visible("sliceBorder"));

    // Вид состояния виден, только когда включён.
    sage::ui::Interactable act;
    const sage::ui::PartType* ip = sage::ui::FindPart("interactable");
    const auto ifields = sage::ui::EditableFields(*ip->Fields);
    auto ivisible = [&](const char* key) {
        for (const auto& f : ifields)
            if (std::string(f.Key) == key) return sage::ui::FieldVisible(ifields, f, &act);
        return false;
    };
    CHECK_FALSE(ivisible("hoverLook.color"));
    CHECK_TRUE(ivisible("hoverBrightness"));
    act.UseHoverLook = true;
    CHECK_TRUE(ivisible("hoverLook.color"));
    CHECK_FALSE(ivisible("hoverBrightness"));
}

// --- Файл стиля ----------------------------------------------------------------------

TEST(UISkin_style_file_changes_look_but_not_content) {
    Scene scene("style");
    auto& reg = scene.Registry();
    GameObject a = scene.CreateEmptyObject("A");
    GameObject b = scene.CreateEmptyObject("B");
    for (GameObject o : {a, b}) {
        reg.emplace<sage::ui::Element>(o.Entity());
        reg.emplace<sage::ui::Fill>(o.Entity());
        reg.emplace<sage::ui::Label>(o.Entity());
    }
    reg.get<sage::ui::Label>(a.Entity()).Text = "Play";
    reg.get<sage::ui::Label>(b.Entity()).Text = "Quit";
    reg.get<sage::ui::Fill>(a.Entity()).Color = {1, 0, 0, 1};
    reg.get<sage::ui::Label>(a.Entity()).Scale = 3.5f;

    const fs::path dir = fs::temp_directory_path() / "sage_style_test";
    fs::create_directories(dir);
    const std::string path = (dir / "red.sageuistyle").string();
    CHECK_TRUE(sage::ui::SaveStyleFile(path, sage::ui::CaptureStyle(reg, a.Entity())));

    // Стиль у B: вид берётся из файла, подпись остаётся своей.
    reg.get<sage::ui::Element>(b.Entity()).Style = path;
    sage::ui::ClearStyleCache();
    sage::ui::ApplyStyles(scene);
    CHECK_NEAR(reg.get<sage::ui::Fill>(b.Entity()).Color.r, 1.0f, 1e-5f);
    CHECK_NEAR(reg.get<sage::ui::Label>(b.Entity()).Scale, 3.5f, 1e-5f);
    CHECK_TRUE(reg.get<sage::ui::Label>(b.Entity()).Text == "Quit");

    // Правка ФАЙЛА доходит до элемента сама, без перезагрузки сцены.
    reg.get<sage::ui::Fill>(a.Entity()).Color = {0, 0, 1, 1};
    CHECK_TRUE(sage::ui::SaveStyleFile(path, sage::ui::CaptureStyle(reg, a.Entity())));
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    sage::ui::ApplyStyles(scene);
    CHECK_NEAR(reg.get<sage::ui::Fill>(b.Entity()).Color.b, 1.0f, 1e-5f);
    CHECK_TRUE(reg.get<sage::ui::Label>(b.Entity()).Text == "Quit");

    std::error_code ec;
    fs::remove_all(dir, ec);
    sage::ui::ClearStyleCache();
}

// --- Контейнеры ------------------------------------------------------------------------

TEST(UISkin_row_grows_wraps_and_centres_across) {
    sage::ui::Stack row;
    row.Direction = sage::ui::Stack::Flow::Horizontal;
    row.Padding = {0, 0, 0, 0};
    row.Spacing = 10.0f;
    row.Cross = sage::ui::Stack::CrossAlign::Center;

    // Рост: 40 + 10 + (остаток) — второй забирает всё свободное место.
    std::vector<sage::ui::LayoutSlot> slots(2);
    slots[0].Size = {40, 20};
    slots[1].Size = {40, 20};
    slots[1].Grow = 1.0f;
    sage::ui::ApplyLayout(row, UIRect{0, 0, 300, 60}, slots);
    CHECK_NEAR(slots[1].Pos.x, 50.0f, 1e-4f);
    CHECK_NEAR(slots[1].Size.x, 250.0f, 1e-4f);
    // Поперёк — по центру: (60 - 20) / 2.
    CHECK_NEAR(slots[0].Pos.y, 20.0f, 1e-4f);
    CHECK_NEAR(slots[0].Size.y, 20.0f, 1e-4f);

    // Перенос: пять по 80 в ширину 300 — три в первой строке, две во второй.
    row.Wrap = true;
    row.Cross = sage::ui::Stack::CrossAlign::Start;
    std::vector<sage::ui::LayoutSlot> tags(5);
    for (auto& t : tags) t.Size = {80, 30};
    const glm::vec2 used = sage::ui::ApplyLayout(row, UIRect{0, 0, 300, 200}, tags);
    CHECK_NEAR(tags[2].Pos.y, 0.0f, 1e-4f);
    CHECK_NEAR(tags[3].Pos.x, 0.0f, 1e-4f);
    CHECK_NEAR(tags[3].Pos.y, 40.0f, 1e-4f);
    CHECK_NEAR(used.y, 70.0f, 1e-4f);

    // Сетка с автоматическим числом столбцов по размеру ячейки.
    sage::ui::Stack grid;
    grid.Direction = sage::ui::Stack::Flow::Grid;
    grid.Padding = {0, 0, 0, 0};
    grid.Spacing = 0.0f;
    grid.Columns = 0;
    grid.CellSize = {50, 50};
    std::vector<sage::ui::LayoutSlot> cells(7);
    for (auto& c : cells) c.Size = {10, 10};
    sage::ui::ApplyLayout(grid, UIRect{0, 0, 210, 400}, cells);
    CHECK_NEAR(cells[3].Pos.x, 150.0f, 1e-4f);  // 4 столбца по 50 в 210
    CHECK_NEAR(cells[4].Pos.y, 50.0f, 1e-4f);
}

TEST(UISkin_child_can_stand_outside_the_row) {
    Scene scene("ignore");
    const entt::entity hud = MakeInterface(scene);
    GameObject row = MakeElement(scene, "Row", hud, {0, 0}, {300, 50});
    sage::ui::Stack st;
    st.Direction = sage::ui::Stack::Flow::Horizontal;
    st.Padding = {0, 0, 0, 0};
    st.Spacing = 0.0f;
    scene.Registry().emplace<sage::ui::Stack>(row.Entity(), st);
    GameObject a = MakeElement(scene, "A", row.Entity(), {0, 0}, {40, 50});
    GameObject badge = MakeElement(scene, "Badge", row.Entity(), {0, 0}, {10, 10});
    GameObject b = MakeElement(scene, "B", row.Entity(), {0, 0}, {40, 50});
    auto& bx = scene.Registry().get<sage::ui::Element>(badge.Entity());
    bx.IgnoreLayout = true;
    bx.Anchor = UIAnchor::TopRight;

    const auto rects = sage::ui::SolveSceneRects(scene, 800, 600, true);
    const auto* ra = RectOf(rects, a.Entity());
    const auto* rb = RectOf(rects, b.Entity());
    const auto* rbadge = RectOf(rects, badge.Entity());
    CHECK_TRUE(ra && rb && rbadge);
    if (!ra || !rb || !rbadge) return;
    // Метка не заняла место в ряду: B сразу за A.
    CHECK_NEAR(rb->Rect.x, 40.0f, 1e-3f);
    // И стоит по своему якорю — в правом верхнем углу ряда.
    CHECK_NEAR(rbadge->Rect.x, 290.0f, 1e-3f);
    CHECK_FALSE(rbadge->InLayout);
}

TEST(UISkin_old_panel_with_a_list_splits_without_moving_anything) {
    // Старая «панель со списком»: подложка, маска и раскладка на одном
    // элементе. Теперь это панель и контейнер внутри неё — а дети стоят там
    // же, где стояли.
    Scene scene("split");
    const entt::entity hud = MakeInterface(scene);
    GameObject list = MakeElement(scene, "List", hud, {20, 30}, {200, 300});
    auto& reg = scene.Registry();
    reg.emplace<sage::ui::Fill>(list.Entity());
    reg.emplace<sage::ui::Mask>(list.Entity());
    sage::ui::Stack st;
    st.Direction = sage::ui::Stack::Flow::Vertical;
    reg.emplace<sage::ui::Stack>(list.Entity(), st);
    reg.get<sage::ui::Element>(list.Entity()).Type = "Vertical List";
    GameObject one = MakeElement(scene, "One", list.Entity(), {0, 0}, {100, 40});
    GameObject two = MakeElement(scene, "Two", list.Entity(), {0, 0}, {100, 40});

    const auto before = sage::ui::SolveSceneRects(scene, 800, 600, true);
    const UIRect oneBefore = RectOf(before, one.Entity())->Rect;
    const UIRect twoBefore = RectOf(before, two.Entity())->Rect;

    CHECK_EQ(sage::ui::NormalizeElements(scene), 1);
    CHECK_FALSE(reg.all_of<sage::ui::Stack>(list.Entity()));
    CHECK_FALSE(reg.all_of<sage::ui::Mask>(list.Entity()));
    CHECK_TRUE(reg.get<sage::ui::Element>(list.Entity()).Type == "Panel");
    const entt::entity box = scene.ParentOf(one.Entity());
    CHECK_TRUE(box != list.Entity());
    CHECK_TRUE(scene.ParentOf(box) == list.Entity());
    CHECK_TRUE(reg.all_of<sage::ui::Stack>(box) && reg.all_of<sage::ui::Mask>(box));
    CHECK_TRUE(reg.get<sage::ui::Element>(box).Type == "Clip Area" ||
               reg.get<sage::ui::Element>(box).Type == "Column");

    const auto after = sage::ui::SolveSceneRects(scene, 800, 600, true);
    const UIRect oneAfter = RectOf(after, one.Entity())->Rect;
    const UIRect twoAfter = RectOf(after, two.Entity())->Rect;
    CHECK_NEAR(oneAfter.x, oneBefore.x, 1e-3f);
    CHECK_NEAR(oneAfter.y, oneBefore.y, 1e-3f);
    CHECK_NEAR(oneAfter.w, oneBefore.w, 1e-3f);
    CHECK_NEAR(twoAfter.y, twoBefore.y, 1e-3f);

    // Второй раз делить нечего.
    CHECK_EQ(sage::ui::NormalizeElements(scene), 0);
}

TEST(UISkin_type_change_keeps_children) {
    // Смена типа в инспекторе не имеет права удалить содержимое: панель со
    // списком внутри, ставшая кнопкой, остаётся со своим списком.
    Scene scene("keep");
    const entt::entity hud = MakeInterface(scene);
    GameObject panel = MakeElement(scene, "Card", hud);
    CHECK_TRUE(sage::ui::ApplyPreset(scene, panel.Entity(), "Panel"));
    GameObject inner = MakeElement(scene, "Inner", panel.Entity());
    CHECK_TRUE(sage::ui::ApplyPreset(scene, panel.Entity(), "Button", /*replaceChildren=*/false));
    CHECK_TRUE(scene.Registry().valid(inner.Entity()));
    CHECK_TRUE(scene.ParentOf(inner.Entity()) == panel.Entity());
    CHECK_TRUE(scene.Registry().all_of<sage::ui::Interactable>(panel.Entity()));
}
