#include "sage/ui/showcase/UIShowcaseDocument.h"

#include "sage/ui/UIFramework.h"

namespace sage::ui {

namespace {

UINode& Add(UIDocument& doc, UINodeId parent, const char* name) {
    return *doc.Create(name, parent);
}

UINode& Panel(UIDocument& doc, UINodeId parent, const char* name, const UIColor& color,
              float radius) {
    UINode& n = Add(doc, parent, name);
    UIFill& f = n.Ensure<UIFill>();
    f.Color = color;
    f.Radius = UICorners(radius);
    return n;
}

UINode& Label(UIDocument& doc, UINodeId parent, const char* name, const std::string& text,
              float size, const UIColor& color) {
    UINode& n = Add(doc, parent, name);
    UITransform& t = n.Ensure<UITransform>();
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIText& tx = n.Ensure<UIText>();
    tx.Text = text;
    tx.Size = size;
    tx.Color = color;
    tx.Wrap = UITextWrap::None;
    return n;
}

} // namespace

void UIBuildShowcase(UIDocument& doc, UITheme& theme) {
    UIInitialize();
    doc.Clear();
    doc.SetName("UI Showcase");
    theme = UITheme::Default();

    UICanvasSettings& canvas = doc.Canvas();
    canvas.Scale = UICanvasSettings::ScaleMode::ScaleWithSize;
    canvas.Reference = {1920.0f, 1080.0f};
    canvas.MatchWidthOrHeight = 0.5f;

    // --- Фон: градиент во весь холст плюс декоративные формы ----------------
    UINode& root = Add(doc, kUIInvalidNode, "Showcase");
    root.Ensure<UITransform>().SetStretch(true, true);

    UINode& background = Add(doc, root.Id, "Background");
    background.Ensure<UITransform>().SetStretch(true, true);
    UIFill& bg = background.Ensure<UIFill>();
    bg.Type = UIFill::Kind::Gradient;
    bg.Color = UIColor(1.0f);
    bg.Gradient.Type = UIGradient::Kind::Linear;
    bg.Gradient.Angle = 20.0f;
    bg.Gradient.Stops = {{0.0f, UIColorFromHex("#0B0D14")},
                         {0.55f, UIColorFromHex("#161B2B")},
                         {1.0f, UIColorFromHex("#241B33")}};

    // Декоративные формы — заодно проверка процедурных фигур и режимов
    // наложения: круг, кольцо и многоугольник поверх градиента.
    UINode& halo = Add(doc, background.Id, "Halo");
    UITransform& ht = halo.Ensure<UITransform>();
    ht.AnchorMin = ht.AnchorMax = {0.82f, 0.18f};
    ht.Pivot = {0.5f, 0.5f};
    ht.Size = {520.0f, 520.0f};
    UIShape& haloShape = halo.Ensure<UIShape>();
    haloShape.Type = UIShape::Kind::Circle;
    haloShape.Color = UIColor(0.95f, 0.76f, 0.20f, 0.10f);
    haloShape.Softness = 90.0f;
    halo.Blend = UIBlendMode::Add;

    UINode& ring = Add(doc, background.Id, "Ring");
    UITransform& rt = ring.Ensure<UITransform>();
    rt.AnchorMin = rt.AnchorMax = {0.12f, 0.86f};
    rt.Pivot = {0.5f, 0.5f};
    rt.Size = {260.0f, 260.0f};
    UIShape& ringShape = ring.Ensure<UIShape>();
    ringShape.Type = UIShape::Kind::Ring;
    ringShape.Thickness = 3.0f;
    ringShape.Color = UIColor(0.45f, 0.65f, 1.0f, 0.25f);

    UINode& poly = Add(doc, background.Id, "Polygon");
    UITransform& pt = poly.Ensure<UITransform>();
    pt.AnchorMin = pt.AnchorMax = {0.5f, 0.94f};
    pt.Pivot = {0.5f, 0.5f};
    pt.Size = {120.0f, 120.0f};
    UIShape& polyShape = poly.Ensure<UIShape>();
    polyShape.Type = UIShape::Kind::Polygon;
    polyShape.Sides = 6;
    polyShape.Color = UIColor(1.0f, 1.0f, 1.0f, 0.06f);

    // --- Верхняя панель: горизонтальная раскладка, подгонка по содержимому ---
    UINode& topBar = Panel(doc, root.Id, "TopBar", UIColorFromHex("#12141ACC"), 0.0f);
    UITransform& tb = topBar.Ensure<UITransform>();
    tb.SetStretch(true, false);
    tb.AnchorMin.y = tb.AnchorMax.y = 0.0f;
    tb.Size.y = 64.0f;
    UILayout& tbl = topBar.Ensure<UILayout>();
    tbl.Kind = UILayout::Mode::Horizontal;
    tbl.Cross = UIAlign::Center;
    tbl.Padding = UIEdges(24.0f, 12.0f, 24.0f, 12.0f);
    tbl.Gap = {16.0f, 0.0f};

    UINode& logo = Add(doc, topBar.Id, "Logo");
    logo.Ensure<UITransform>().Size = {36.0f, 36.0f};
    UIShape& logoShape = logo.Ensure<UIShape>();
    logoShape.Type = UIShape::Kind::RoundedRect;
    logoShape.Radius = UICorners(10.0f, 4.0f, 10.0f, 4.0f); // разные радиусы углов
    logoShape.Color = UIColorFromHex("#F2C230");

    Label(doc, topBar.Id, "Title", "SAGE UI", 26.0f, UIColorFromHex("#ECEFF4"));

    // Распорка: пустой узел, который забирает всё свободное место. Так «прижать
    // остальное вправо» делается раскладкой, а не подбором отступа.
    UINode& spacer = Add(doc, topBar.Id, "Spacer");
    spacer.Ensure<UITransform>().WidthMode = UISizeMode::Stretch;

    UINode& currency = Add(doc, topBar.Id, "Currency");
    UITransform& ct = currency.Ensure<UITransform>();
    ct.WidthMode = UISizeMode::Content;
    ct.HeightMode = UISizeMode::Content;
    UILayout& cl = currency.Ensure<UILayout>();
    cl.Kind = UILayout::Mode::Horizontal;
    cl.Cross = UIAlign::Center;
    cl.Gap = {8.0f, 0.0f};
    cl.Padding = UIEdges(12.0f, 6.0f, 12.0f, 6.0f);
    cl.FitWidth = cl.FitHeight = true;
    UIFill& cf = currency.Ensure<UIFill>();
    cf.Color = UIColorFromHex("#1F2431");
    cf.Radius = UICorners(999.0f);
    UINode& coin = Add(doc, currency.Id, "Coin");
    coin.Ensure<UITransform>().Size = {16.0f, 16.0f};
    UIShape& coinShape = coin.Ensure<UIShape>();
    coinShape.Type = UIShape::Kind::Circle;
    coinShape.Color = UIColorFromHex("#F2C230");
    Label(doc, currency.Id, "Amount", "1 240", 18.0f, UIColorFromHex("#ECEFF4"));

    // --- Карточки: сетка, тень, свечение, вложенные группы ------------------
    UINode& cards = Add(doc, root.Id, "Cards");
    UITransform& cardsT = cards.Ensure<UITransform>();
    cardsT.AnchorMin = {0.0f, 0.0f};
    cardsT.AnchorMax = {1.0f, 1.0f};
    cardsT.WidthMode = UISizeMode::Stretch;
    cardsT.HeightMode = UISizeMode::Stretch;
    cardsT.Margin = UIEdges(48.0f, 104.0f, 420.0f, 48.0f);
    UILayout& cardsL = cards.Ensure<UILayout>();
    cardsL.Kind = UILayout::Mode::Grid;
    cardsL.Columns = 3;
    cardsL.Gap = {24.0f, 24.0f};
    cardsL.Padding = UIEdges::Uniform(0.0f);
    cardsL.CellSize = {0.0f, 220.0f};

    const char* kCardTitles[] = {"Маски", "Эффекты", "Раскладка",
                                 "Типографика", "Формы", "Состояния"};
    for (int i = 0; i < 6; ++i) {
        UINode& card = Panel(doc, cards.Id, "Card", UIColorFromHex("#1A1D25F2"), 14.0f);
        card.Ensure<UITransform>().Size = {280.0f, 220.0f};
        UIBorder& cb = card.Ensure<UIBorder>();
        cb.Thickness = UIEdges::Uniform(1.0f);
        cb.Color = UIColorFromHex("#333A47");
        // Тень — ЭФФЕКТ, а не десяток прямоугольников (§39).
        UIEffects& fx = card.Ensure<UIEffects>();
        UIDropShadow& shadow = fx.Ensure<UIDropShadow>();
        shadow.Offset = {0.0f, 8.0f};
        shadow.Blur = 22.0f;
        shadow.Color = UIColor(0.0f, 0.0f, 0.0f, 0.5f);
        if (i == 1) {
            // Вторая карточка ещё и светится — проверка стека из двух эффектов
            // и того, что порядок в нём значим.
            UIGlow& glow = fx.Ensure<UIGlow>();
            glow.Color = UIColorFromHex("#F2C230");
            glow.Radius = 30.0f;
            glow.Intensity = 0.5f;
        }

        UILayout& cardLayout = card.Ensure<UILayout>();
        cardLayout.Kind = UILayout::Mode::Vertical;
        cardLayout.Padding = UIEdges::Uniform(16.0f);
        cardLayout.Gap = {0.0f, 10.0f};
        cardLayout.Cross = UIAlign::Stretch;

        // Обложка с маской: скруглённая маска режет содержимое поддерева.
        UINode& cover = Add(doc, card.Id, "Cover");
        cover.Ensure<UITransform>().Size = {0.0f, 96.0f};
        UIMask& mask = cover.Ensure<UIMask>();
        mask.Form = UIMask::Shape::RoundedRect;
        mask.Radius = UICorners(10.0f);
        UINode& coverFill = Add(doc, cover.Id, "Gradient");
        coverFill.Ensure<UITransform>().SetStretch(true, true);
        UIFill& cfill = coverFill.Ensure<UIFill>();
        cfill.Type = UIFill::Kind::Gradient;
        cfill.Gradient.Type = UIGradient::Kind::Linear;
        cfill.Gradient.Angle = 120.0f;
        cfill.Gradient.Stops = {{0.0f, UIColorFromHex("#2A3350")},
                                {1.0f, UIColorFromHex("#5B3A6E")}};
        // Круг, ВЫЛЕЗАЮЩИЙ за границу обложки: если маска не работает, это
        // видно сразу.
        UINode& bubble = Add(doc, cover.Id, "Bubble");
        UITransform& bt = bubble.Ensure<UITransform>();
        bt.AnchorMin = bt.AnchorMax = {1.0f, 1.0f};
        bt.Pivot = {0.5f, 0.5f};
        bt.Size = {110.0f, 110.0f};
        UIShape& bubbleShape = bubble.Ensure<UIShape>();
        bubbleShape.Type = UIShape::Kind::Circle;
        bubbleShape.Color = UIColor(1.0f, 1.0f, 1.0f, 0.10f);

        Label(doc, card.Id, "Title", kCardTitles[i], 20.0f, UIColorFromHex("#ECEFF4"));

        UINode& desc = Add(doc, card.Id, "Desc");
        UITransform& dt = desc.Ensure<UITransform>();
        dt.WidthMode = UISizeMode::Stretch;
        dt.HeightMode = UISizeMode::Content;
        UIText& dtext = desc.Ensure<UIText>();
        dtext.Text = "Длинное описание, которое переносится по словам и обрезается многоточием, "
                     "если строк больше двух.";
        dtext.Size = 14.0f;
        dtext.Color = UIColorFromHex("#9AA3B2");
        dtext.Wrap = UITextWrap::Word;
        dtext.MaxLines = 2;
        dtext.Overflow = UITextOverflow::Ellipsis;
    }

    // --- Боковая панель: прокрутка, маска, элементы управления --------------
    UINode& side = Panel(doc, root.Id, "SidePanel", UIColorFromHex("#12141AE6"), 16.0f);
    UITransform& st = side.Ensure<UITransform>();
    st.AnchorMin = {1.0f, 0.0f};
    st.AnchorMax = {1.0f, 1.0f};
    st.HeightMode = UISizeMode::Stretch;
    st.Margin = UIEdges(0.0f, 104.0f, 0.0f, 48.0f);
    st.Size.x = 340.0f;
    st.Pivot = {1.0f, 0.0f};
    st.Offset = {-48.0f, 0.0f};
    UIBorder& sideBorder = side.Ensure<UIBorder>();
    sideBorder.Thickness = UIEdges::Uniform(1.0f);
    sideBorder.Color = UIColorFromHex("#2B3242");
    UILayout& sideLayout = side.Ensure<UILayout>();
    sideLayout.Kind = UILayout::Mode::Vertical;
    sideLayout.Padding = UIEdges::Uniform(20.0f);
    sideLayout.Gap = {0.0f, 14.0f};
    sideLayout.Cross = UIAlign::Stretch;

    Label(doc, side.Id, "Header", "Элементы", 22.0f, UIColorFromHex("#ECEFF4"));

    const UINodeId button = UIMakeButton(doc, side.Id, "Кнопка", "showcase.action");
    if (UINode* b = doc.Find(button)) b->Ensure<UITransform>().Size.y = 44.0f;
    UIMakeCheckbox(doc, side.Id, "Переключатель");
    UIMakeSlider(doc, side.Id);
    const UINodeId progress = UIMakeProgress(doc, side.Id);
    if (UINode* p = doc.Find(progress)) {
        UIProgress& pr = p->Ensure<UIProgress>();
        pr.Value = 0.72f;
        pr.FillColor = UIColorFromHex("#5CBF6B");
    }
    UIMakeInputField(doc, side.Id, "Поиск");

    // Прокручиваемый список с маской и мягким краем: содержимое растворяется у
    // нижней границы, а не обрывается.
    UINode& scroll = Add(doc, side.Id, "Scroll");
    UITransform& scT = scroll.Ensure<UITransform>();
    scT.HeightMode = UISizeMode::Stretch;
    scroll.Ensure<UIScrollView>();
    UIMask& scrollMask = scroll.Ensure<UIMask>();
    scrollMask.Form = UIMask::Shape::Gradient;
    scrollMask.GradientAngle = 180.0f;
    scrollMask.GradientStart = 0.86f;
    scrollMask.GradientEnd = 1.0f;
    scrollMask.Invert = true;
    UINode& content = Add(doc, scroll.Id, "Content");
    UITransform& contentT = content.Ensure<UITransform>();
    contentT.WidthMode = UISizeMode::Stretch;
    contentT.HeightMode = UISizeMode::Content;
    UILayout& contentL = content.Ensure<UILayout>();
    contentL.Kind = UILayout::Mode::Vertical;
    contentL.Gap = {0.0f, 6.0f};
    contentL.Padding = UIEdges::Uniform(0.0f);
    contentL.FitHeight = true;
    contentL.Cross = UIAlign::Stretch;
    for (int i = 0; i < 12; ++i) {
        UINode& row = Panel(doc, content.Id, "Row", UIColorFromHex("#1A1D25"), 8.0f);
        row.Ensure<UITransform>().Size.y = 34.0f;
        UILayout& rowLayout = row.Ensure<UILayout>();
        rowLayout.Kind = UILayout::Mode::Horizontal;
        rowLayout.Cross = UIAlign::Center;
        rowLayout.Padding = UIEdges(10.0f, 0.0f, 10.0f, 0.0f);
        rowLayout.Gap = {8.0f, 0.0f};
        UINode& dot = Add(doc, row.Id, "Dot");
        dot.Ensure<UITransform>().Size = {8.0f, 8.0f};
        UIShape& dotShape = dot.Ensure<UIShape>();
        dotShape.Type = UIShape::Kind::Circle;
        dotShape.Color = UIColorFromHex("#F2C230");
        Label(doc, row.Id, "Text", "Строка списка " + std::to_string(i + 1), 14.0f,
              UIColorFromHex("#C6CCD8"));
        UIInteraction& ia = row.Ensure<UIInteraction>();
        ia.Hit = UIHitShape::RoundedRect;
    }

    // --- Типографика: выравнивание, обводка, тень ---------------------------
    UINode& typo = Add(doc, root.Id, "Typography");
    UITransform& typoT = typo.Ensure<UITransform>();
    typoT.AnchorMin = {0.0f, 1.0f};
    typoT.AnchorMax = {0.0f, 1.0f};
    typoT.Pivot = {0.0f, 1.0f};
    typoT.Offset = {48.0f, -48.0f};
    typoT.WidthMode = UISizeMode::Content;
    typoT.HeightMode = UISizeMode::Content;
    UILayout& typoL = typo.Ensure<UILayout>();
    typoL.Kind = UILayout::Mode::Vertical;
    typoL.FitWidth = typoL.FitHeight = true;
    typoL.Padding = UIEdges::Uniform(0.0f);
    typoL.Gap = {0.0f, 4.0f};

    UINode& big = Label(doc, typo.Id, "Big", "Заголовок с обводкой", 34.0f,
                        UIColorFromHex("#FFFFFF"));
    UIText& bigText = big.Ensure<UIText>();
    bigText.OutlineWidth = 2.0f;
    bigText.OutlineColor = UIColorFromHex("#12141A");
    bigText.ShadowOffset = {0.0f, 3.0f};
    bigText.ShadowColor = UIColor(0.0f, 0.0f, 0.0f, 0.6f);

    Label(doc, typo.Id, "Small", "Мелкий текст · latin text · 0123456789", 15.0f,
          UIColorFromHex("#9AA3B2"));

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

} // namespace sage::ui
