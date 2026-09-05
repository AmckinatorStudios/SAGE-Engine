// ---------------------------------------------------------------------------
// Тесты новой системы интерфейса (engine/src/sage/ui/UIFramework.h).
//
// Всё на процессоре, БЕЗ GL: раскладка, текст, маски, ввод, стили,
// сериализация, префабы и подготовка кадра отделены от рисования именно затем,
// чтобы их можно было проверить так — быстро и без окна.
//
// Шрифт в тестах свой, поддельный: настоящий зависит от машины, а проверять
// надо ПРАВИЛА раскладки (перенос, многоточие, выравнивание), а не начертание.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <cmath>
#include <string>

#include "sage/ui/UIFramework.h"
#include "sage/ui/showcase/UIShowcaseDocument.h"

using namespace sage::ui;

namespace {

// Моноширинный шрифт: ширина любого символа 0.5 кегля, высота строки 1.25.
// С такими числами ожидаемые размеры считаются в уме, и тест проверяет
// раскладку, а не метрики чужого файла.
class FakeFonts : public IUIFontSource {
public:
    int Resolve(const std::string& family, int, bool) override {
        return family == "missing" ? -1 : 0;
    }
    int Fallback() const override { return 0; }
    UIFontMetrics Metrics(int) const override {
        UIFontMetrics m;
        m.LineHeight = 1.25f;
        m.Ascent = 1.0f;
        m.Descent = 0.25f;
        return m;
    }
    float Advance(int, uint32_t) const override { return 0.5f; }
    bool HasGlyph(int, uint32_t cp) const override { return cp != 0x1F600; }
};

class FakeTextures : public IUITextureSource {
public:
    const Texture* Get(const std::string&) override { return nullptr; }
    glm::ivec2 Size(const std::string& path) override {
        return path == "big.png" ? glm::ivec2(200, 100) : glm::ivec2(0);
    }
};

struct Harness {
    UIRuntime rt;
    FakeFonts fonts;
    FakeTextures textures;

    Harness(float w = 1920.0f, float h = 1080.0f) {
        rt.Context().Fonts = &fonts;
        rt.Context().Textures = &textures;
        rt.Context().ScreenPixels = {w, h};
        rt.Doc().Canvas().Scale = UICanvasSettings::ScaleMode::Pixels;
    }
    UIDocument& Doc() { return rt.Doc(); }
    void Step(float dt = 0.016f) { rt.Update(dt); }
    UIRect RectOf(UINodeId id) {
        UIRect r{};
        rt.Layout().RectOf(id, r);
        return r;
    }
};

UINode& Node(UIDocument& doc, const char* name, UINodeId parent = kUIInvalidNode) {
    return *doc.Create(name, parent);
}

} // namespace

// --- Ядро: дерево, компоненты, личность -------------------------------------

TEST(UIx_document_tree_basics) {
    UIDocument doc;
    UINode& root = Node(doc, "Root");
    UINode& a = Node(doc, "A", root.Id);
    UINode& b = Node(doc, "B", root.Id);

    CHECK_EQ(doc.Roots().size(), (size_t)1);
    CHECK_EQ(root.Children.size(), (size_t)2);
    CHECK_EQ(a.Parent, root.Id);
    // Каждый узел получает прямоугольник сразу: узел без него негде рисовать.
    CHECK_TRUE(a.Has<UITransform>());
    CHECK_TRUE(!a.Guid.empty());
    CHECK_TRUE(a.Guid != b.Guid);

    CHECK_EQ(doc.FindByPath("Root/B"), &b);
    CHECK_EQ(doc.PathOf(b.Id), std::string("Root/B"));

    doc.Destroy(a.Id);
    CHECK_EQ(root.Children.size(), (size_t)1);
    CHECK_EQ(doc.Find(a.Id), (UINode*)nullptr);
}

TEST(UIx_reparent_rejects_cycles) {
    UIDocument doc;
    UINode& root = Node(doc, "Root");
    UINode& child = Node(doc, "Child", root.Id);
    UINode& grand = Node(doc, "Grand", child.Id);

    // Родитель внутри собственного поддерева — бесконечный обход при первой же
    // отрисовке, поэтому это отказ, а не «как-нибудь разберётся».
    CHECK_FALSE(doc.Reparent(root.Id, grand.Id));
    CHECK_FALSE(doc.Reparent(root.Id, root.Id));
    CHECK_TRUE(doc.Reparent(grand.Id, root.Id));
    CHECK_EQ(grand.Parent, root.Id);
}

TEST(UIx_essential_component_cannot_be_removed) {
    UIDocument doc;
    UINode& n = Node(doc, "N");
    CHECK_FALSE(n.RemoveById("transform"));
    CHECK_TRUE(n.Has<UITransform>());
    n.Ensure<UIFill>();
    CHECK_TRUE(n.RemoveById("fill"));
    CHECK_FALSE(n.Has<UIFill>());
}

TEST(UIx_duplicate_copies_all_components) {
    UIDocument doc;
    UINode& src = Node(doc, "Src");
    src.Ensure<UIFill>().Color = UIColor(0.1f, 0.2f, 0.3f, 1.0f);
    src.Ensure<UIText>().Text = "hello";
    Node(doc, "Kid", src.Id).Ensure<UIImage>().Path = "a.png";

    const UINodeId copyId = doc.Duplicate(src.Id);
    UINode* copy = doc.Find(copyId);
    CHECK_TRUE(copy != nullptr);
    // Копирование идёт по Clone каждого компонента, а не по списку «что
    // копировать»: списка нет, поэтому он не может устареть.
    CHECK_NEAR(copy->Get<UIFill>()->Color.b, 0.3f, 1e-5);
    CHECK_EQ(copy->Get<UIText>()->Text, std::string("hello"));
    CHECK_EQ(copy->Children.size(), (size_t)1);
    CHECK_EQ(doc.Find(copy->Children[0])->Get<UIImage>()->Path, std::string("a.png"));
}

// --- Раскладка ---------------------------------------------------------------

TEST(UIx_layout_anchor_point) {
    UIRect parent{0, 0, 1000, 500};
    UITransform t;
    t.Size = {100.0f, 50.0f};

    t.SetAnchorPoint(UIAnchor::TopLeft);
    UIRect r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, 0.0, 1e-4);
    CHECK_NEAR(r.y, 0.0, 1e-4);

    t.SetAnchorPoint(UIAnchor::Center);
    r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, 450.0, 1e-4);
    CHECK_NEAR(r.y, 225.0, 1e-4);

    t.SetAnchorPoint(UIAnchor::BottomRight);
    r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, 900.0, 1e-4);
    CHECK_NEAR(r.y, 450.0, 1e-4);
}

TEST(UIx_layout_arbitrary_anchor) {
    // Якорь — доли, а не одно из девяти значений: (0.25, 0.75) законен.
    UIRect parent{0, 0, 800, 400};
    UITransform t;
    t.AnchorMin = t.AnchorMax = {0.25f, 0.75f};
    t.Pivot = {0.5f, 0.5f};
    t.Size = {40.0f, 20.0f};
    const UIRect r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, 200.0 - 20.0, 1e-4);
    CHECK_NEAR(r.y, 300.0 - 10.0, 1e-4);
}

TEST(UIx_layout_stretch_and_margins) {
    UIRect parent{0, 0, 1000, 600};
    UITransform t;
    t.SetStretch(true, true);
    t.Margin = UIEdges(20.0f, 10.0f, 30.0f, 40.0f);
    const UIRect r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, 20.0, 1e-4);
    CHECK_NEAR(r.y, 10.0, 1e-4);
    CHECK_NEAR(r.w, 950.0, 1e-4);
    CHECK_NEAR(r.h, 550.0, 1e-4);
}

