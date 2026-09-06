#include "sage/ui/showcase/UIDemos.h"

#include "sage/ui/UIFramework.h"
#include "sage/ui/visual/UIIcon.h"

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
              float size, const UIColor& color, UITextAlign align = UITextAlign::Left) {
    UINode& n = Add(doc, parent, name);
    UITransform& t = n.Ensure<UITransform>();
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIText& tx = n.Ensure<UIText>();
    tx.Text = text;
    tx.Size = size;
    tx.Color = color;
    tx.Align = align;
    tx.Wrap = UITextWrap::None;
    return n;
}

// Общее начало любого экрана: холст под опорное разрешение и корень во весь
// экран. Повторять это в каждом демо — верный способ получить пять разных
// холстов и объяснять потом, почему меню масштабируется, а худ нет.
UINode& Screen(UIDocument& doc, UITheme& theme, const char* name) {
    UIInitialize();
    doc.Clear();
    doc.SetName(name);
    theme = UITheme::Default();
    UICanvasSettings& canvas = doc.Canvas();
    canvas.Scale = UICanvasSettings::ScaleMode::ScaleWithSize;
    canvas.Reference = {1920.0f, 1080.0f};
    canvas.MatchWidthOrHeight = 0.5f;

    UINode& root = Add(doc, kUIInvalidNode, name);
    root.Ensure<UITransform>().SetStretch(true, true);
    return root;
}

// Кнопка меню: подложка, надпись по центру и команда наружу.
UINodeId MenuButton(UIDocument& doc, UINodeId parent, const std::string& text,
                    const std::string& command, bool primary = false) {
    const UINodeId id = UIMakeButton(doc, parent, text, command);
    UINode* n = doc.Find(id);
    if (!n) return id;
    n->Name = text;
    UITransform& t = n->Ensure<UITransform>();
    t.Size = {320.0f, 56.0f};
    t.WidthMode = UISizeMode::Stretch;
    UIFill& f = n->Ensure<UIFill>();
    f.Radius = UICorners(12.0f);
    f.Color = primary ? UIColorFromHex("#F2C230") : UIColorFromHex("#232833E6");
    n->Ensure<UIStyled>().Style = primary ? "ButtonPrimary" : "Button";
    // Подпись — отдельный узел внутри кнопки; поправим ей кегль и цвет.
    if (!n->Children.empty()) {
        if (UINode* label = doc.Find(n->Children.front())) {
            UIText& tx = label->Ensure<UIText>();
            tx.Size = 22.0f;
            tx.Color = primary ? UIColorFromHex("#1A1200") : UIColorFromHex("#ECEFF4");
        }
    }
    return id;
}

void GradientBackground(UIDocument& doc, UINodeId parent, const UIColor& a, const UIColor& b,
                        float angle) {
    UINode& bg = Add(doc, parent, "Background");
    bg.Ensure<UITransform>().SetStretch(true, true);
    UIFill& f = bg.Ensure<UIFill>();
    f.Type = UIFill::Kind::Gradient;
    f.Gradient.Type = UIGradient::Kind::Linear;
    f.Gradient.Angle = angle;
    f.Gradient.Stops = {{0.0f, a}, {1.0f, b}};
}

} // namespace

