#include "sage/ui/sageui/UIWidgetsOO.h"

#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/sageui/UIRegistry.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIImage.h"

namespace sage::ui::sui {

// --- Panel ------------------------------------------------------------------

void Panel::OnAttach() {
    SetName("Panel");
    Ensure<UIFill>();
    SetStyle("Panel");
}

Panel* Panel::SetColor(const UIColor& color) {
    Ensure<UIFill>().Color = color;
    // Цвет, выставленный руками, обязан пережить применение темы — иначе
    // «покрасил и вернулось обратно» на следующем же кадре.
    Ensure<UIStyled>().SetOverride("fill.Color", true);
    Dirty(UIDirty_Visual);
    return this;
}

Panel* Panel::SetRadius(float radius) {
    Ensure<UIFill>().Radius = UICorners(radius);
    Ensure<UIStyled>().SetOverride("fill.Radius", true);
    Dirty(UIDirty_Visual);
    return this;
}

Panel* Panel::SetBorder(float width, const UIColor& color) {
    UIBorder& b = Ensure<UIBorder>();
    b.Thickness = UIEdges::Uniform(width);
    b.Color = color;
    Dirty(UIDirty_Visual);
    return this;
}

// --- Label ------------------------------------------------------------------

Label::Label(std::string text) : m_initial(std::move(text)) {}

void Label::OnAttach() {
    SetName("Label");
    UITransform& t = Ensure<UITransform>();
    // Надпись по умолчанию размером с текст: подпись, растянутая на всю
    // строку, ломает любую раскладку рядом с собой.
    t.WidthMode = UISizeMode::Content;
    t.HeightMode = UISizeMode::Content;
    UIText& tx = Ensure<UIText>();
    tx.Text = m_initial;
    tx.Wrap = UITextWrap::None;
    SetStyle("Label");
}

Label* Label::SetText(const std::string& text) {
    UIText& t = Ensure<UIText>();
    if (t.Text == text) return this;
    t.Text = text;
    // Ключ перевода сбрасывается: текст, поставленный кодом, — это готовая
    // строка, и переводить её второй раз нечем.
    t.Key.clear();
    Dirty(UIDirty_Text | UIDirty_Layout);
    return this;
}

const std::string& Label::Text() const {
    static const std::string kEmpty;
    const UIText* t = Get<UIText>();
    return t ? t->Text : kEmpty;
}

Label* Label::SetFontSize(float size) {
    Ensure<UIText>().Size = size;
    Ensure<UIStyled>().SetOverride("text.Size", true);
    Dirty(UIDirty_Text | UIDirty_Layout);
    return this;
}

Label* Label::SetColor(const UIColor& color) {
    Ensure<UIText>().Color = color;
    Ensure<UIStyled>().SetOverride("text.Color", true);
    Dirty(UIDirty_Visual);
    return this;
}

Label* Label::SetAlign(UITextAlign align, UITextVAlign vertical) {
    UIText& t = Ensure<UIText>();
    t.Align = align;
    t.VAlign = vertical;
    Dirty(UIDirty_Text | UIDirty_Layout);
    return this;
}

Label* Label::SetWrap(bool wrap) {
    UIText& t = Ensure<UIText>();
    t.Wrap = wrap ? UITextWrap::Word : UITextWrap::None;
    // Перенос без ширины бессмыслен: переносить не по чему. Ширину берём у
    // родителя, а высоту оставляем по содержимому — так текст растёт вниз.
    if (wrap) {
        UITransform& xf = Ensure<UITransform>();
        if (xf.WidthMode == UISizeMode::Content) xf.WidthMode = UISizeMode::Stretch;
    }
    Dirty(UIDirty_Text | UIDirty_Layout);
    return this;
}

Label* Label::SetEllipsis(bool on) {
    Ensure<UIText>().Overflow = on ? UITextOverflow::Ellipsis : UITextOverflow::Clip;
    Dirty(UIDirty_Text);
    return this;
}

// --- Button -----------------------------------------------------------------

Button::Button(std::string text, std::string command)
    : m_text(std::move(text)), m_command(std::move(command)) {}

void Button::OnAttach() {
    SetName(m_text.empty() ? "Button" : m_text);
    UITransform& t = Ensure<UITransform>();
    t.Size = {160.0f, 32.0f};

    Ensure<UIFill>();
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    ia.Focusable = true;
    ia.Command = m_command;
    SetStyle("Button");

    // Подпись — ОТДЕЛЬНЫЙ узел, а не поле кнопки. Так её можно подвинуть,
    // покрасить, заменить значком или убрать совсем, не заводя у кнопки
    // десяток полей «про текст».
    m_caption = Ctx().Create<Label>(m_text);
    Add(m_caption);
    m_caption->SetName("Caption");
    m_caption->SetStretch(true, true);
    m_caption->SetAlign(UITextAlign::Center, UITextVAlign::Center);
    m_caption->SetStyle("ButtonLabel");
}

Button* Button::SetText(const std::string& text) {
    m_text = text;
    if (m_caption) m_caption->SetText(text);
    return this;
}

Button* Button::SetCommand(const std::string& command) {
    m_command = command;
    Ensure<UIInteraction>().Command = command;
    return this;
}

// --- Checkbox ---------------------------------------------------------------

Checkbox::Checkbox(std::string text, bool checked)
    : m_text(std::move(text)), m_checked(checked) {}

void Checkbox::OnAttach() {
    SetName(m_text.empty() ? "Checkbox" : m_text);
    const UINodeId id = UIMakeCheckbox(Doc(), Parent() ? Parent()->NodeId() : kUIInvalidNode,
                                       m_text);
    // Заготовка ядра собрала узел со всеми частями; переносим их себе, чтобы
    // элемент оставался ОДНИМ узлом. Иначе у элемента был бы «свой» узел и
    // «настоящий» рядом, и любая правка попадала бы не туда.
    Doc().Destroy(id);

    UITransform& t = Ensure<UITransform>();
    t.Size = {180.0f, 24.0f};
    Ensure<UIFill>();
    UIRangeValue& r = Ensure<UIRangeValue>();
    r.Toggle = true;
    r.Min = 0.0f;
    r.Max = 1.0f;
    r.Step = 1.0f;
    r.Value = m_checked ? 1.0f : 0.0f;
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Hit = UIHitShape::Rect;
    SetStyle("Checkbox");

    if (!m_text.empty()) {
        Label* caption = Ctx().Create<Label>(m_text);
        Add(caption);
        caption->SetName("Caption");
        caption->SetPosition({28.0f, 0.0f});
        caption->SetAlign(UITextAlign::Left, UITextVAlign::Center);
    }
}

bool Checkbox::Checked() const {
    const UIRangeValue* r = Get<UIRangeValue>();
    return r && r->Value >= 0.5f;
}

Checkbox* Checkbox::SetChecked(bool on) {
    Ensure<UIRangeValue>().Value = on ? 1.0f : 0.0f;
    Dirty(UIDirty_Visual);
    return this;
}

Checkbox* Checkbox::OnToggle(std::function<void(bool)> fn) {
    On(UIEventType::ValueChanged, [fn = std::move(fn)](UIEvent& e) {
        if (fn) fn(e.Value >= 0.5f);
    });
    return this;
}

// --- Slider -----------------------------------------------------------------

Slider::Slider(float value, float min, float max) : m_value(value), m_min(min), m_max(max) {}

void Slider::OnAttach() {
    SetName("Slider");
    Ensure<UITransform>().Size = {200.0f, 20.0f};
    UIRangeValue& r = Ensure<UIRangeValue>();
    r.Min = m_min;
    r.Max = m_max;
    r.Value = m_value;
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Draggable = true;   // без этого ползунок «нажимается», но не тянется
    SetStyle("Slider");
}

float Slider::Value() const {
    const UIRangeValue* r = Get<UIRangeValue>();
    return r ? r->Value : 0.0f;
}

Slider* Slider::SetValue(float value) {
    Ensure<UIRangeValue>().Value = value;
    Dirty(UIDirty_Visual);
    return this;
}

Slider* Slider::SetRange(float min, float max) {
    UIRangeValue& r = Ensure<UIRangeValue>();
    r.Min = min;
    r.Max = max;
    Dirty(UIDirty_Visual);
    return this;
}

Slider* Slider::SetStep(float step) {
    Ensure<UIRangeValue>().Step = step;
    return this;
}

// --- ProgressBar ------------------------------------------------------------

ProgressBar::ProgressBar(float value) : m_value(value) {}

void ProgressBar::OnAttach() {
    SetName("Progress");
    Ensure<UITransform>().Size = {200.0f, 12.0f};
    Ensure<UIProgress>().Value = m_value;
    SetStyle("Progress");
}

ProgressBar* ProgressBar::SetValue(float value) {
    Ensure<UIProgress>().Value = value;
    Dirty(UIDirty_Visual);
    return this;
}

float ProgressBar::Value() const {
    const UIProgress* p = Get<UIProgress>();
    return p ? p->Value : 0.0f;
}

// --- TextInput --------------------------------------------------------------

TextInput::TextInput(std::string value, std::string placeholder)
    : m_value(std::move(value)), m_placeholder(std::move(placeholder)) {}

void TextInput::OnAttach() {
    SetName("TextInput");
    Ensure<UITransform>().Size = {220.0f, 28.0f};
    Ensure<UIFill>();
    UITextField& f = Ensure<UITextField>();
    f.Value = m_value;
    f.PlaceholderKey = m_placeholder;
    UIText& t = Ensure<UIText>();
    t.Text = m_value;
    t.VAlign = UITextVAlign::Center;
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Focusable = true;
    ia.Cursor = "text";
    SetStyle("Input");
}

const std::string& TextInput::Value() const {
    static const std::string kEmpty;
    const UITextField* f = Get<UITextField>();
    return f ? f->Value : kEmpty;
}

TextInput* TextInput::SetValue(const std::string& value) {
    Ensure<UITextField>().Value = value;
    Ensure<UIText>().Text = value;
    Dirty(UIDirty_Text | UIDirty_Layout);
    return this;
}

TextInput* TextInput::OnSubmit(std::function<void(const std::string&)> fn) {
    On(UIEventType::Submit, [this, fn = std::move(fn)](UIEvent&) {
        if (fn) fn(Value());
    });
    return this;
}

TextInput* TextInput::OnChanged(std::function<void(const std::string&)> fn) {
    On(UIEventType::TextInput, [this, fn = std::move(fn)](UIEvent&) {
        if (fn) fn(Value());
    });
    return this;
}

// --- Image ------------------------------------------------------------------

Image::Image(std::string path) : m_path(std::move(path)) {}

void Image::OnAttach() {
    SetName("Image");
    Ensure<UITransform>().Size = {64.0f, 64.0f};
    Ensure<UIImage>().Path = m_path;
}

Image* Image::SetPath(const std::string& path) {
    UIImage& im = Ensure<UIImage>();
    im.Path = path;
    im.Resolved = nullptr;  // путь сменился — прежняя текстура не та
    Dirty(UIDirty_Visual);
    return this;
}

Image* Image::SetTint(const UIColor& tint) {
    Ensure<UIImage>().Tint = tint;
    Dirty(UIDirty_Visual);
    return this;
}

// --- ScrollView -------------------------------------------------------------

void ScrollView::OnAttach() {
    SetName("ScrollView");
    Ensure<UITransform>().Size = {240.0f, 200.0f};
    Ensure<UIScrollView>();
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.ScrollTarget = true;
    // Маска на окне: без неё лента рисуется за границами окна поверх соседей —
    // и это выглядит как «прокрутка сломана».
    ClipChildren(true);

    m_content = Ctx().Create<Group>();
    Add(m_content);
    m_content->SetName("Content");
    // Лента растянута по ширине и растёт по содержимому вниз: иначе прокручивать
    // нечего — содержимое всегда ровно по окну.
    UITransform& t = m_content->Ensure<UITransform>();
    t.WidthMode = UISizeMode::Stretch;
    t.HeightMode = UISizeMode::Content;
    m_content->Vertical(4.0f);
    m_content->Layout().FitHeight = true;
}

ScrollView* ScrollView::SetHorizontal(bool on) {
    Ensure<UIScrollView>().Horizontal = on;
    return this;
}

ScrollView* ScrollView::SetVertical(bool on) {
    Ensure<UIScrollView>().Vertical = on;
    return this;
}

// --- Реестр -----------------------------------------------------------------

void RegisterBuiltinUIElements() {
    static bool done = false;
    if (done) return;
    done = true;

    UIElementRegistry& r = UIElementRegistry::Instance();
    r.Register<UIElement>("Element", "Элемент", "Основные", "rect");
    r.Register<Group>("Group", "Группа", "Контейнеры", "rect");
    r.Register<Panel>("Panel", "Панель", "Контейнеры", "rect");
    r.Register<ScrollView>("ScrollView", "Прокрутка", "Контейнеры", "rect");
    r.Register<Label>("Label", "Надпись", "Текст", "text");
    r.Register<Button>("Button", "Кнопка", "Управление", "rect");
    r.Register<Checkbox>("Checkbox", "Галка", "Управление", "check");
    r.Register<Slider>("Slider", "Ползунок", "Управление", "rect");
    r.Register<ProgressBar>("ProgressBar", "Полоса", "Значения", "rect");
    r.Register<TextInput>("TextInput", "Поле ввода", "Управление", "text");
    r.Register<Image>("Image", "Картинка", "Оформление", "texture");
}

} // namespace sage::ui::sui