TEST(UIx_layout_percent_min_max_aspect) {
    UIRect parent{0, 0, 1000, 600};
    UITransform t;
    t.WidthMode = UISizeMode::Percent;
    t.HeightMode = UISizeMode::Percent;
    t.Percent = {50.0f, 25.0f};
    UIRect r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.w, 500.0, 1e-4);
    CHECK_NEAR(r.h, 150.0, 1e-4);

    t.MaxSize = {300.0f, 0.0f};
    r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.w, 300.0, 1e-4);

    t.MaxSize = {0.0f, 0.0f};
    t.MinSize = {700.0f, 0.0f};
    r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.w, 700.0, 1e-4);

    t.MinSize = {0.0f, 0.0f};
    t.AspectRatio = 2.0f;
    t.Aspect = UIAspectMode::HeightFromWidth;
    r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.h, 250.0, 1e-4);
}

TEST(UIx_offset_roundtrip) {
    // Обратная задача обязана быть ТОЧНЫМ обратным ходом: редактор тянет
    // прямоугольник мышью, а записывает Offset.
    UIRect parent{0, 0, 900, 500};
    UITransform t;
    t.SetAnchorPoint(UIAnchor::BottomRight);
    t.Size = {120.0f, 60.0f};
    const glm::vec2 want{613.0f, 271.0f};
    t.Offset = UIOffsetForTopLeft(t, want, t.Size, parent);
    const UIRect r = UIResolveTransform(t, parent, {});
    CHECK_NEAR(r.x, want.x, 1e-3);
    CHECK_NEAR(r.y, want.y, 1e-3);
}

TEST(UIx_layout_vertical_container) {
    Harness h;
    UINode& panel = Node(h.Doc(), "Panel");
    UITransform& pt = panel.Ensure<UITransform>();
    pt.Size = {200.0f, 400.0f};
    UILayout& layout = panel.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Vertical;
    layout.Padding = UIEdges::Uniform(10.0f);
    layout.Gap = {0.0f, 6.0f};

    UINodeId ids[3];
    for (int i = 0; i < 3; ++i) {
        UINode& kid = Node(h.Doc(), "Kid", panel.Id);
        kid.Ensure<UITransform>().Size = {50.0f, 30.0f};
        ids[i] = kid.Id;
    }
    h.Step();

    const UIRect a = h.RectOf(ids[0]);
    const UIRect b = h.RectOf(ids[1]);
    const UIRect c = h.RectOf(ids[2]);
    CHECK_NEAR(a.y, 10.0, 1e-3);
    CHECK_NEAR(b.y, 46.0, 1e-3);  // 10 + 30 + 6
    CHECK_NEAR(c.y, 82.0, 1e-3);
    CHECK_NEAR(a.x, 10.0, 1e-3);
}

TEST(UIx_layout_horizontal_stretch_shares_free_space) {
    Harness h;
    UINode& row = Node(h.Doc(), "Row");
    row.Ensure<UITransform>().Size = {400.0f, 60.0f};
    UILayout& layout = row.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Horizontal;
    layout.Padding = UIEdges::Uniform(0.0f);
    layout.Gap = {0.0f, 0.0f};

    UINode& fixed = Node(h.Doc(), "Fixed", row.Id);
    fixed.Ensure<UITransform>().Size = {100.0f, 40.0f};
    UINode& grow1 = Node(h.Doc(), "Grow1", row.Id);
    grow1.Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    UINode& grow2 = Node(h.Doc(), "Grow2", row.Id);
    grow2.Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    h.Step();

    // Растягивающиеся делят ОСТАТОК, а не весь контейнер: иначе фиксированный
    // сосед выдавливается за край.
    CHECK_NEAR(h.RectOf(grow1.Id).w, 150.0, 1e-3);
    CHECK_NEAR(h.RectOf(grow2.Id).w, 150.0, 1e-3);
    CHECK_NEAR(h.RectOf(grow2.Id).x, 250.0, 1e-3);
}

TEST(UIx_layout_grid_and_wrap) {
    Harness h;
    UINode& grid = Node(h.Doc(), "Grid");
    grid.Ensure<UITransform>().Size = {320.0f, 300.0f};
    UILayout& gl = grid.Ensure<UILayout>();
    gl.Kind = UILayout::Mode::Grid;
    gl.Columns = 2;
    gl.Padding = UIEdges::Uniform(0.0f);
    gl.Gap = {20.0f, 20.0f};
    gl.CellSize = {100.0f, 50.0f};

    UINodeId ids[4];
    for (int i = 0; i < 4; ++i) {
        UINode& kid = Node(h.Doc(), "Cell", grid.Id);
        kid.Ensure<UITransform>().Size = {100.0f, 50.0f};
        ids[i] = kid.Id;
    }
    h.Step();
    CHECK_NEAR(h.RectOf(ids[0]).x, 0.0, 1e-3);
    CHECK_NEAR(h.RectOf(ids[1]).x, 120.0, 1e-3);
    CHECK_NEAR(h.RectOf(ids[2]).y, 70.0, 1e-3);
    CHECK_NEAR(h.RectOf(ids[3]).x, 120.0, 1e-3);
}

TEST(UIx_layout_fit_content_nested) {
    Harness h;
    // Панель по содержимому, внутри которой строка по содержимому: без прохода
    // измерения снизу вверх этот случай не считается вовсе.
    UINode& panel = Node(h.Doc(), "Panel");
    UITransform& pt = panel.Ensure<UITransform>();
    pt.WidthMode = UISizeMode::Content;
    pt.HeightMode = UISizeMode::Content;
    UILayout& pl = panel.Ensure<UILayout>();
    pl.Kind = UILayout::Mode::Vertical;
    pl.FitWidth = pl.FitHeight = true;
    pl.Padding = UIEdges::Uniform(8.0f);
    pl.Gap = {0.0f, 4.0f};

    for (int i = 0; i < 2; ++i) {
        UINode& kid = Node(h.Doc(), "Kid", panel.Id);
        kid.Ensure<UITransform>().Size = {120.0f, 20.0f};
    }
    h.Step();
    const UIRect r = h.RectOf(panel.Id);
    CHECK_NEAR(r.w, 136.0, 1e-3);          // 120 + 8 + 8
    CHECK_NEAR(r.h, 8.0 + 20 + 4 + 20 + 8, 1e-3);
}

TEST(UIx_layout_ignore_layout_escapes_container) {
    Harness h;
    UINode& row = Node(h.Doc(), "Row");
    row.Ensure<UITransform>().Size = {300.0f, 100.0f};
    UILayout& layout = row.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Horizontal;
    layout.Padding = UIEdges::Uniform(0.0f);

    UINode& managed = Node(h.Doc(), "Managed", row.Id);
    managed.Ensure<UITransform>().Size = {40.0f, 40.0f};
    UINode& overlay = Node(h.Doc(), "Overlay", row.Id);
    UITransform& ot = overlay.Ensure<UITransform>();
    ot.IgnoreLayout = true;
    ot.AnchorMin = ot.AnchorMax = {1.0f, 0.0f};
    ot.Pivot = {1.0f, 0.0f};
    ot.Size = {20.0f, 20.0f};
    h.Step();

    CHECK_NEAR(h.RectOf(managed.Id).x, 0.0, 1e-3);
    // Оверлей встал по якорю и НЕ сдвинул соседа.
    CHECK_NEAR(h.RectOf(overlay.Id).x, 280.0, 1e-3);
}

