#include "sage/ui/widgets/UIWidgets.h"

#include <algorithm>
#include <cmath>

#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMask.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIText.h"

namespace sage::ui {

// --- Компоненты поведения ---------------------------------------------------

namespace {
const char* const kProgressDirs[] = {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom",
                                     "Radial"};
}

float UIProgress::Normalized() const {
    const float span = Max - Min;
    return span > 0.0001f ? UIClamp01((Value - Min) / span) : 0.0f;
}

const UIComponentType& UIProgress::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "progress";
        d.Title = SAGE_UI_TEXT("Progress");
        d.Hint = SAGE_UI_TEXT("Displays a value; does not let the user change it");
        d.Icon = "bar";
        d.Category = UIComponentCategory::Appearance;
        d.Order = 30;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIProgress()); };
        d.Props = {
            {"Value", SAGE_UI_TEXT("Value"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIProgress, Value), 0.0f, 1.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Min", SAGE_UI_TEXT("Min"), UIProperty::Kind::Float, SAGE_UI_OFFSET(UIProgress, Min),
             0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Max", SAGE_UI_TEXT("Max"), UIProperty::Kind::Float, SAGE_UI_OFFSET(UIProgress, Max),
             0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Grow", SAGE_UI_TEXT("Direction"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIProgress, Grow), 0, 0, nullptr, kProgressDirs, 5,
             UIProperty::Widget::Auto, nullptr},
            {"FillColor", SAGE_UI_TEXT("Fill color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIProgress, FillColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"TrackColor", SAGE_UI_TEXT("Track color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIProgress, TrackColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIProgress, Radius), 0.0f, 100.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Padding", SAGE_UI_TEXT("Padding"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIProgress, Padding), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Smoothing", SAGE_UI_TEXT("Smoothing"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIProgress, Smoothing), 0.0f, 10.0f,
             SAGE_UI_TEXT("Units per second; zero snaps instantly"), nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
        };
        return d;
    }();
    return t;
}

float UIRangeValue::Normalized() const {
    const float span = Max - Min;
    return span > 0.0001f ? UIClamp01((Value - Min) / span) : 0.0f;
}

void UIRangeValue::SetNormalized(float t) {
    float v = Min + (Max - Min) * UIClamp01(t);
    // Прилипание к шагу считается ЗДЕСЬ, а не у того, кто тянет ползунок:
    // иначе клавиатура, мышь и скрипт округляют по-разному.
    if (Step > 0.0f) v = Min + std::round((v - Min) / Step) * Step;
    if (Toggle) v = v >= (Min + Max) * 0.5f ? Max : Min;
    Value = std::min(Max, std::max(Min, v));
}