// ---------------------------------------------------------------------------
// ГЛАВНОЕ МЕНЮ
// ---------------------------------------------------------------------------
void UIBuildMainMenu(UIDocument& doc, UITheme& theme) {
    UINode& root = Screen(doc, theme, "MainMenu");
    GradientBackground(doc, root.Id, UIColorFromHex("#0A0C13"), UIColorFromHex("#1E1730"), 25.0f);

    // Декоративное свечение за заголовком: складывается с фоном, а не
    // закрывает его.
    UINode& glow = Add(doc, root.Id, "Glow");
    UITransform& gt = glow.Ensure<UITransform>();
    gt.AnchorMin = gt.AnchorMax = {0.28f, 0.35f};
    gt.Pivot = {0.5f, 0.5f};
    gt.Size = {760.0f, 760.0f};
    UIShape& gs = glow.Ensure<UIShape>();
    gs.Type = UIShape::Kind::Circle;
    gs.Color = UIColor(0.95f, 0.76f, 0.20f, 0.09f);
    gs.Softness = 140.0f;
    glow.Blend = UIBlendMode::Add;

    // Левая колонка: заголовок и кнопки. Вертикальная раскладка с подгонкой по
    // содержимому — добавить шестую кнопку не значит пересчитать пять чужих
    // отступов.
    UINode& column = Add(doc, root.Id, "Menu");
    UITransform& ct = column.Ensure<UITransform>();
    ct.AnchorMin = ct.AnchorMax = {0.0f, 0.5f};
    ct.Pivot = {0.0f, 0.5f};
    ct.Offset = {160.0f, 0.0f};
    ct.Size = {380.0f, 0.0f};
    ct.HeightMode = UISizeMode::Content;
    UILayout& cl = column.Ensure<UILayout>();
    cl.Kind = UILayout::Mode::Vertical;
    cl.Gap = {0.0f, 14.0f};
    cl.Padding = UIEdges::Uniform(0.0f);
    cl.Cross = UIAlign::Stretch;
    cl.FitHeight = true;

    UINode& title = Label(doc, column.Id, "Title", "SAGE", 96.0f, UIColorFromHex("#FFFFFF"));
    UIText& titleText = title.Ensure<UIText>();
    titleText.LetterSpacing = 6.0f;
    titleText.ShadowOffset = {0.0f, 6.0f};
    titleText.ShadowColor = UIColor(0.0f, 0.0f, 0.0f, 0.55f);
    UINode& subtitle = Label(doc, column.Id, "Subtitle", "ДВИЖОК И РЕДАКТОР", 18.0f,
                             UIColorFromHex("#9AA3B2"));
    subtitle.Ensure<UIText>().LetterSpacing = 4.0f;

    UINode& spacer = Add(doc, column.Id, "Spacer");
    spacer.Ensure<UITransform>().Size = {0.0f, 26.0f};

    MenuButton(doc, column.Id, "Продолжить", "menu.continue", true);
    MenuButton(doc, column.Id, "Новая игра", "menu.new");
    MenuButton(doc, column.Id, "Настройки", "menu.settings");
    MenuButton(doc, column.Id, "Выход", "menu.quit");

    UINode& version = Label(doc, root.Id, "Version", "v0.9 · сборка для показа", 15.0f,
                            UIColorFromHex("#5C6474"));
    UITransform& vt = version.Ensure<UITransform>();
    vt.AnchorMin = vt.AnchorMax = {1.0f, 1.0f};
    vt.Pivot = {1.0f, 1.0f};
    vt.Offset = {-32.0f, -24.0f};

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

// ---------------------------------------------------------------------------
// ХУД
// ---------------------------------------------------------------------------
void UIBuildHud(UIDocument& doc, UITheme& theme) {
    UINode& root = Screen(doc, theme, "HUD");

    // --- Левый верх: здоровье и выносливость -------------------------------
    UINode& vitals = Add(doc, root.Id, "Vitals");
    UITransform& vt = vitals.Ensure<UITransform>();
    vt.AnchorMin = vt.AnchorMax = {0.0f, 0.0f};
    vt.Pivot = {0.0f, 0.0f};
    vt.Offset = {40.0f, 36.0f};
    vt.WidthMode = UISizeMode::Content;
    vt.HeightMode = UISizeMode::Content;
    UILayout& vl = vitals.Ensure<UILayout>();
    vl.Kind = UILayout::Mode::Vertical;
    vl.Gap = {0.0f, 10.0f};
    vl.Padding = UIEdges::Uniform(0.0f);
    vl.FitWidth = vl.FitHeight = true;

    struct BarSpec { const char* Name; const char* Icon; const char* Color; float Value; };
    const BarSpec kBars[] = {
        {"Health", "heart", "#E05C5C", 0.68f},
        {"Stamina", "drop", "#5CBF6B", 0.42f},
    };
    for (const BarSpec& spec : kBars) {
        UINode& row = Add(doc, vitals.Id, spec.Name);
        UITransform& rt = row.Ensure<UITransform>();
        rt.Size = {360.0f, 30.0f};
        UILayout& rl = row.Ensure<UILayout>();
        rl.Kind = UILayout::Mode::Horizontal;
        rl.Gap = {10.0f, 0.0f};
        rl.Padding = UIEdges::Uniform(0.0f);
        rl.Cross = UIAlign::Center;

        // Значок и шкала — РАЗНЫЕ узлы: значок не сдвигает шкалу «по правилу
        // из отрисовки», он просто стоит перед ней в раскладке.
        UINode& icon = Add(doc, row.Id, "Icon");
        icon.Ensure<UITransform>().Size = {26.0f, 26.0f};
        UIIcon& ic = icon.Ensure<UIIcon>();
        ic.Name = spec.Icon;
        ic.Color = UIColorFromHex(spec.Color);

        UINode& bar = Add(doc, row.Id, "Fill");
        UITransform& bt = bar.Ensure<UITransform>();
        bt.WidthMode = UISizeMode::Stretch;
        bt.Size.y = 18.0f;
        UIProgress& p = bar.Ensure<UIProgress>();
        p.Value = spec.Value;
        p.FillColor = UIColorFromHex(spec.Color);
        p.TrackColor = UIColorFromHex("#0C0E13CC");
        p.Radius = UICorners(9.0f);
        p.Smoothing = 3.0f;
        UIBorder& bb = bar.Ensure<UIBorder>();
        bb.Thickness = UIEdges::Uniform(1.0f);
        bb.Color = UIColor(1.0f, 1.0f, 1.0f, 0.10f);
        bb.Radius = UICorners(9.0f);
    }

    // --- Правый низ: боезапас ----------------------------------------------
    UINode& ammo = Panel(doc, root.Id, "Ammo", UIColorFromHex("#0C0E13B3"), 14.0f);
    UITransform& at = ammo.Ensure<UITransform>();
    at.AnchorMin = at.AnchorMax = {1.0f, 1.0f};
    at.Pivot = {1.0f, 1.0f};
    at.Offset = {-40.0f, -36.0f};
    at.WidthMode = UISizeMode::Content;
    at.HeightMode = UISizeMode::Content;
    UILayout& al = ammo.Ensure<UILayout>();
    al.Kind = UILayout::Mode::Horizontal;
    al.Cross = UIAlign::Center;
    al.Gap = {10.0f, 0.0f};
    al.Padding = UIEdges(18.0f, 12.0f, 18.0f, 12.0f);
    al.FitWidth = al.FitHeight = true;
    ammo.Ensure<UIEffects>().Ensure<UIDropShadow>().Blur = 20.0f;
    Label(doc, ammo.Id, "Count", "24", 44.0f, UIColorFromHex("#FFFFFF"));
    Label(doc, ammo.Id, "Total", "/ 120", 20.0f, UIColorFromHex("#9AA3B2"));

    // --- Верх по центру: цель -----------------------------------------------
    UINode& objective = Add(doc, root.Id, "Objective");
    UITransform& ot = objective.Ensure<UITransform>();
    ot.AnchorMin = ot.AnchorMax = {0.5f, 0.0f};
    ot.Pivot = {0.5f, 0.0f};
    ot.Offset = {0.0f, 40.0f};
    ot.WidthMode = UISizeMode::Content;
    ot.HeightMode = UISizeMode::Content;
    UILayout& ol = objective.Ensure<UILayout>();
    ol.Kind = UILayout::Mode::Vertical;
    ol.Gap = {0.0f, 4.0f};
    ol.Padding = UIEdges::Uniform(0.0f);
    ol.Cross = UIAlign::Center;
    ol.FitWidth = ol.FitHeight = true;
    Label(doc, objective.Id, "Caption", "ТЕКУЩАЯ ЦЕЛЬ", 13.0f, UIColorFromHex("#F2C230"),
          UITextAlign::Center);
    UINode& goal = Label(doc, objective.Id, "Text", "Добраться до маяка", 24.0f,
                         UIColorFromHex("#ECEFF4"), UITextAlign::Center);
    UIText& goalText = goal.Ensure<UIText>();
    goalText.OutlineWidth = 2.0f;
    goalText.OutlineColor = UIColor(0.0f, 0.0f, 0.0f, 0.75f);

    // --- Центр: прицел ------------------------------------------------------
    UINode& crosshair = Add(doc, root.Id, "Crosshair");
    UITransform& cht = crosshair.Ensure<UITransform>();
    cht.AnchorMin = cht.AnchorMax = {0.5f, 0.5f};
    cht.Pivot = {0.5f, 0.5f};
    cht.Size = {26.0f, 26.0f};
    UIShape& chs = crosshair.Ensure<UIShape>();
    chs.Type = UIShape::Kind::Ring;
    chs.Thickness = 2.0f;
    chs.Color = UIColor(1.0f, 1.0f, 1.0f, 0.75f);

    // --- Левый низ: подсказка ------------------------------------------------
    UINode& hint = Panel(doc, root.Id, "Hint", UIColorFromHex("#0C0E1399"), 8.0f);
    UITransform& ht = hint.Ensure<UITransform>();
    ht.AnchorMin = ht.AnchorMax = {0.0f, 1.0f};
    ht.Pivot = {0.0f, 1.0f};
    ht.Offset = {40.0f, -36.0f};
    ht.WidthMode = UISizeMode::Content;
    ht.HeightMode = UISizeMode::Content;
    UILayout& hl = hint.Ensure<UILayout>();
    hl.Kind = UILayout::Mode::Horizontal;
    hl.Cross = UIAlign::Center;
    hl.Gap = {8.0f, 0.0f};
    hl.Padding = UIEdges(10.0f, 8.0f, 12.0f, 8.0f);
    hl.FitWidth = hl.FitHeight = true;
    UINode& key = Panel(doc, hint.Id, "Key", UIColorFromHex("#232833"), 5.0f);
    UITransform& kt = key.Ensure<UITransform>();
    kt.Size = {30.0f, 30.0f};
    UINode& keyLabel = Label(doc, key.Id, "Text", "E", 17.0f, UIColorFromHex("#ECEFF4"),
                             UITextAlign::Center);
    UITransform& klt = keyLabel.Ensure<UITransform>();
    klt.SetStretch(true, true);
    klt.WidthMode = UISizeMode::Stretch;
    klt.HeightMode = UISizeMode::Stretch;
    keyLabel.Ensure<UIText>().VAlign = UITextVAlign::Center;
    Label(doc, hint.Id, "Action", "подобрать", 17.0f, UIColorFromHex("#C6CCD8"));

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

// ---------------------------------------------------------------------------
// НАСТРОЙКИ
// ---------------------------------------------------------------------------
void UIBuildSettings(UIDocument& doc, UITheme& theme) {
    UINode& root = Screen(doc, theme, "Settings");
    GradientBackground(doc, root.Id, UIColorFromHex("#0B0D14"), UIColorFromHex("#151C2B"), 0.0f);

    UINode& window = Panel(doc, root.Id, "Window", UIColorFromHex("#161A24F2"), 18.0f);
    UITransform& wt = window.Ensure<UITransform>();
    wt.AnchorMin = wt.AnchorMax = {0.5f, 0.5f};
    wt.Pivot = {0.5f, 0.5f};
    wt.Size = {820.0f, 620.0f};
    UIBorder& wb = window.Ensure<UIBorder>();
    wb.Thickness = UIEdges::Uniform(1.0f);
    wb.Color = UIColorFromHex("#2B3242");
    UIDropShadow& shadow = window.Ensure<UIEffects>().Ensure<UIDropShadow>();
    shadow.Blur = 40.0f;
    shadow.Offset = {0.0f, 16.0f};
    shadow.Color = UIColor(0.0f, 0.0f, 0.0f, 0.6f);
    UILayout& wl = window.Ensure<UILayout>();
    wl.Kind = UILayout::Mode::Vertical;
    wl.Padding = UIEdges::Uniform(28.0f);
    wl.Gap = {0.0f, 18.0f};
    wl.Cross = UIAlign::Stretch;

    Label(doc, window.Id, "Title", "Настройки", 34.0f, UIColorFromHex("#ECEFF4"));

    // Вкладки — обычный горизонтальный контейнер с кнопками.
    const UINodeId tabs = UIMakeTabs(doc, window.Id, {"Изображение", "Звук", "Управление"});
    if (UINode* tn = doc.Find(tabs)) tn->Ensure<UITransform>().Size.y = 44.0f;

    // Строки настроек: подпись слева, элемент справа. Один контейнер на строку —
    // и ни одного отступа, посчитанного руками.
    auto row = [&](const char* name, const char* caption) -> UINode& {
        UINode& r = Add(doc, window.Id, name);
        UITransform& rt = r.Ensure<UITransform>();
        rt.Size = {0.0f, 44.0f};
        UILayout& rl = r.Ensure<UILayout>();
        rl.Kind = UILayout::Mode::Horizontal;
        rl.Cross = UIAlign::Center;
        rl.Gap = {16.0f, 0.0f};
        rl.Padding = UIEdges::Uniform(0.0f);
        UINode& cap = Label(doc, r.Id, "Caption", caption, 19.0f, UIColorFromHex("#C6CCD8"));
        cap.Ensure<UITransform>().Size.x = 280.0f;
        cap.Ensure<UITransform>().WidthMode = UISizeMode::Fixed;
        return r;
    };

    {
        UINode& r = row("Volume", "Громкость");
        const UINodeId slider = UIMakeSlider(doc, r.Id);
        if (UINode* s = doc.Find(slider)) {
            s->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
            UIRangeValue& v = s->Ensure<UIRangeValue>();
            v.Value = 0.7f;
            v.AccentColor = UIColorFromHex("#F2C230");
        }
    }
    {
        UINode& r = row("Sensitivity", "Чувствительность");
        const UINodeId slider = UIMakeSlider(doc, r.Id);
        if (UINode* s = doc.Find(slider)) {
            s->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
            s->Ensure<UIRangeValue>().Value = 0.35f;
        }
    }
    {
        UINode& r = row("Fullscreen", "Полный экран");
        const UINodeId box = UIMakeCheckbox(doc, r.Id, "");
        if (UINode* b = doc.Find(box)) {
            b->Ensure<UITransform>().Size = {40.0f, 30.0f};
            if (!b->Children.empty()) {
                if (UINode* inner = doc.Find(b->Children.front()))
                    inner->Ensure<UIRangeValue>().Value = 1.0f;
            }
        }
    }
    {
        UINode& r = row("Quality", "Качество");
        const UINodeId dd = UIMakeDropdown(doc, r.Id, {"Высокое", "Среднее", "Низкое"});
        if (UINode* d = doc.Find(dd)) d->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    }
    {
        UINode& r = row("Name", "Имя игрока");
        const UINodeId field = UIMakeInputField(doc, r.Id, "");
        if (UINode* f = doc.Find(field)) {
            f->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
            f->Ensure<UITextField>().Value = "Странник";
        }
    }

    // Растяжка, отжимающая кнопки к низу окна.
    UINode& gap = Add(doc, window.Id, "Spacer");
    gap.Ensure<UITransform>().HeightMode = UISizeMode::Stretch;

    UINode& footer = Add(doc, window.Id, "Footer");
    UITransform& ft = footer.Ensure<UITransform>();
    ft.Size = {0.0f, 52.0f};
    UILayout& fl = footer.Ensure<UILayout>();
    fl.Kind = UILayout::Mode::Horizontal;
    fl.Gap = {12.0f, 0.0f};
    fl.Padding = UIEdges::Uniform(0.0f);
    fl.Main = UIAlign::End;
    fl.Cross = UIAlign::Stretch;
    MenuButton(doc, footer.Id, "Отмена", "settings.cancel");
    MenuButton(doc, footer.Id, "Применить", "settings.apply", true);

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

// ---------------------------------------------------------------------------
// ИНВЕНТАРЬ
// ---------------------------------------------------------------------------
void UIBuildInventory(UIDocument& doc, UITheme& theme) {
    UINode& root = Screen(doc, theme, "Inventory");
    GradientBackground(doc, root.Id, UIColorFromHex("#0A0C12"), UIColorFromHex("#111726"), 0.0f);

    UINode& window = Panel(doc, root.Id, "Window", UIColorFromHex("#141821F2"), 18.0f);
    UITransform& wt = window.Ensure<UITransform>();
    wt.AnchorMin = wt.AnchorMax = {0.5f, 0.5f};
    wt.Pivot = {0.5f, 0.5f};
    wt.Size = {980.0f, 640.0f};
    window.Ensure<UIEffects>().Ensure<UIDropShadow>().Blur = 44.0f;
    UIBorder& wb = window.Ensure<UIBorder>();
    wb.Thickness = UIEdges::Uniform(1.0f);
    wb.Color = UIColorFromHex("#2B3242");
    UILayout& wl = window.Ensure<UILayout>();
    wl.Kind = UILayout::Mode::Vertical;
    wl.Padding = UIEdges::Uniform(24.0f);
    wl.Gap = {0.0f, 16.0f};
    wl.Cross = UIAlign::Stretch;

    UINode& header = Add(doc, window.Id, "Header");
    header.Ensure<UITransform>().Size.y = 40.0f;
    UILayout& hl = header.Ensure<UILayout>();
    hl.Kind = UILayout::Mode::Horizontal;
    hl.Cross = UIAlign::Center;
    hl.Padding = UIEdges::Uniform(0.0f);
    hl.Gap = {12.0f, 0.0f};
    Label(doc, header.Id, "Title", "Инвентарь", 30.0f, UIColorFromHex("#ECEFF4"));
    UINode& spacer = Add(doc, header.Id, "Spacer");
    spacer.Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    UINode& weight = Label(doc, header.Id, "Weight", "38 / 60 кг", 18.0f,
                           UIColorFromHex("#9AA3B2"));
    (void)weight;

    UINode& body = Add(doc, window.Id, "Body");
    body.Ensure<UITransform>().HeightMode = UISizeMode::Stretch;
    UILayout& bl = body.Ensure<UILayout>();
    bl.Kind = UILayout::Mode::Horizontal;
    bl.Gap = {20.0f, 0.0f};
    bl.Padding = UIEdges::Uniform(0.0f);
    bl.Cross = UIAlign::Stretch;

    // Сетка ячеек: пять столбцов, ячейки со скруглением и значком.
    UINode& grid = Add(doc, body.Id, "Grid");
    grid.Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    UILayout& gl = grid.Ensure<UILayout>();
    gl.Kind = UILayout::Mode::Grid;
    gl.Columns = 5;
    gl.Gap = {12.0f, 12.0f};
    gl.Padding = UIEdges::Uniform(0.0f);
    gl.CellSize = {0.0f, 96.0f};
    // Ячейка занимает ЯЧЕЙКУ СЕТКИ, а не свой прежний размер. Без этого слоты
    // остаются размером «по умолчанию» и лишь центрируются в клетках — то есть
    // налезают друг на друга и вылезают за сетку, а сетка выглядит сломанной.
    gl.Cross = UIAlign::Stretch;

    // Только имена, которые в наборе движка ЕСТЬ: неизвестное имя рисуется
    // перечёркнутым квадратом (и правильно делает — молчать об опечатке хуже),
    // но в показательном экране такой квадрат читается как поломка движка.
    const char* kIcons[] = {"heart", "drop", "flame", "clock", "gear",
                            "sun",   "plus", "check", "warn",  "save"};
    for (int i = 0; i < 15; ++i) {
        UINode& cell = Panel(doc, grid.Id, "Slot", UIColorFromHex("#1B2029"), 10.0f);
        UIBorder& cb = cell.Ensure<UIBorder>();
        cb.Thickness = UIEdges::Uniform(1.0f);
        cb.Color = i == 2 ? UIColorFromHex("#F2C230") : UIColorFromHex("#262D3A");
        UIInteraction& ia = cell.Ensure<UIInteraction>();
        ia.Hit = UIHitShape::RoundedRect;
        ia.Command = "inventory.slot";
        if (i < 10) {
            UINode& icon = Add(doc, cell.Id, "Icon");
            UITransform& it = icon.Ensure<UITransform>();
            it.AnchorMin = it.AnchorMax = {0.5f, 0.5f};
            it.Pivot = {0.5f, 0.5f};
            it.Size = {44.0f, 44.0f};
            UIIcon& ic = icon.Ensure<UIIcon>();
            ic.Name = kIcons[i % 10];
            ic.Color = UIColorFromHex("#C6CCD8");

            UINode& count = Label(doc, cell.Id, "Count", std::to_string((i + 1) * 3), 14.0f,
                                  UIColorFromHex("#9AA3B2"));
            UITransform& ct = count.Ensure<UITransform>();
            ct.AnchorMin = ct.AnchorMax = {1.0f, 1.0f};
            ct.Pivot = {1.0f, 1.0f};
            ct.Offset = {-8.0f, -6.0f};
        }
    }

    // Карточка предмета: маска со скруглением на обложке и вложенные группы.
    UINode& card = Panel(doc, body.Id, "Details", UIColorFromHex("#10141C"), 12.0f);
    UITransform& dt = card.Ensure<UITransform>();
    dt.Size.x = 300.0f;
    UILayout& dl = card.Ensure<UILayout>();
    dl.Kind = UILayout::Mode::Vertical;
    dl.Padding = UIEdges::Uniform(16.0f);
    dl.Gap = {0.0f, 12.0f};
    dl.Cross = UIAlign::Stretch;

    UINode& cover = Add(doc, card.Id, "Cover");
    cover.Ensure<UITransform>().Size.y = 150.0f;
    UIMask& mask = cover.Ensure<UIMask>();
    mask.Form = UIMask::Shape::RoundedRect;
    mask.Radius = UICorners(10.0f);
    UINode& coverFill = Add(doc, cover.Id, "Gradient");
    coverFill.Ensure<UITransform>().SetStretch(true, true);
    UIFill& cf = coverFill.Ensure<UIFill>();
    cf.Type = UIFill::Kind::Gradient;
    cf.Gradient.Type = UIGradient::Kind::Linear;
    cf.Gradient.Angle = 135.0f;
    cf.Gradient.Stops = {{0.0f, UIColorFromHex("#3B2E52")}, {1.0f, UIColorFromHex("#7A4A3A")}};
    UINode& bubble = Add(doc, cover.Id, "Bubble");
    UITransform& bt = bubble.Ensure<UITransform>();
    bt.AnchorMin = bt.AnchorMax = {1.0f, 0.0f};
    bt.Pivot = {0.5f, 0.5f};
    bt.Size = {150.0f, 150.0f};
    UIShape& bs = bubble.Ensure<UIShape>();
    bs.Type = UIShape::Kind::Circle;
    bs.Color = UIColor(1.0f, 1.0f, 1.0f, 0.10f);

    Label(doc, card.Id, "Name", "Огниво странника", 22.0f, UIColorFromHex("#ECEFF4"));
    UINode& desc = Add(doc, card.Id, "Desc");
    UITransform& det = desc.Ensure<UITransform>();
    det.WidthMode = UISizeMode::Stretch;
    det.HeightMode = UISizeMode::Content;
    UIText& dtx = desc.Ensure<UIText>();
    dtx.Text = "Разжигает костёр даже под дождём. Долгое описание переносится по словам "
               "и обрезается многоточием после четвёртой строки.";
    dtx.Size = 15.0f;
    dtx.Color = UIColorFromHex("#9AA3B2");
    dtx.Wrap = UITextWrap::Word;
    dtx.MaxLines = 4;
    dtx.Overflow = UITextOverflow::Ellipsis;

    UINode& stats = Add(doc, card.Id, "Stats");
    stats.Ensure<UITransform>().HeightMode = UISizeMode::Content;
    UILayout& sl = stats.Ensure<UILayout>();
    sl.Kind = UILayout::Mode::Vertical;
    sl.Gap = {0.0f, 6.0f};
    sl.Padding = UIEdges::Uniform(0.0f);
    sl.Cross = UIAlign::Stretch;
    sl.FitHeight = true;
    const char* kStats[][2] = {{"Вес", "0.4 кг"}, {"Прочность", "82%"}, {"Ценность", "17"}};
    for (const auto& st : kStats) {
        UINode& r = Add(doc, stats.Id, "Stat");
        r.Ensure<UITransform>().Size.y = 22.0f;
        UILayout& rl = r.Ensure<UILayout>();
        rl.Kind = UILayout::Mode::Horizontal;
        rl.Padding = UIEdges::Uniform(0.0f);
        rl.Main = UIAlign::SpaceBetween;
        rl.Cross = UIAlign::Center;
        Label(doc, r.Id, "Key", st[0], 15.0f, UIColorFromHex("#6E7787"));
        Label(doc, r.Id, "Value", st[1], 15.0f, UIColorFromHex("#C6CCD8"));
    }

    UINode& gap2 = Add(doc, card.Id, "Spacer");
    gap2.Ensure<UITransform>().HeightMode = UISizeMode::Stretch;
    MenuButton(doc, card.Id, "Использовать", "inventory.use", true);

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

// ---------------------------------------------------------------------------
// ДИАЛОГ
// ---------------------------------------------------------------------------
void UIBuildDialogue(UIDocument& doc, UITheme& theme) {
    UINode& root = Screen(doc, theme, "Dialogue");

    // Затемнение сцены: полупрозрачная плёнка на весь экран.
    UINode& dim = Add(doc, root.Id, "Dim");
    dim.Ensure<UITransform>().SetStretch(true, true);
    dim.Ensure<UIFill>().Color = UIColor(0.0f, 0.0f, 0.0f, 0.45f);

    UINode& box = Panel(doc, root.Id, "Box", UIColorFromHex("#0F131BF2"), 16.0f);
    UITransform& bt = box.Ensure<UITransform>();
    bt.AnchorMin = {0.0f, 1.0f};
    bt.AnchorMax = {1.0f, 1.0f};
    bt.WidthMode = UISizeMode::Stretch;
    bt.Margin = UIEdges(160.0f, 0.0f, 160.0f, 0.0f);
    bt.Pivot = {0.0f, 1.0f};
    bt.Offset = {0.0f, -56.0f};
    bt.Size.y = 260.0f;
    UIBorder& bb = box.Ensure<UIBorder>();
    bb.Thickness = UIEdges::Uniform(1.0f);
    bb.Color = UIColorFromHex("#2B3242");
    box.Ensure<UIEffects>().Ensure<UIDropShadow>().Blur = 36.0f;
    UILayout& bl = box.Ensure<UILayout>();
    bl.Kind = UILayout::Mode::Horizontal;
    bl.Padding = UIEdges::Uniform(22.0f);
    bl.Gap = {20.0f, 0.0f};
    // Start, а не Stretch: портрет обязан остаться КРУГЛЫМ. Растяжение по
    // поперечной оси задало бы ему высоту строки, и эллиптическая маска
    // превратила бы круг в овал — молча и каждый раз по-разному.
    bl.Cross = UIAlign::Start;

    // Круглый портрет — эллиптическая маска, которую прямоугольным clip не
    // сделать вовсе.
    UINode& portrait = Add(doc, box.Id, "Portrait");
    UITransform& pt = portrait.Ensure<UITransform>();
    pt.Size = {150.0f, 150.0f};
    pt.HeightMode = UISizeMode::Fixed;
    UIMask& pmask = portrait.Ensure<UIMask>();
    pmask.Form = UIMask::Shape::Ellipse;
    UINode& pfill = Add(doc, portrait.Id, "Gradient");
    pfill.Ensure<UITransform>().SetStretch(true, true);
    UIFill& pf = pfill.Ensure<UIFill>();
    pf.Type = UIFill::Kind::Gradient;
    pf.Gradient.Type = UIGradient::Kind::Linear;
    pf.Gradient.Angle = 160.0f;
    pf.Gradient.Stops = {{0.0f, UIColorFromHex("#2B4360")}, {1.0f, UIColorFromHex("#6B3E5C")}};
    UINode& pIcon = Add(doc, portrait.Id, "Icon");
    UITransform& pit = pIcon.Ensure<UITransform>();
    pit.AnchorMin = pit.AnchorMax = {0.5f, 0.5f};
    pit.Pivot = {0.5f, 0.5f};
    pit.Size = {80.0f, 80.0f};
    UIIcon& pic = pIcon.Ensure<UIIcon>();
    pic.Name = "lamp";
    pic.Color = UIColor(1.0f, 1.0f, 1.0f, 0.85f);

    UINode& text = Add(doc, box.Id, "Text");
    UITransform& txt = text.Ensure<UITransform>();
    txt.WidthMode = UISizeMode::Stretch;
    txt.HeightMode = UISizeMode::Stretch;
    UILayout& tl = text.Ensure<UILayout>();
    tl.Kind = UILayout::Mode::Vertical;
    tl.Gap = {0.0f, 10.0f};
    tl.Padding = UIEdges::Uniform(0.0f);
    tl.Cross = UIAlign::Stretch;

    Label(doc, text.Id, "Speaker", "Смотритель маяка", 22.0f, UIColorFromHex("#F2C230"));
    UINode& line = Add(doc, text.Id, "Line");
    UITransform& lt = line.Ensure<UITransform>();
    lt.WidthMode = UISizeMode::Stretch;
    lt.HeightMode = UISizeMode::Stretch;
    UIText& ltx = line.Ensure<UIText>();
    ltx.Text = "Свет гаснет третью ночь подряд. Если поднимешься наверх и починишь линзу, "
               "я расскажу, что видел в тумане.";
    ltx.Size = 20.0f;
    ltx.Color = UIColorFromHex("#DCE2EC");
    ltx.Wrap = UITextWrap::Word;
    ltx.LineSpacing = 1.35f;

    UINode& answers = Add(doc, text.Id, "Answers");
    // Высота ответов задана ЧИСЛОМ, а не «по содержимому»: в колонке выше стоит
    // растянутая реплика, она забирает всю свободную высоту, и ряд ответов
    // получал нулевую — кнопки вылезали за нижний край окна.
    UITransform& at = answers.Ensure<UITransform>();
    at.HeightMode = UISizeMode::Fixed;
    at.Size.y = 46.0f;
    UILayout& al = answers.Ensure<UILayout>();
    al.Kind = UILayout::Mode::Horizontal;
    al.Gap = {12.0f, 0.0f};
    al.Padding = UIEdges::Uniform(0.0f);
    MenuButton(doc, answers.Id, "Помогу", "dialogue.accept", true);
    MenuButton(doc, answers.Id, "Что за туман?", "dialogue.ask");
    MenuButton(doc, answers.Id, "Не сейчас", "dialogue.decline");
    for (UINodeId child : doc.Find(answers.Id)->Children) {
        if (UINode* b = doc.Find(child)) b->Ensure<UITransform>().Size.y = 46.0f;
    }

    theme.Apply(doc);
    doc.MarkDirty(UIDirty_All);
}

// ---------------------------------------------------------------------------

const std::vector<std::string>& UIDemoNames() {
    static const std::vector<std::string> names = {"menu", "hud", "settings", "inventory",
                                                   "dialogue"};
    return names;
}

bool UIBuildDemo(const std::string& name, UIDocument& doc, UITheme& theme) {
    if (name == "menu") { UIBuildMainMenu(doc, theme); return true; }
    if (name == "hud") { UIBuildHud(doc, theme); return true; }
    if (name == "settings") { UIBuildSettings(doc, theme); return true; }
    if (name == "inventory") { UIBuildInventory(doc, theme); return true; }
    if (name == "dialogue") { UIBuildDialogue(doc, theme); return true; }
    // Неизвестное имя — false, а не пустой документ: «экран собрался и пустой»
    // отлаживается втрое дольше, чем «экран не собрался».
    return false;
}

} // namespace sage::ui