TEST(UIx_canvas_scale_maps_logical_to_pixels) {
    Harness h(3840.0f, 2160.0f);
    h.Doc().Canvas().Scale = UICanvasSettings::ScaleMode::ScaleWithSize;
    h.Doc().Canvas().Reference = {1920.0f, 1080.0f};
    UINode& n = Node(h.Doc(), "N");
    n.Ensure<UITransform>().Size = {100.0f, 50.0f};
    h.Step();
    // Вёрстка в логических единицах, наружу — пиксели: 2x экран, 2x размер.
    CHECK_NEAR(h.RectOf(n.Id).w, 200.0, 1e-3);
    CHECK_NEAR(h.RectOf(n.Id).h, 100.0, 1e-3);
}

TEST(UIx_safe_area_shrinks_viewport) {
    Harness h(1000.0f, 800.0f);
    UICanvasSettings& c = h.Doc().Canvas();
    c.RespectSafeArea = true;
    c.SafeArea = UIEdges(40.0f, 20.0f, 40.0f, 0.0f);
    UINode& n = Node(h.Doc(), "N");
    n.Ensure<UITransform>().SetStretch(true, true);
    h.Step();
    const UIRect r = h.RectOf(n.Id);
    CHECK_NEAR(r.x, 40.0, 1e-3);
    CHECK_NEAR(r.w, 920.0, 1e-3);
}

// --- Текст -------------------------------------------------------------------

TEST(UIx_text_wraps_by_words) {
    Harness h;
    UIText t;
    t.Text = "aaa bbb ccc";
    t.Size = 10.0f;              // символ 5 пикселей
    t.Wrap = UITextWrap::Word;
    // "aaa bbb" = 7 символов = 35 пикселей; третье слово не влезает в 40.
    const UITextLayoutResult r = UILayoutText(h.rt.Context(), t, 40.0f, 0.0f);
    CHECK_EQ(r.Lines.size(), (size_t)2);
}

TEST(UIx_text_respects_explicit_newlines) {
    Harness h;
    UIText t;
    t.Text = "one\ntwo\nthree";
    t.Size = 10.0f;
    t.Wrap = UITextWrap::None;
    const UITextLayoutResult r = UILayoutText(h.rt.Context(), t, 0.0f, 0.0f);
    CHECK_EQ(r.Lines.size(), (size_t)3);
}

TEST(UIx_text_ellipsis_and_max_lines) {
    Harness h;
    UIText t;
    t.Text = "aaaa bbbb cccc dddd eeee";
    t.Size = 10.0f;
    t.Wrap = UITextWrap::Word;
    t.MaxLines = 2;
    t.Overflow = UITextOverflow::Ellipsis;
    const UITextLayoutResult r = UILayoutText(h.rt.Context(), t, 50.0f, 0.0f);
    CHECK_EQ(r.Lines.size(), (size_t)2);
    CHECK_TRUE(r.Truncated);
}

TEST(UIx_text_alignment) {
    Harness h;
    UIText t;
    t.Text = "ab";       // 2 символа = 10 пикселей при кегле 10
    t.Size = 10.0f;
    t.Wrap = UITextWrap::None;

    t.Align = UITextAlign::Left;
    UITextLayoutResult r = UILayoutText(h.rt.Context(), t, 100.0f, 0.0f);
    CHECK_NEAR(r.Glyphs.front().X, 0.0, 1e-3);

    t.Align = UITextAlign::Center;
    r = UILayoutText(h.rt.Context(), t, 100.0f, 0.0f);
    CHECK_NEAR(r.Glyphs.front().X, 45.0, 1e-3);

    t.Align = UITextAlign::Right;
    r = UILayoutText(h.rt.Context(), t, 100.0f, 0.0f);
    CHECK_NEAR(r.Glyphs.front().X, 90.0, 1e-3);
}

TEST(UIx_text_autosize_fits_box) {
    Harness h;
    UIText t;
    t.Text = "abcdefghij"; // 10 символов
    t.Size = 40.0f;
    t.AutoSize = true;
    t.MinSize = 4.0f;
    t.MaxSize = 40.0f;
    t.Wrap = UITextWrap::None;
    const UITextLayoutResult r = UILayoutText(h.rt.Context(), t, 50.0f, 0.0f);
    // 10 символов по 0.5 кегля должны уместиться в 50 → кегль около 10.
    CHECK_TRUE(r.FontSize <= 10.5f);
    CHECK_TRUE(r.FontSize > 8.0f);
    CHECK_TRUE(r.Size.x <= 50.5f);
}

TEST(UIx_text_measures_container_by_content) {
    Harness h;
    UINode& n = Node(h.Doc(), "Label");
    UITransform& t = n.Ensure<UITransform>();
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIText& text = n.Ensure<UIText>();
    text.Text = "abcd";
    text.Size = 20.0f;
    text.Wrap = UITextWrap::None;
    h.Step();
    // 4 символа × 0.5 × 20 = 40
    CHECK_NEAR(h.RectOf(n.Id).w, 40.0, 1e-3);
    CHECK_NEAR(h.RectOf(n.Id).h, 25.0, 1e-3); // 1.25 × 20
}

TEST(UIx_text_localization_key_and_fallback) {
    Harness h;
    h.rt.Context().Localize = [](const std::string& key) {
        return key == "menu.play" ? std::string("Играть") : key;
    };
    UIText t;
    t.Key = "menu.play";
    t.Text = "Play";
    CHECK_EQ(t.Resolve(h.rt.Context()), std::string("Играть"));

    // Ключа нет в словаре — остаётся запасной текст, а не сырой ключ на экране.
    t.Key = "menu.missing";
    CHECK_EQ(t.Resolve(h.rt.Context()), std::string("Play"));
}

TEST(UIx_utf8_helpers) {
    const std::string s = "Привет";
    CHECK_EQ(UIUtf8Length(s), 6);
    int i = 0;
    const uint32_t first = UIUtf8Next(s, i);
    CHECK_EQ((int)first, 0x041F);
    CHECK_EQ(i, 2);
    CHECK_EQ(UIUtf8Prev(s, 2), 0);
    std::string out;
    UIUtf8Append(out, 0x0416);
    CHECK_EQ(UIUtf8Length(out), 1);
}

// --- Прозрачность, слои, порядок ---------------------------------------------

TEST(UIx_opacity_multiplies_through_hierarchy) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Opacity = 0.5f;
    root.Ensure<UITransform>().SetStretch(true, true);
    UINode& child = Node(h.Doc(), "Child", root.Id);
    child.Opacity = 0.5f;
    h.Step();
    CHECK_NEAR(h.rt.Layout().Get(child.Id)->Opacity, 0.25, 1e-4);
}

TEST(UIx_layer_then_order_then_tree) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Ensure<UITransform>().SetStretch(true, true);
    UINode& low = Node(h.Doc(), "Low", root.Id);
    low.Layer = 0;
    UINode& high = Node(h.Doc(), "High", root.Id);
    high.Layer = 10;
    UINode& mid = Node(h.Doc(), "Mid", root.Id);
    mid.Layer = 0;
    mid.Order = 5;
    h.Step();

    const uint64_t lowKey = h.rt.Layout().Get(low.Id)->SortKey;
    const uint64_t midKey = h.rt.Layout().Get(mid.Id)->SortKey;
    const uint64_t highKey = h.rt.Layout().Get(high.Id)->SortKey;
    CHECK_TRUE(lowKey < midKey);
    CHECK_TRUE(midKey < highKey);
}