const UIComponentType& UIRangeValue::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "range";
        d.Title = SAGE_UI_TEXT("Range");
        d.Hint = SAGE_UI_TEXT("A number the user can change: slider, checkbox, stepper");
        d.Icon = "slider";
        d.Category = UIComponentCategory::Interaction;
        d.Order = 35;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIRangeValue()); };
        d.Props = {
            {"Value", SAGE_UI_TEXT("Value"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIRangeValue, Value), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Min", SAGE_UI_TEXT("Min"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIRangeValue, Min), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Max", SAGE_UI_TEXT("Max"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIRangeValue, Max), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Step", SAGE_UI_TEXT("Step"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIRangeValue, Step), 0.0f, 1.0f,
             SAGE_UI_TEXT("Zero means continuous"), nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Vertical", SAGE_UI_TEXT("Vertical"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIRangeValue, Vertical), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Toggle", SAGE_UI_TEXT("Toggle"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIRangeValue, Toggle), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"TrackColor", SAGE_UI_TEXT("Track color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIRangeValue, TrackColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"AccentColor", SAGE_UI_TEXT("Accent color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIRangeValue, AccentColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"HandleSize", SAGE_UI_TEXT("Handle size"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIRangeValue, HandleSize), 4.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIRangeValue, Radius), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIComponentType& UITextField::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "textfield";
        d.Title = SAGE_UI_TEXT("Text field");
        d.Hint = SAGE_UI_TEXT("Editable text with a caret");
        d.Icon = "input";
        d.Category = UIComponentCategory::Interaction;
        d.Order = 62;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UITextField()); };
        d.Props = {
            {"Value", SAGE_UI_TEXT("Value"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UITextField, Value), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"PlaceholderKey", SAGE_UI_TEXT("Placeholder key"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UITextField, PlaceholderKey), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"MaxLength", SAGE_UI_TEXT("Max length"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UITextField, MaxLength), 0.0f, 512.0f,
             SAGE_UI_TEXT("In characters, not bytes"), nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Password", SAGE_UI_TEXT("Password"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UITextField, Password), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"ReadOnly", SAGE_UI_TEXT("Read only"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UITextField, ReadOnly), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Multiline", SAGE_UI_TEXT("Multiline"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UITextField, Multiline), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"SelectAllOnFocus", SAGE_UI_TEXT("Select all on focus"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UITextField, SelectAllOnFocus), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIComponentType& UIScrollView::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "scroll";
        d.Title = SAGE_UI_TEXT("Scroll view");
        d.Hint = SAGE_UI_TEXT("Viewport, content size and offset — nothing about the content");
        d.Icon = "scroll";
        d.Category = UIComponentCategory::Layout;
        d.Order = 6;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIScrollView()); };
        d.Props = {
            {"Offset", SAGE_UI_TEXT("Offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIScrollView, Offset), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Horizontal", SAGE_UI_TEXT("Horizontal"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIScrollView, Horizontal), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Vertical", SAGE_UI_TEXT("Vertical"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIScrollView, Vertical), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Speed", SAGE_UI_TEXT("Speed"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIScrollView, Speed), 1.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Damping", SAGE_UI_TEXT("Damping"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIScrollView, Damping), 0.0f, 40.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Elastic", SAGE_UI_TEXT("Elastic"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIScrollView, Elastic), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIComponentType& UISelection::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "selection";
        d.Title = SAGE_UI_TEXT("Selection");
        d.Hint = SAGE_UI_TEXT("Index of the selected child — tabs, radio groups");
        d.Icon = "list";
        d.Category = UIComponentCategory::Interaction;
        d.Order = 36;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UISelection()); };
        d.Props = {
            {"Index", SAGE_UI_TEXT("Index"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UISelection, Index), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"AllowNone", SAGE_UI_TEXT("Allow none"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UISelection, AllowNone), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Group", SAGE_UI_TEXT("Group"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UISelection, Group), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

void RegisterBuiltinUIWidgetComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    UIComponentRegistry& r = UIComponentRegistry::Instance();
    r.Register(UIProgress::StaticType());
    r.Register(UIRangeValue::StaticType());
    r.Register(UITextField::StaticType());
    r.Register(UIScrollView::StaticType());
    r.Register(UISelection::StaticType());
}

// --- Сборка виджетов --------------------------------------------------------
//
// Виджет — это РЕЦЕПТ из обычных узлов. Собранная кнопка ничем не отличается от
// кнопки, собранной руками в редакторе: её можно разобрать, переставить части,
// добавить свои. Именно поэтому «встроенный виджет» здесь не ограничивает —
// он лишь избавляет от десяти кликов.

namespace {
UINode& NewNode(UIDocument& doc, UINodeId parent, const char* name) {
    return *doc.Create(name, parent);
}
} // namespace

UINodeId UIMakePanel(UIDocument& doc, UINodeId parent, const std::string& name) {
    UINode& n = NewNode(doc, parent, name.c_str());
    UITransform& t = n.Ensure<UITransform>();
    t.Size = {320.0f, 200.0f};
    UIFill& f = n.Ensure<UIFill>();
    f.Color = UIColorFromHex("#1A1D25E6");
    f.Radius = UICorners(10.0f);
    n.Ensure<UIStyled>().Style = "Panel";
    return n.Id;
}

UINodeId UIMakeLabel(UIDocument& doc, UINodeId parent, const std::string& text) {
    UINode& n = NewNode(doc, parent, "Label");
    UITransform& t = n.Ensure<UITransform>();
    // Надпись по умолчанию меряется по содержимому: подпись, у которой ширина
    // задана числом, обрезается на первом же переводе на другой язык.
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIText& tx = n.Ensure<UIText>();
    tx.Text = text;
    tx.Wrap = UITextWrap::None;
    n.Ensure<UIStyled>().Style = "Label";
    return n.Id;
}

UINodeId UIMakeImage(UIDocument& doc, UINodeId parent, const std::string& path) {
    UINode& n = NewNode(doc, parent, "Image");
    n.Ensure<UITransform>().Size = {128.0f, 128.0f};
    UIImage& img = n.Ensure<UIImage>();
    img.Path = path;
    img.Fit = UIImageFit::Contain;
    return n.Id;
}

UINodeId UIMakeButton(UIDocument& doc, UINodeId parent, const std::string& text,
                      const std::string& command) {
    UINode& n = NewNode(doc, parent, "Button");
    UITransform& t = n.Ensure<UITransform>();
    t.Size = {180.0f, 44.0f};
    UIFill& f = n.Ensure<UIFill>();
    f.Color = UIColorFromHex("#232833");
    f.Radius = UICorners(10.0f);
    UIBorder& b = n.Ensure<UIBorder>();
    b.Thickness = UIEdges::Uniform(1.0f);
    b.Color = UIColorFromHex("#333A47");
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Hit = UIHitShape::RoundedRect;
    ia.Command = command;
    n.Ensure<UIStyled>().Style = "Button";

    // ПОДПИСЬ — ОТДЕЛЬНЫЙ УЗЕЛ, а не поле кнопки. Её видно в дереве, её можно
    // подвинуть, покрасить, заменить картинкой или убрать совсем; можно
    // добавить вторую. Ровно этого от заготовок и ждут.
    UINode& label = NewNode(doc, n.Id, "Text");
    UITransform& lt = label.Ensure<UITransform>();
    lt.SetStretch(true, true);
    UIText& tx = label.Ensure<UIText>();
    tx.Text = text;
    tx.Align = UITextAlign::Center;
    tx.VAlign = UITextVAlign::Center;
    return n.Id;
}

UINodeId UIMakeCheckbox(UIDocument& doc, UINodeId parent, const std::string& text) {
    UINode& n = NewNode(doc, parent, "Checkbox");
    UITransform& t = n.Ensure<UITransform>();
    t.Size = {200.0f, 28.0f};
    UILayout& layout = n.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Horizontal;
    layout.Gap = {8.0f, 0.0f};
    layout.Padding = UIEdges::Uniform(0.0f);
    layout.Cross = UIAlign::Center;
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Focusable = true;

    UINode& box = NewNode(doc, n.Id, "Box");
    box.Ensure<UITransform>().Size = {24.0f, 24.0f};
    UIRangeValue& range = box.Ensure<UIRangeValue>();
    range.Toggle = true;
    range.Step = 1.0f;
    range.Value = 0.0f;

    UINode& label = NewNode(doc, n.Id, "Text");
    UITransform& lt = label.Ensure<UITransform>();
    lt.WidthMode = UISizeMode::Content;
    lt.HeightMode = UISizeMode::Content;
    label.Ensure<UIText>().Text = text;
    return n.Id;
}

UINodeId UIMakeSlider(UIDocument& doc, UINodeId parent) {
    UINode& n = NewNode(doc, parent, "Slider");
    n.Ensure<UITransform>().Size = {220.0f, 28.0f};
    n.Ensure<UIRangeValue>();
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Draggable = true;
    return n.Id;
}

UINodeId UIMakeProgress(UIDocument& doc, UINodeId parent) {
    UINode& n = NewNode(doc, parent, "Progress");
    n.Ensure<UITransform>().Size = {220.0f, 16.0f};
    UIProgress& p = n.Ensure<UIProgress>();
    p.Value = 0.6f;
    p.Radius = UICorners(8.0f);
    return n.Id;
}

UINodeId UIMakeInputField(UIDocument& doc, UINodeId parent, const std::string& placeholder) {
    UINode& n = NewNode(doc, parent, "InputField");
    n.Ensure<UITransform>().Size = {240.0f, 36.0f};
    UIFill& f = n.Ensure<UIFill>();
    f.Color = UIColorFromHex("#12141A");
    f.Radius = UICorners(6.0f);
    UIBorder& b = n.Ensure<UIBorder>();
    b.Thickness = UIEdges::Uniform(1.0f);
    b.Color = UIColorFromHex("#333A47");
    UIText& t = n.Ensure<UIText>();
    t.Padding = UIEdges(8.0f, 6.0f, 8.0f, 6.0f);
    t.VAlign = UITextVAlign::Center;
    UITextField& field = n.Ensure<UITextField>();
    field.PlaceholderKey = placeholder;
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Hit = UIHitShape::RoundedRect;
    n.Ensure<UIStyled>().Style = "InputField";
    return n.Id;
}

UINodeId UIMakeScrollView(UIDocument& doc, UINodeId parent) {
    UINode& n = NewNode(doc, parent, "ScrollView");
    n.Ensure<UITransform>().Size = {320.0f, 240.0f};
    n.Ensure<UIScrollView>();
    // Маска — обязательная часть прокрутки: без неё содержимое вылезает за
    // рамки окна, и «прокрутка» выглядит как сдвинутый список.
    UIMask& mask = n.Ensure<UIMask>();
    mask.Form = UIMask::Shape::RoundedRect;
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.ScrollTarget = true;

    UINode& content = NewNode(doc, n.Id, "Content");
    UITransform& ct = content.Ensure<UITransform>();
    ct.SetStretch(true, false);
    ct.HeightMode = UISizeMode::Content;
    UILayout& layout = content.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Vertical;
    layout.FitHeight = true;
    return n.Id;
}

UINodeId UIMakeList(UIDocument& doc, UINodeId parent) {
    UINode& n = NewNode(doc, parent, "List");
    n.Ensure<UITransform>().Size = {280.0f, 200.0f};
    UILayout& layout = n.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Vertical;
    layout.Cross = UIAlign::Stretch;
    return n.Id;
}

UINodeId UIMakeTabs(UIDocument& doc, UINodeId parent, const std::vector<std::string>& titles) {
    UINode& n = NewNode(doc, parent, "Tabs");
    UITransform& t = n.Ensure<UITransform>();
    t.Size = {480.0f, 40.0f};
    UILayout& layout = n.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Horizontal;
    layout.Gap = {4.0f, 0.0f};
    layout.Padding = UIEdges::Uniform(0.0f);
    n.Ensure<UISelection>();
    for (size_t i = 0; i < titles.size(); ++i) {
        const UINodeId tab = UIMakeButton(doc, n.Id, titles[i], "tab." + std::to_string(i));
        if (UINode* tn = doc.Find(tab)) {
            tn->Name = "Tab";
            tn->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
        }
    }
    return n.Id;
}

UINodeId UIMakeDropdown(UIDocument& doc, UINodeId parent, const std::vector<std::string>& options) {
    UINode& n = NewNode(doc, parent, "Dropdown");
    n.Ensure<UITransform>().Size = {220.0f, 36.0f};
    UIFill& f = n.Ensure<UIFill>();
    f.Color = UIColorFromHex("#232833");
    f.Radius = UICorners(6.0f);
    UIInteraction& ia = n.Ensure<UIInteraction>();
    ia.Focusable = true;
    n.Ensure<UISelection>();

    UINode& label = NewNode(doc, n.Id, "Text");
    label.Ensure<UITransform>().SetStretch(true, true);
    UIText& tx = label.Ensure<UIText>();
    tx.Text = options.empty() ? std::string() : options.front();
    tx.VAlign = UITextVAlign::Center;
    tx.Padding = UIEdges(8.0f, 0.0f, 24.0f, 0.0f);

    // Список вариантов — обычное поддерево, спрятанное до нажатия. Не «особый
    // режим выпадающего списка»: его можно оформить, замаскировать и
    // прокрутить как любое другое.
    UINode& list = NewNode(doc, n.Id, "Options");
    list.Visible = false;
    list.Layer = 40; // выше соседей: список обязан лечь поверх
    UITransform& lt = list.Ensure<UITransform>();
    lt.AnchorMin = lt.AnchorMax = {0.0f, 1.0f};
    lt.Pivot = {0.0f, 0.0f};
    lt.WidthMode = UISizeMode::Percent;
    lt.HeightMode = UISizeMode::Content;
    UILayout& ll = list.Ensure<UILayout>();
    ll.Kind = UILayout::Mode::Vertical;
    ll.FitHeight = true;
    ll.Cross = UIAlign::Stretch;
    UIFill& lf = list.Ensure<UIFill>();
    lf.Color = UIColorFromHex("#12141AF2");
    lf.Radius = UICorners(6.0f);
    for (const std::string& o : options) {
        const UINodeId item = UIMakeButton(doc, list.Id, o, {});
        if (UINode* in = doc.Find(item)) in->Name = "Option";
    }
    return n.Id;
}

UINodeId UIMakeTooltip(UIDocument& doc, UINodeId parent, const std::string& text) {
    UINode& n = NewNode(doc, parent, "Tooltip");
    n.Layer = 50; // §25: подсказки выше меню и всплывающих окон
    n.Visible = false;
    UITransform& t = n.Ensure<UITransform>();
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIFill& f = n.Ensure<UIFill>();
    f.Color = UIColorFromHex("#0C0E13F5");
    f.Radius = UICorners(6.0f);
    UILayout& layout = n.Ensure<UILayout>();
    layout.Kind = UILayout::Mode::Vertical;
    layout.FitWidth = layout.FitHeight = true;
    layout.Padding = UIEdges::Uniform(8.0f);
    UIMakeLabel(doc, n.Id, text);
    return n.Id;
}

void RegisterBuiltinUIWidgets() {
    static bool done = false;
    if (done) return;
    done = true;
    RegisterBuiltinUIWidgetComponents();
    UIWidgetRegistry& r = UIWidgetRegistry::Instance();

    auto add = [&](const char* id, const char* title, const char* category, const char* hint,
                   std::function<UINodeId(UIDocument&, UINodeId)> build) {
        UIWidgetType t;
        t.Id = id;
        t.Title = title;
        t.Category = category;
        t.Hint = hint;
        t.Build = std::move(build);
        r.Register(std::move(t));
    };

    add("empty", SAGE_UI_TEXT("Empty node"), SAGE_UI_TEXT("Basic"),
        SAGE_UI_TEXT("A node with nothing but a rectangle"),
        [](UIDocument& d, UINodeId p) { return d.Create("Node", p)->Id; });
    add("panel", SAGE_UI_TEXT("Panel"), SAGE_UI_TEXT("Basic"), SAGE_UI_TEXT("Background surface"),
        [](UIDocument& d, UINodeId p) { return UIMakePanel(d, p); });
    add("label", SAGE_UI_TEXT("Label"), SAGE_UI_TEXT("Basic"), SAGE_UI_TEXT("A line of text"),
        [](UIDocument& d, UINodeId p) { return UIMakeLabel(d, p, "Text"); });
    add("image", SAGE_UI_TEXT("Image"), SAGE_UI_TEXT("Basic"), SAGE_UI_TEXT("Texture or sprite"),
        [](UIDocument& d, UINodeId p) { return UIMakeImage(d, p, ""); });
    add("button", SAGE_UI_TEXT("Button"), SAGE_UI_TEXT("Input"),
        SAGE_UI_TEXT("Reports a click; the game decides what it means"),
        [](UIDocument& d, UINodeId p) { return UIMakeButton(d, p, "Button", ""); });
    add("checkbox", SAGE_UI_TEXT("Checkbox"), SAGE_UI_TEXT("Input"), SAGE_UI_TEXT("Toggle"),
        [](UIDocument& d, UINodeId p) { return UIMakeCheckbox(d, p, "Option"); });
    add("slider", SAGE_UI_TEXT("Slider"), SAGE_UI_TEXT("Input"), SAGE_UI_TEXT("Editable number"),
        [](UIDocument& d, UINodeId p) { return UIMakeSlider(d, p); });
    add("progress", SAGE_UI_TEXT("Progress bar"), SAGE_UI_TEXT("Basic"),
        SAGE_UI_TEXT("Shows a value"), [](UIDocument& d, UINodeId p) { return UIMakeProgress(d, p); });
    add("input", SAGE_UI_TEXT("Input field"), SAGE_UI_TEXT("Input"), SAGE_UI_TEXT("Editable text"),
        [](UIDocument& d, UINodeId p) { return UIMakeInputField(d, p, ""); });
    add("scroll", SAGE_UI_TEXT("Scroll view"), SAGE_UI_TEXT("Containers"),
        SAGE_UI_TEXT("Masked, scrollable area"),
        [](UIDocument& d, UINodeId p) { return UIMakeScrollView(d, p); });
    add("list", SAGE_UI_TEXT("List"), SAGE_UI_TEXT("Containers"),
        SAGE_UI_TEXT("Vertical layout container"),
        [](UIDocument& d, UINodeId p) { return UIMakeList(d, p); });
    add("tabs", SAGE_UI_TEXT("Tabs"), SAGE_UI_TEXT("Containers"), SAGE_UI_TEXT("Row of tabs"),
        [](UIDocument& d, UINodeId p) {
            return UIMakeTabs(d, p, {"One", "Two", "Three"});
        });
    add("dropdown", SAGE_UI_TEXT("Dropdown"), SAGE_UI_TEXT("Input"), SAGE_UI_TEXT("Option picker"),
        [](UIDocument& d, UINodeId p) {
            return UIMakeDropdown(d, p, {"Option A", "Option B"});
        });
    add("tooltip", SAGE_UI_TEXT("Tooltip"), SAGE_UI_TEXT("Basic"),
        SAGE_UI_TEXT("Hidden hint panel on a high layer"),
        [](UIDocument& d, UINodeId p) { return UIMakeTooltip(d, p, "Hint"); });
}

// --- Шаг поведения ----------------------------------------------------------

void UIUpdateWidgets(UIDocument& doc, float dt) {
    if (dt <= 0.0f) return;
    for (UINodeId id : doc.Ordered()) {
        UINode* node = doc.Find(id);
        if (!node) continue;

        if (UIProgress* p = node->Get<UIProgress>()) {
            const float target = p->Normalized();
            if (p->Displayed < 0.0f || p->Smoothing <= 0.0f) {
                p->Displayed = target;
            } else if (p->Displayed != target) {
                const float step = p->Smoothing * dt;
                const float diff = target - p->Displayed;
                p->Displayed += std::fabs(diff) <= step ? diff : (diff > 0 ? step : -step);
                doc.MarkDirty(UIDirty_Visual);
            }
        }
        if (UITextField* f = node->Get<UITextField>()) {
            f->Blink += dt;
            if (f->Blink >= 1.0f) f->Blink -= 1.0f;
            // Каретка не может стоять внутри многобайтового символа: иначе
            // следующая вставка разрежет букву пополам.
            f->Caret = std::max(0, std::min(f->Caret, (int)f->Value.size()));
        }
        if (UIScrollView* sv = node->Get<UIScrollView>()) {
            if (sv->Velocity != glm::vec2(0.0f)) {
                sv->Offset += sv->Velocity * dt;
                const float k = std::max(0.0f, 1.0f - sv->Damping * dt);
                sv->Velocity *= k;
                if (glm::length(sv->Velocity) < 1.0f) sv->Velocity = glm::vec2(0.0f);
                doc.MarkDirty(UIDirty_Layout);
            }
            // Границы прокрутки: контент меньше окна — смещения нет вовсе.
            const glm::vec2 maxOffset = glm::max(glm::vec2(0.0f), sv->ContentSize);
            sv->Offset.x = sv->Horizontal ? std::min(std::max(0.0f, sv->Offset.x), maxOffset.x) : 0.0f;
            sv->Offset.y = sv->Vertical ? std::min(std::max(0.0f, sv->Offset.y), maxOffset.y) : 0.0f;
        }
    }
}

} // namespace sage::ui