TEST(UIx_disabled_subtree_leaves_layout) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Enabled = false;
    Node(h.Doc(), "Child", root.Id);
    h.Step();
    CHECK_EQ(h.rt.Layout().Nodes().size(), (size_t)0);
}

// --- Маски -------------------------------------------------------------------

TEST(UIx_mask_clips_subtree) {
    Harness h;
    UINode& panel = Node(h.Doc(), "Panel");
    UITransform& pt = panel.Ensure<UITransform>();
    pt.Size = {100.0f, 100.0f};
    panel.Ensure<UIMask>();
    UINode& child = Node(h.Doc(), "Child", panel.Id);
    child.Ensure<UITransform>().Size = {200.0f, 200.0f};
    h.Step();

    const UIResolvedNode* r = h.rt.Layout().Get(child.Id);
    CHECK_TRUE(r->Clipped);
    CHECK_NEAR(r->Clip.w, 100.0, 1e-3);
    // Сама панель НЕ обрезана собственной маской: маска действует на потомков.
    CHECK_FALSE(h.rt.Layout().Get(panel.Id)->Clipped);
}

TEST(UIx_nested_masks_intersect) {
    Harness h;
    UINode& outer = Node(h.Doc(), "Outer");
    outer.Ensure<UITransform>().Size = {200.0f, 200.0f};
    outer.Ensure<UIMask>();

    UINode& inner = Node(h.Doc(), "Inner", outer.Id);
    UITransform& it = inner.Ensure<UITransform>();
    it.Offset = {50.0f, 50.0f};
    it.Size = {200.0f, 200.0f};
    inner.Ensure<UIMask>();

    UINode& leaf = Node(h.Doc(), "Leaf", inner.Id);
    leaf.Ensure<UITransform>().Size = {400.0f, 400.0f};
    h.Step();

    // A ∩ B: от 50 до 200.
    const UIRect clip = h.rt.Layout().Get(leaf.Id)->Clip;
    CHECK_NEAR(clip.x, 50.0, 1e-3);
    CHECK_NEAR(clip.w, 150.0, 1e-3);
}

TEST(UIx_mask_value_is_continuous_not_binary) {
    UIMaskEntry e;
    e.Form = UIMask::Shape::Rect;
    e.Rect = {0.0f, 0.0f, 100.0f, 100.0f};
    e.Softness = 10.0f;
    // Мягкая маска — ЗНАЧЕНИЕ 0..1, а не «видно/не видно»: отсюда мягкие края.
    const float inside = UIMaskValue(e, {50.0f, 50.0f});
    const float edge = UIMaskValue(e, {100.0f, 50.0f});
    const float outside = UIMaskValue(e, {130.0f, 50.0f});
    CHECK_NEAR(inside, 1.0, 1e-3);
    CHECK_TRUE(edge > 0.2f && edge < 0.8f);
    CHECK_NEAR(outside, 0.0, 1e-3);
}

TEST(UIx_mask_shapes_and_invert) {
    UIMaskEntry e;
    e.Form = UIMask::Shape::Ellipse;
    e.Rect = {0.0f, 0.0f, 100.0f, 100.0f};
    CHECK_NEAR(UIMaskValue(e, {50.0f, 50.0f}), 1.0, 1e-3);
    CHECK_NEAR(UIMaskValue(e, {4.0f, 4.0f}), 0.0, 1e-3); // угол вне круга

    e.Invert = true;
    CHECK_NEAR(UIMaskValue(e, {50.0f, 50.0f}), 0.0, 1e-3);
}

TEST(UIx_mask_compose_subtract) {
    UIMaskStack stack;
    const int root = stack.Root();
    UIMaskEntry hole;
    hole.Form = UIMask::Shape::Rect;
    hole.Rect = {40.0f, 40.0f, 20.0f, 20.0f};
    hole.Mode = UIMask::Compose::Subtract;
    const int state = stack.Push(root, hole);
    CHECK_NEAR(stack.Sample(state, {50.0f, 50.0f}, false), 0.0, 1e-3); // в дырке
    CHECK_NEAR(stack.Sample(state, {10.0f, 10.0f}, false), 1.0, 1e-3); // вне дырки
}

// --- Попадание курсором и ввод ------------------------------------------------

TEST(UIx_hit_test_topmost_by_order) {
    Harness h;
    UINode& a = Node(h.Doc(), "A");
    a.Ensure<UITransform>().Size = {100.0f, 100.0f};
    a.Ensure<UIInteraction>();
    UINode& b = Node(h.Doc(), "B");
    b.Ensure<UITransform>().Size = {100.0f, 100.0f};
    b.Ensure<UIInteraction>();
    b.Layer = 5;
    h.Step();

    const UIHitResult hit = UIHitTest(h.Doc(), h.rt.Layout(), h.rt.Context(), {50.0f, 50.0f});
    CHECK_EQ(hit.Node, b.Id);
}

TEST(UIx_hit_test_respects_alpha_and_mask) {
    Harness h;
    UINode& panel = Node(h.Doc(), "Panel");
    panel.Ensure<UITransform>().Size = {100.0f, 100.0f};
    panel.Ensure<UIMask>();
    UINode& child = Node(h.Doc(), "Child", panel.Id);
    child.Ensure<UITransform>().Size = {300.0f, 300.0f};
    child.Ensure<UIInteraction>();
    h.Step();

    CHECK_TRUE(UIHitNode(h.Doc(), h.rt.Layout(), h.rt.Context(), child.Id, {50.0f, 50.0f}));
    // Точка внутри ребёнка, но ЗА маской родителя — попадания нет.
    CHECK_FALSE(UIHitNode(h.Doc(), h.rt.Layout(), h.rt.Context(), child.Id, {200.0f, 50.0f}));

    // Полностью прозрачный узел мышь не ловит.
    child.Opacity = 0.0f;
    h.Step();
    CHECK_FALSE(UIHitNode(h.Doc(), h.rt.Layout(), h.rt.Context(), child.Id, {50.0f, 50.0f}));
}

TEST(UIx_hit_test_rounded_and_ellipse) {
    Harness h;
    UINode& n = Node(h.Doc(), "Round");
    n.Ensure<UITransform>().Size = {100.0f, 100.0f};
    n.Ensure<UIFill>().Radius = UICorners(50.0f);
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    h.Step();
    // Центр — попадание, дальний угол круга радиуса 50 — нет.
    CHECK_TRUE(UIHitNode(h.Doc(), h.rt.Layout(), h.rt.Context(), n.Id, {50.0f, 50.0f}));
    CHECK_FALSE(UIHitNode(h.Doc(), h.rt.Layout(), h.rt.Context(), n.Id, {2.0f, 2.0f}));
}

TEST(UIx_click_reports_command_not_action) {
    Harness h;
    UINode& button = Node(h.Doc(), "Button");
    button.Ensure<UITransform>().Size = {100.0f, 40.0f};
    UIInteraction& ia = button.Ensure<UIInteraction>();
    ia.Command = "menu.play";
    h.Step();

    UIInputFrame in;
    in.Pointer = {50.0f, 20.0f};
    in.Buttons[0] = true;
    h.rt.HandleInput(in);
    in.Buttons[0] = false;
    const UIInputResult r = h.rt.HandleInput(in);

    // Интерфейс сообщает КОМАНДУ, а не выполняет действие: что она значит,
    // решает игра снаружи.
    CHECK_EQ(r.Commands.size(), (size_t)1);
    CHECK_EQ(r.Commands[0], std::string("menu.play"));
    CHECK_TRUE(button.Get<UIInteraction>()->Runtime.Clicked);
}

TEST(UIx_event_propagation_capture_target_bubble) {
    Harness h;
    UINode& panel = Node(h.Doc(), "Panel");
    panel.Ensure<UITransform>().Size = {200.0f, 200.0f};
    UINode& button = Node(h.Doc(), "Button", panel.Id);
    button.Ensure<UITransform>().Size = {100.0f, 40.0f};
    button.Ensure<UIInteraction>();
    h.Step();

    std::string trace;
    h.rt.Events().OnAny([&](UIEvent& e) {
        if (e.Type != UIEventType::Click) return;
        const char* phase = e.Phase == UIEventPhase::Capture   ? "C"
                            : e.Phase == UIEventPhase::Target ? "T"
                                                              : "B";
        trace += phase;
    });

    UIInputFrame in;
    in.Pointer = {50.0f, 20.0f};
    in.Buttons[0] = true;
    h.rt.HandleInput(in);
    in.Buttons[0] = false;
    h.rt.HandleInput(in);
    // Панель (перехват) → кнопка (цель) → панель (всплытие).
    CHECK_EQ(trace, std::string("CTB"));
}

TEST(UIx_focus_and_tab_navigation) {
    Harness h;
    UINodeId ids[3];
    for (int i = 0; i < 3; ++i) {
        UINode& n = Node(h.Doc(), "Focusable");
        UITransform& t = n.Ensure<UITransform>();
        t.Offset = {0.0f, (float)i * 50.0f};
        t.Size = {100.0f, 40.0f};
        UIInteraction& ia = n.Ensure<UIInteraction>();
        ia.Focusable = true;
        ids[i] = n.Id;
    }
    h.Step();

    UIInputFrame in;
    in.NavNext = true;
    h.rt.HandleInput(in);
    CHECK_EQ(h.rt.Input().Focused(), ids[0]);
    h.rt.HandleInput(in);
    CHECK_EQ(h.rt.Input().Focused(), ids[1]);

    UIInputFrame down;
    down.NavY = 1;
    h.rt.HandleInput(down);
    CHECK_EQ(h.rt.Input().Focused(), ids[2]);
}

TEST(UIx_text_field_typing_and_backspace) {
    Harness h;
    UINode& field = Node(h.Doc(), "Field");
    field.Ensure<UITransform>().Size = {200.0f, 32.0f};
    field.Ensure<UITextField>();
    UIInteraction& ia = field.Ensure<UIInteraction>();
    ia.Focusable = true;
    h.Step();

    UIInputFrame click;
    click.Pointer = {50.0f, 16.0f};
    click.Buttons[0] = true;
    h.rt.HandleInput(click);
    click.Buttons[0] = false;
    h.rt.HandleInput(click);

    UIInputFrame typing;
    typing.TextInput = "Да";
    h.rt.HandleInput(typing);
    CHECK_EQ(field.Get<UITextField>()->Value, std::string("Да"));

    UIInputFrame back;
    back.KeysDown.push_back(259); // backspace
    h.rt.HandleInput(back);
    // Удаляется СИМВОЛ, а не байт: иначе кириллица рвётся пополам.
    CHECK_EQ(field.Get<UITextField>()->Value, std::string("Д"));
}

TEST(UIx_slider_drag_changes_value_only) {
    Harness h;
    UINode& slider = Node(h.Doc(), "Slider");
    slider.Ensure<UITransform>().Size = {200.0f, 20.0f};
    UIRangeValue& range = slider.Ensure<UIRangeValue>();
    range.Value = 0.0f;
    UIInteraction& ia = slider.Ensure<UIInteraction>();
    ia.Draggable = true;
    h.Step();

    UIInputFrame in;
    in.Pointer = {150.0f, 10.0f};
    in.Buttons[0] = true;
    h.rt.HandleInput(in);
    CHECK_NEAR(slider.Get<UIRangeValue>()->Value, 0.75, 1e-3);
}

TEST(UIx_range_step_snapping) {
    UIRangeValue r;
    r.Min = 0.0f;
    r.Max = 100.0f;
    r.Step = 25.0f;
    r.SetNormalized(0.42f);
    CHECK_NEAR(r.Value, 50.0, 1e-3);
    r.SetNormalized(0.1f);
    CHECK_NEAR(r.Value, 0.0, 1e-3);
}

// --- Стили и тема ------------------------------------------------------------

TEST(UIx_theme_applies_and_respects_overrides) {
    UIDocument doc;
    UITheme theme = UITheme::Default();
    UINode& n = Node(doc, "Button");
    n.Ensure<UIFill>().Color = UIColor(1.0f, 0.0f, 0.0f, 1.0f);
    UIStyled& styled = n.Ensure<UIStyled>();
    styled.Style = "Button";

    theme.Apply(doc);
    // Тема положила свой цвет.
    CHECK_FALSE(n.Get<UIFill>()->Color.r == 1.0f && n.Get<UIFill>()->Color.g == 0.0f);

    // Локальная правка сильнее темы (§59, пункт 5).
    n.Get<UIFill>()->Color = UIColor(1.0f, 0.0f, 0.0f, 1.0f);
    styled.SetOverride("fill.Color", true);
    theme.Apply(doc);
    CHECK_NEAR(n.Get<UIFill>()->Color.r, 1.0, 1e-4);
    CHECK_NEAR(n.Get<UIFill>()->Color.g, 0.0, 1e-4);
}

TEST(UIx_theme_state_styles) {
    UIDocument doc;
    UITheme theme = UITheme::Default();
    UINode& n = Node(doc, "Button");
    n.Ensure<UIStyled>().Style = "Button";
    n.Ensure<UIFill>();
    n.Ensure<UIInteraction>();

    theme.ApplyTo(n, UIState_Normal);
    const UIColor normal = n.Get<UIFill>()->Color;
    theme.ApplyTo(n, UIState_Hovered);
    const UIColor hovered = n.Get<UIFill>()->Color;
    CHECK_TRUE(normal != hovered);
}

TEST(UIx_theme_style_inheritance) {
    UITheme theme = UITheme::Default();
    UIDocument doc;
    UINode& n = Node(doc, "Primary");
    n.Ensure<UIStyled>().Style = "ButtonPrimary";
    n.Ensure<UIFill>();
    n.Ensure<UIBorder>();
    theme.ApplyTo(n, UIState_Normal);
    // Своё значение производного стиля перекрывает базовое...
    CHECK_NEAR(n.Get<UIFill>()->Color.r, theme.Tokens.Color("Color.Accent").r, 1e-4);
    // ...а не заданное в нём наследуется от базового.
    CHECK_TRUE(n.Get<UIBorder>()->Thickness.L > 0.0f);
}

TEST(UIx_design_tokens) {
    const UIDesignTokens t = UIDesignTokens::Default();
    CHECK_TRUE(t.Has("Spacing.Medium"));
    CHECK_NEAR(t.Number("Spacing.Medium"), 16.0, 1e-4);
    CHECK_NEAR(t.Number("Missing.Token", 7.0f), 7.0, 1e-4);
}

TEST(UIx_color_hex_roundtrip) {
    const UIColor c = UIColorFromHex("#3366CCFF");
    CHECK_NEAR(c.r, 0.2, 0.01);
    CHECK_NEAR(c.b, 0.8, 0.01);
    CHECK_EQ(UIColorToHex(c), std::string("#3366CCFF"));
    // Мусор даёт запасной цвет, а не чёрный: «не разобралось» обязано быть видно.
    const UIColor fallback = UIColorFromHex("zzz", UIColor(1.0f, 0.0f, 1.0f, 1.0f));
    CHECK_NEAR(fallback.g, 0.0, 1e-4);
    CHECK_NEAR(fallback.b, 1.0, 1e-4);
}

// --- Эффекты -----------------------------------------------------------------

TEST(UIx_effects_stack_order_and_cost) {
    UIDocument doc;
    UINode& n = Node(doc, "Card");
    UIEffects& fx = n.Ensure<UIEffects>();
    fx.Ensure<UIDropShadow>();
    fx.Ensure<UIGlow>();
    CHECK_EQ(fx.Items.size(), (size_t)2);
    // Ни один из них не требует промежуточной цели — значит, ноль лишних
    // проходов (§131).
    CHECK_FALSE(fx.NeedsOffscreen());

    fx.Ensure<UIBlur>();
    CHECK_TRUE(fx.NeedsOffscreen());

    // Порядок значим и его можно менять.
    fx.MoveItem(2, 0);
    CHECK_EQ(fx.Items[0]->Type().Id, std::string("blur"));
}

TEST(UIx_no_effects_means_no_cost) {
    Harness h;
    UINode& n = Node(h.Doc(), "Plain");
    n.Ensure<UITransform>().Size = {100.0f, 100.0f};
    n.Ensure<UIFill>();
    h.Step();
    h.rt.Build();
    // Простая заливка — ровно одна команда: ни целей, ни проходов эффектов.
    CHECK_EQ(h.rt.DrawList().Stats().Commands, 1);
    CHECK_EQ(h.rt.DrawList().Stats().RenderTargets, 0);
    CHECK_EQ(h.rt.DrawList().Stats().EffectPasses, 0);
}

TEST(UIx_shadow_is_one_command_not_many_rects) {
    Harness h;
    UINode& n = Node(h.Doc(), "Card");
    n.Ensure<UITransform>().Size = {100.0f, 100.0f};
    n.Ensure<UIFill>();
    n.Ensure<UIEffects>().Ensure<UIDropShadow>();
    h.Step();
    h.rt.Build();
    // Тень + заливка = две команды. Не десяток прямоугольников с падающей
    // прозрачностью (§39, §145).
    CHECK_EQ(h.rt.DrawList().Stats().Commands, 2);
    CHECK_TRUE(h.rt.DrawList().Commands()[0].Softness > 0.0f);
}

// --- Подготовка кадра и батчинг -----------------------------------------------

TEST(UIx_batching_merges_compatible_commands) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Ensure<UITransform>().SetStretch(true, true);
    for (int i = 0; i < 10; ++i) {
        UINode& n = Node(h.Doc(), "Box", root.Id);
        UITransform& t = n.Ensure<UITransform>();
        t.Offset = {(float)i * 20.0f, 0.0f};
        t.Size = {18.0f, 18.0f};
        n.Ensure<UIFill>();
    }
    h.Step();
    h.rt.Build();
    // Десять сплошных прямоугольников с одинаковым состоянием — ОДИН батч
    // (§145: «каждый элемент — отдельный draw call» считается плохим решением).
    CHECK_EQ(h.rt.DrawList().Stats().Commands, 10);
    CHECK_EQ(h.rt.DrawList().Stats().Batches, 1);
}

TEST(UIx_batch_breaks_on_clip_change) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Ensure<UITransform>().SetStretch(true, true);
    UINode& plain = Node(h.Doc(), "Plain", root.Id);
    plain.Ensure<UITransform>().Size = {50.0f, 50.0f};
    plain.Ensure<UIFill>();

    UINode& masked = Node(h.Doc(), "Masked", root.Id);
    masked.Ensure<UITransform>().Size = {50.0f, 50.0f};
    masked.Ensure<UIMask>();
    UINode& inner = Node(h.Doc(), "Inner", masked.Id);
    inner.Ensure<UITransform>().Size = {50.0f, 50.0f};
    inner.Ensure<UIFill>();
    h.Step();
    h.rt.Build();
    CHECK_TRUE(h.rt.DrawList().Stats().Batches >= 2);
}

TEST(UIx_culling_skips_offscreen_nodes) {
    Harness h(200.0f, 200.0f);
    UINode& n = Node(h.Doc(), "FarAway");
    UITransform& t = n.Ensure<UITransform>();
    t.Offset = {5000.0f, 5000.0f};
    t.Size = {50.0f, 50.0f};
    n.Ensure<UIFill>();
    h.Step();
    CHECK_TRUE(h.rt.Layout().Get(n.Id)->Culled);
    h.rt.Build();
    CHECK_EQ(h.rt.DrawList().Stats().Commands, 0);
}

// --- Сериализация -------------------------------------------------------------

TEST(UIx_serialization_roundtrip) {
    UIDocument doc;
    doc.SetName("Test");
    doc.Canvas().Reference = {1280.0f, 720.0f};
    UINode& root = Node(doc, "Root");
    root.Layer = 3;
    root.Opacity = 0.8f;
    UIFill& fill = root.Ensure<UIFill>();
    fill.Color = UIColor(0.1f, 0.2f, 0.3f, 0.4f);
    fill.Radius = UICorners(1.0f, 2.0f, 3.0f, 4.0f);
    fill.Gradient = UIGradient::TwoColor(UIColor(1, 0, 0, 1), UIColor(0, 0, 1, 1), 45.0f);
    UINode& kid = Node(doc, "Kid", root.Id);
    kid.Ensure<UIText>().Text = "Привет, мир";
    kid.Ensure<UIEffects>().Ensure<UIDropShadow>().Blur = 17.0f;

    const std::string json = UISaveDocumentToString(doc);

    UIDocument loaded;
    const UILoadReport report = UILoadDocumentFromString(loaded, json);
    CHECK_TRUE(report.Ok);
    CHECK_EQ(report.Nodes, 2);
    CHECK_EQ(loaded.Name(), std::string("Test"));
    CHECK_NEAR(loaded.Canvas().Reference.x, 1280.0, 1e-4);

    UINode* r2 = loaded.FindByName("Root");
    CHECK_TRUE(r2 != nullptr);
    CHECK_EQ(r2->Layer, 3);
    CHECK_NEAR(r2->Opacity, 0.8, 1e-4);
    CHECK_NEAR(r2->Get<UIFill>()->Color.a, 0.4, 1e-4);
    CHECK_NEAR(r2->Get<UIFill>()->Radius.BR, 3.0, 1e-4);
    CHECK_EQ(r2->Get<UIFill>()->Gradient.Stops.size(), (size_t)2);

    UINode* k2 = loaded.FindByName("Kid");
    CHECK_EQ(k2->Get<UIText>()->Text, std::string("Привет, мир"));
    CHECK_NEAR(k2->Get<UIEffects>()->Get<UIDropShadow>()->Blur, 17.0, 1e-4);
}

TEST(UIx_serialization_keeps_unknown_components) {
    // Документ из сборки с плагином открывается сборкой без него — и чужие
    // данные обязаны уцелеть, иначе одно открытие уничтожает чужую работу.
    const std::string json = R"({
      "ui_version": 1,
      "name": "X",
      "nodes": [{"id":1,"guid":"a","name":"Root","components":[
          {"type":"transform"},
          {"type":"plugin_radar","Range":42.5,"Label":"радар"}]}]
    })";
    UIDocument doc;
    const UILoadReport report = UILoadDocumentFromString(doc, json);
    CHECK_TRUE(report.Ok);
    CHECK_EQ(report.Warnings.size(), (size_t)1);

    const std::string again = UISaveDocumentToString(doc);
    CHECK_TRUE(again.find("plugin_radar") != std::string::npos);
    CHECK_TRUE(again.find("42.5") != std::string::npos);
}

TEST(UIx_serialization_missing_property_uses_default) {
    // Новое свойство не ломает старые файлы: его просто нет, и берётся
    // значение из конструктора компонента (§65).
    const std::string json = R"({"ui_version":1,"nodes":[
        {"name":"N","components":[{"type":"transform"},{"type":"text","Text":"hi"}]}]})";
    UIDocument doc;
    CHECK_TRUE(UILoadDocumentFromString(doc, json).Ok);
    const UIText* t = doc.FindByName("N")->Get<UIText>();
    CHECK_EQ(t->Text, std::string("hi"));
    CHECK_NEAR(t->Size, UIText().Size, 1e-4);
    CHECK_NEAR(t->LineSpacing, 1.0, 1e-4);
}

TEST(UIx_serialization_broken_json_is_reported) {
    UIDocument doc;
    const UILoadReport r = UILoadDocumentFromString(doc, "{ not json");
    CHECK_FALSE(r.Ok);
    CHECK_TRUE(!r.Error.empty());
}

TEST(UIx_theme_roundtrip) {
    UITheme theme = UITheme::Default();
    theme.Tokens.SetColor("Color.Custom", UIColor(0.25f, 0.5f, 0.75f, 1.0f));
    const std::string json = UISaveThemeToString(theme);

    UITheme loaded;
    CHECK_TRUE(UILoadThemeFromString(loaded, json));
    CHECK_NEAR(loaded.Tokens.Color("Color.Custom").g, 0.5, 0.01);
    CHECK_TRUE(loaded.Find("Button") != nullptr);
    CHECK_EQ(loaded.Find("ButtonPrimary")->Parent, std::string("Button"));
}

TEST(UIx_subtree_clipboard_roundtrip) {
    UIDocument doc;
    UINode& src = Node(doc, "Src");
    src.Ensure<UIText>().Text = "copy me";
    Node(doc, "Kid", src.Id);

    const std::string clip = UISaveSubtree(doc, src.Id);
    const UINodeId pasted = UILoadSubtree(doc, kUIInvalidNode, clip);
    CHECK_TRUE(pasted != kUIInvalidNode);
    UINode* p = doc.Find(pasted);
    CHECK_EQ(p->Get<UIText>()->Text, std::string("copy me"));
    // Guid вставленной копии обязан отличаться, иначе ссылки указывают
    // неизвестно на какую из двух.
    CHECK_TRUE(p->Guid != src.Guid);
}

// --- Привязка данных и анимация ------------------------------------------------

TEST(UIx_property_path_parsing) {
    const UIPropertyPath p = UIPropertyPath::Parse("Panel/Health.fill.Color.a");
    CHECK_EQ(p.NodePath, std::string("Panel/Health"));
    CHECK_EQ(p.Component, std::string("fill"));
    CHECK_EQ(p.Property, std::string("Color"));
    CHECK_EQ(p.Channel, 3);
}

TEST(UIx_property_binding_reads_and_writes) {
    UIDocument doc;
    UINode& root = Node(doc, "Panel");
    UINode& bar = Node(doc, "Bar", root.Id);
    bar.Ensure<UIFill>().Color = UIColor(1.0f, 1.0f, 1.0f, 1.0f);

    UIPropertyBinding b;
    CHECK_TRUE(b.Bind(doc, "Panel/Bar.fill.Color.a"));
    float v = 0.0f;
    CHECK_TRUE(b.Get(v));
    CHECK_NEAR(v, 1.0, 1e-4);
    CHECK_TRUE(b.Set(0.25f));
    CHECK_NEAR(bar.Get<UIFill>()->Color.a, 0.25, 1e-4);
    // Повторная запись того же значения ничего не меняет — иначе «изменилось»
    // теряет смысл и пересчёт идёт каждый кадр.
    CHECK_FALSE(b.Set(0.25f));
}

TEST(UIx_data_source_feeds_ui_without_ui_knowing_meaning) {
    struct Source : IUIDataSource {
        float number = 0.75f;
        std::string text = "120";
        bool Number(const std::string& key, float& out) const override {
            if (key != "any.number") return false;
            out = number;
            return true;
        }
        bool Text(const std::string& key, std::string& out) const override {
            if (key != "any.text") return false;
            out = text;
            return true;
        }
    } source;

    UIDocument doc;
    UINode& bar = Node(doc, "Bar");
    bar.Ensure<UIProgress>().Value = 0.0f;
    UINode& label = Node(doc, "Label");
    label.Ensure<UIText>().Text.clear();

    UIBindings bindings;
    bindings.Add("Bar.progress.Value", "any.number");
    bindings.Add("Label.text.Text", "any.text", /*asText=*/true);
    CHECK_EQ(bindings.Apply(doc, source), 2);

    // Интерфейс получил 0.75 и "120" — и не знает, что это здоровье и патроны.
    CHECK_NEAR(bar.Get<UIProgress>()->Value, 0.75, 1e-4);
    CHECK_EQ(label.Get<UIText>()->Text, std::string("120"));
}

TEST(UIx_animated_value_approaches_target) {
    UIAnimatedValue v;
    v.Current = 0.0f;
    v.Target = 1.0f;
    v.Speed = 2.0f;
    v.Step(0.25f);
    CHECK_NEAR(v.Current, 0.5, 1e-4);
    v.Step(1.0f);
    CHECK_NEAR(v.Current, 1.0, 1e-4); // не перелетает цель
}

TEST(UIx_progress_smoothing) {
    UIDocument doc;
    UINode& n = Node(doc, "Bar");
    UIProgress& p = n.Ensure<UIProgress>();
    p.Value = 1.0f;
    p.Smoothing = 2.0f;
    p.Displayed = 0.0f;
    UIUpdateWidgets(doc, 0.25f);
    CHECK_NEAR(n.Get<UIProgress>()->Displayed, 0.5, 1e-3);
}

// --- Префабы -------------------------------------------------------------------

TEST(UIx_prefab_instance_overrides_survive_refresh) {
    const std::string path = "test_ui_prefab.uiprefab";
    {
        UIDocument source;
        UINode& root = Node(source, "Card");
        root.Ensure<UIFill>().Color = UIColor(0.1f, 0.1f, 0.1f, 1.0f);
        UINode& title = Node(source, "Title", root.Id);
        title.Ensure<UIText>().Text = "Original";
        CHECK_TRUE(UICreatePrefabFromNode(source, root.Id, path));
    }

    UIDocument doc;
    const UINodeId instance = UIInstantiatePrefab(doc, kUIInvalidNode, path);
    CHECK_TRUE(instance != kUIInvalidNode);
    UINode* inst = doc.Find(instance);
    CHECK_TRUE(inst->Get<UIPrefabInstance>() != nullptr);

    // Правим экземпляр и запоминаем отличия.
    UINode* title = doc.Find(inst->Children[0]);
    title->Get<UIText>()->Text = "Changed";
    CHECK_TRUE(UICapturePrefabOverrides(doc, instance) >= 1);

    // Обновление из источника не должно стирать ручную правку (§63).
    CHECK_EQ(UIRefreshPrefabInstances(doc, path), 1);
    UINode* freshInstance = doc.Find(doc.Roots().front());
    UINode* freshTitle = doc.Find(freshInstance->Children[0]);
    CHECK_EQ(freshTitle->Get<UIText>()->Text, std::string("Changed"));

    std::remove(path.c_str());
}

TEST(UIx_missing_prefab_leaves_traceable_stub) {
    UIDocument doc;
    const UINodeId id = UIInstantiatePrefab(doc, kUIInvalidNode, "no_such_file.uiprefab");
    CHECK_TRUE(id != kUIInvalidNode);
    // Ссылку видно — иначе поправить путь потом не по чему (§134).
    CHECK_EQ(doc.Find(id)->Get<UIPrefabInstance>()->Source,
             std::string("no_such_file.uiprefab"));
}

// --- Реестры и расширяемость ----------------------------------------------------

namespace {
// Свой компонент разработчика: одна структура, одна таблица свойств, одна
// регистрация. Ни строчки правок в ядре — ровно то, что требует §98/§128.
struct MyRadar : UIComponentOf<MyRadar> {
    static const UIComponentType& StaticType() {
        static UIComponentType t = [] {
            UIComponentType d;
            d.Id = "my_radar";
            d.Title = "Radar";
            d.Category = UIComponentCategory::Appearance;
            d.Order = 40;
            d.Create = [] { return std::unique_ptr<UIComponent>(new MyRadar()); };
            d.Props = {
                {"Range", "Range", UIProperty::Kind::Float, SAGE_UI_OFFSET(MyRadar, Range), 0.0f,
                 500.0f, nullptr, nullptr, 0, UIProperty::Widget::Auto, nullptr},
            };
            return d;
        }();
        return t;
    }
    float Range = 100.0f;
};
} // namespace

TEST(UIx_custom_component_gets_everything_from_one_registration) {
    UIComponentRegistry::Instance().Register(MyRadar::StaticType());

    UIDocument doc;
    UINode& n = Node(doc, "Radar");
    MyRadar& radar = n.Ensure<MyRadar>();
    radar.Range = 42.0f;

    // Сериализация — бесплатно.
    const std::string json = UISaveDocumentToString(doc);
    UIDocument loaded;
    CHECK_TRUE(UILoadDocumentFromString(loaded, json).Ok);
    CHECK_NEAR(loaded.FindByName("Radar")->Get<MyRadar>()->Range, 42.0, 1e-4);

    // Копирование — бесплатно.
    const UINodeId copy = doc.Duplicate(n.Id);
    CHECK_NEAR(doc.Find(copy)->Get<MyRadar>()->Range, 42.0, 1e-4);

    // Привязка свойства — бесплатно.
    UIPropertyBinding b;
    CHECK_TRUE(b.Bind(doc, "Radar.my_radar.Range"));
    CHECK_TRUE(b.Set(7.0f));
    CHECK_NEAR(radar.Range, 7.0, 1e-4);
}

TEST(UIx_widget_registry_builds_by_name) {
    UIDocument doc;
    const UINodeId button = doc.CreateWidget("button");
    CHECK_TRUE(button != kUIInvalidNode);
    CHECK_TRUE(doc.Find(button)->Has<UIInteraction>());
    // Неизвестное имя — пустой ответ, а не «кнопка создалась и не работает».
    CHECK_EQ(doc.CreateWidget("no_such_widget"), kUIInvalidNode);
    CHECK_TRUE(UIWidgetRegistry::Instance().All().size() >= 10);
}

// --- Отладка и целостность --------------------------------------------------------

TEST(UIx_validation_detects_broken_documents) {
    UIDocument doc;
    UINode& a = Node(doc, "A");
    UINode& b = Node(doc, "B", a.Id);
    CHECK_TRUE(UIValidate(doc).empty());

    // Ломаем связь руками — ровно то, что делает битый файл или чужой код.
    b.Parent = 999;
    const auto issues = UIValidate(doc);
    CHECK_TRUE(!issues.empty());
}

TEST(UIx_explanations_answer_the_four_questions) {
    Harness h;
    UINode& panel = Node(h.Doc(), "Panel");
    panel.Ensure<UITransform>().Size = {100.0f, 100.0f};
    panel.Ensure<UIFill>();
    UINode& hidden = Node(h.Doc(), "Hidden", panel.Id);
    hidden.Visible = false;
    hidden.Ensure<UIEffects>().Ensure<UIDropShadow>();
    h.Step();

    CHECK_TRUE(UIExplainPosition(h.Doc(), h.rt.Layout(), panel.Id).find("якоря") !=
               std::string::npos);
    CHECK_TRUE(UIExplainVisibility(h.Doc(), h.rt.Layout(), hidden.Id).find("ПРИЧИНА") !=
               std::string::npos);
    CHECK_TRUE(UIExplainOrder(h.Doc(), h.rt.Layout(), panel.Id).find("слой") != std::string::npos);
    CHECK_TRUE(UIExplainEffects(h.Doc(), hidden.Id).find("Drop shadow") != std::string::npos);
    CHECK_TRUE(UIDumpTree(h.Doc(), &h.rt.Layout()).find("Panel") != std::string::npos);
}

TEST(UIx_profiler_reports_cost) {
    Harness h;
    UINode& root = Node(h.Doc(), "Root");
    root.Ensure<UITransform>().SetStretch(true, true);
    for (int i = 0; i < 5; ++i) {
        UINode& n = Node(h.Doc(), "Box", root.Id);
        n.Ensure<UITransform>().Size = {10.0f, 10.0f};
        n.Ensure<UIFill>();
    }
    h.Step();
    h.rt.Build();
    const UIProfile& p = h.rt.Profile();
    CHECK_EQ(p.Layout.Nodes, 6);
    CHECK_EQ(p.Render.Commands, 5);
    CHECK_TRUE(!p.Summary().empty());
}

// --- Витрина ---------------------------------------------------------------------

TEST(UIx_showcase_builds_and_lays_out) {
    Harness h;
    UITheme theme;
    UIBuildShowcase(h.Doc(), theme);
    CHECK_TRUE(h.Doc().NodeCount() > 50);
    CHECK_TRUE(UIValidate(h.Doc()).empty());

    h.Step();
    h.rt.Build();
    // Витрина обязана не только собираться, но и давать команды рисования:
    // «собралась, но ничего не видно» — не проверка.
    CHECK_TRUE(h.rt.DrawList().Stats().Commands > 30);
    CHECK_TRUE(h.rt.Layout().Stats().Visible > 30);

    // И пережить сохранение/чтение без потерь.
    const std::string json = UISaveDocumentToString(h.Doc(), &theme);
    UIDocument loaded;
    UITheme loadedTheme;
    const UILoadReport report = UILoadDocumentFromString(loaded, json, &loadedTheme);
    CHECK_TRUE(report.Ok);
    CHECK_TRUE(report.Warnings.empty());
    CHECK_EQ(loaded.NodeCount(), h.Doc().NodeCount());
}

TEST(UIx_simple_things_stay_simple) {
    // §129: простейший интерфейс — холст и надпись. Три строки, ни маски, ни
    // эффектов, ни стилей, ни единого лишнего понятия.
    Harness h;
    UINode* label = h.Doc().Create("Hello");
    label->Ensure<UIText>().Text = "Привет";
    h.Step();
    h.rt.Build();
    CHECK_EQ(h.rt.DrawList().Stats().Commands, 1);
    CHECK_TRUE(h.rt.DrawList().Stats().Glyphs >= 6);
}
