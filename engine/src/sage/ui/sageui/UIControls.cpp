#include "sage/ui/sageui/UIControls.h"

#include <algorithm>
#include <cstdio>

#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/sageui/UIRegistry.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIIcon.h"
#include "sage/ui/visual/UIShape.h"

namespace sage::ui::sui {

namespace {

// Число из темы. Через неё, а не константой в коде: плотность инструмента —
// решение темы, и «сделать редактор компактнее» должно быть правкой набора
// токенов, а не обходом полусотни файлов.
float Metric(UIElement& e, const char* token, float fallback) {
    return e.Ctx().Theme().Tokens.Number(token, fallback);
}

} // namespace

// ============================================================================
//  Separator
// ============================================================================

Separator::Separator(bool vertical) : m_vertical(vertical) {}

void Separator::OnAttach() {
    SetName("Separator");
    UITransform& t = Ensure<UITransform>();
    if (m_vertical) {
        t.Size.x = 1.0f;
        t.HeightMode = UISizeMode::Stretch;
        t.AnchorMin.y = 0.0f;
        t.AnchorMax.y = 1.0f;
    } else {
        t.Size.y = 1.0f;
        t.WidthMode = UISizeMode::Stretch;
        t.AnchorMin.x = 0.0f;
        t.AnchorMax.x = 1.0f;
    }
    Ensure<UIFill>().Radius = UICorners(0.0f);
    SetStyle("Separator");
}

// ============================================================================
//  IconButton
// ============================================================================

IconButton::IconButton(std::string icon, std::string tooltip)
    : m_icon(std::move(icon)), m_tooltip(std::move(tooltip)) {}

void IconButton::OnAttach() {
    SetName(m_icon.empty() ? "IconButton" : m_icon);
    const float side = Metric(*this, "Size.IconButton", 26.0f);
    SetSize({side, side});
    Ensure<UIFill>();
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    ia.Focusable = true;
    ia.TooltipKey = m_tooltip;
    SetStyle("IconButton");

    m_glyph = Ctx().CreateIn<UIElement>(this);
    m_glyph->SetName("Glyph");
    UITransform& gt = m_glyph->Ensure<UITransform>();
    gt.AnchorMin = gt.AnchorMax = {0.5f, 0.5f};
    gt.Pivot = {0.5f, 0.5f};
    gt.Size = {side - 10.0f, side - 10.0f};
    m_glyph->Ensure<UIIcon>().Name = m_icon;
}

IconButton::IconButton(Icon icon, std::string tooltip)
    : IconButton(std::string(icons::Icons::Name(icon)), std::move(tooltip)) {}

IconButton* IconButton::SetIcon(const std::string& icon) {
    m_icon = icon;
    if (m_glyph) m_glyph->Ensure<UIIcon>().Name = icon;
    Dirty(UIDirty_Visual);
    return this;
}

IconButton* IconButton::SetActive(bool active) {
    m_active = active;
    // Через СТОЯНИЕ узла, а не свой цвет: подсветка активного инструмента
    // обязана меняться вместе с темой, как и наведение с нажатием.
    if (UIInteraction* ia = Get<UIInteraction>()) {
        if (active) ia->Runtime.Flags |= UIState_Selected;
        else ia->Runtime.Flags &= ~(uint32_t)UIState_Selected;
    }
    Dirty(UIDirty_Style);
    return this;
}

IconButton* IconButton::OnPress(std::function<void()> fn) {
    OnClick(std::move(fn));
    return this;
}

// ============================================================================
//  Toolbar
// ============================================================================

void Toolbar::OnAttach() {
    SetName("Toolbar");
    SetHeight(Metric(*this, "Size.Toolbar", 34.0f));
    SetStretch(true, false);
    Ensure<UIFill>();
    SetStyle("Toolbar");
    Horizontal(Metric(*this, "Spacing.Small", 4.0f))
        ->Padding(UIEdges(6.0f, 4.0f, 6.0f, 4.0f));
    Layout().Cross = UIAlign::Center;
}

IconButton* Toolbar::AddIcon(const std::string& icon, const std::string& tooltip,
                             std::function<void()> onPress) {
    IconButton* b = Ctx().CreateIn<IconButton>(this, icon, tooltip);
    if (onPress) b->OnPress(std::move(onPress));
    return b;
}

Button* Toolbar::AddButton(const std::string& text, std::function<void()> onPress) {
    Button* b = Ctx().CreateIn<Button>(this, text, std::string());
    b->SetHeight(Metric(*this, "Size.Control", 22.0f));
    b->FitToText(10.0f);
    if (onPress) b->OnClick(std::move(onPress));
    return b;
}

void Toolbar::AddSeparator() {
    Separator* s = Ctx().CreateIn<Separator>(this, true);
    s->SetWidth(1.0f);
    // Воздух вокруг разделителя: вплотную он читается как край кнопки.
    s->Ensure<UITransform>().Margin = UIEdges(4.0f, 6.0f, 4.0f, 6.0f);
}

UIElement* Toolbar::AddSpacer() {
    UIElement* spacer = Ctx().CreateIn<UIElement>(this);
    spacer->SetName("Spacer");
    spacer->SetStretch(true, false);
    return spacer;
}

// ============================================================================
//  MenuBar
// ============================================================================

void MenuBar::OnAttach() {
    SetName("MenuBar");
    SetHeight(Metric(*this, "Size.MenuBar", 28.0f));
    SetStretch(true, false);
    Ensure<UIFill>();
    SetStyle("MenuBar");
    Horizontal(0.0f)->Padding(UIEdges(8.0f, 0.0f, 8.0f, 0.0f));
    Layout().Cross = UIAlign::Stretch;

    m_left = Ctx().CreateIn<UIElement>(this);
    m_left->SetName("Left");
    m_left->Horizontal(2.0f)->Padding(UIEdges::Uniform(0.0f));
    m_left->Layout().Cross = UIAlign::Center;
    m_left->Layout().FitWidth = true;
    m_left->Ensure<UITransform>().WidthMode = UISizeMode::Content;

    UIElement* spacer = Ctx().CreateIn<UIElement>(this);
    spacer->SetName("Spacer");
    spacer->SetStretch(true, false);

    m_right = Ctx().CreateIn<UIElement>(this);
    m_right->SetName("Right");
    m_right->Horizontal(6.0f)->Padding(UIEdges::Uniform(0.0f));
    m_right->Layout().Cross = UIAlign::Center;
    m_right->Layout().FitWidth = true;
    m_right->Ensure<UITransform>().WidthMode = UISizeMode::Content;
}

Label* MenuBar::SetBrand(const std::string& text) {
    if (!m_brand) {
        m_brand = Ctx().Create<Label>(text);
        m_left->Add(m_brand, 0);
        m_brand->SetName("Brand");
        m_brand->SetStyle("Brand");
        m_brand->Ensure<UITransform>().Margin = UIEdges(0.0f, 0.0f, 10.0f, 0.0f);
    }
    m_brand->SetText(text);
    return m_brand;
}

Popup* MenuBar::AddMenu(const std::string& title) {
    Button* head = Ctx().CreateIn<Button>(m_left, title, std::string());
    head->SetName("Menu");
    head->SetHeight(Metric(*this, "Size.MenuBar", 28.0f) - 6.0f);
    head->FitToText(9.0f);
    head->SetStyle("MenuTitle");

    Popup* menu = Ctx().Create<Popup>();
    menu->SetName(title + " menu");
    // Меню открывается ПОД заголовком, а не под курсором: так оно всегда в
    // одном месте относительно своего пункта, и по нему можно вести мышь.
    head->OnClick([menu, head] {
        if (menu->IsOpen()) menu->Close();
        else menu->OpenUnder(head);
    });
    return menu;
}

UIElement* MenuBar::Right() { return m_right; }

// ============================================================================
//  SearchBox
// ============================================================================

SearchBox::SearchBox(std::string placeholder) : m_placeholder(std::move(placeholder)) {}

void SearchBox::OnAttach() {
    SetName("SearchBox");
    SetHeight(Metric(*this, "Size.Control", 22.0f));
    Ensure<UIFill>();
    SetStyle("InputField");
    Horizontal(4.0f)->Padding(UIEdges(6.0f, 0.0f, 4.0f, 0.0f));
    Layout().Cross = UIAlign::Center;

    UIElement* glyph = Ctx().CreateIn<UIElement>(this);
    glyph->SetName("Glyph");
    glyph->SetSize({12.0f, 12.0f});
    glyph->Ensure<UIIcon>().Name = icons::Icons::Name(Icon::Search);
    glyph->Ensure<UIIcon>().Color = UIColor(0.55f, 0.57f, 0.60f, 1.0f);

    m_input = Ctx().CreateIn<TextInput>(this, std::string(), m_placeholder);
    m_input->SetStretch(true, false);
    m_input->SetHeight(Metric(*this, "Size.Control", 22.0f) - 2.0f);
    // Своей подложки у поля внутри нет: она уже есть у самого SearchBox, и
    // вторая рамка внутри первой читается как поле в поле.
    m_input->Ensure<UIFill>().Color = UIColor(0.0f, 0.0f, 0.0f, 0.0f);
    m_input->Ensure<UIStyled>().Style.clear();
    m_input->OnChanged([this](const std::string& text) {
        if (m_onChanged) m_onChanged(text);
    });

    m_clear = Ctx().CreateIn<IconButton>(this, Icon::Close, std::string("Очистить"));
    m_clear->SetSize({16.0f, 16.0f});
    static_cast<IconButton*>(m_clear)->OnPress([this] {
        m_input->SetValue(std::string());
        if (m_onChanged) m_onChanged(std::string());
    });
    m_clear->SetVisible(false);
}

void SearchBox::Update(float) {
    // Крестик виден, только когда есть что стирать: пустой крестик — обещание
    // действия, которого нет.
    if (m_clear) m_clear->SetVisible(!Value().empty());
}

const std::string& SearchBox::Value() const {
    static const std::string kEmpty;
    return m_input ? m_input->Value() : kEmpty;
}

SearchBox* SearchBox::SetValue(const std::string& text) {
    if (m_input) m_input->SetValue(text);
    return this;
}

SearchBox* SearchBox::OnChanged(std::function<void(const std::string&)> fn) {
    m_onChanged = std::move(fn);
    return this;
}

// ============================================================================
//  Dropdown
// ============================================================================

Dropdown::Dropdown(std::vector<std::string> items, int index)
    : m_items(std::move(items)), m_index(index) {}

void Dropdown::OnAttach() {
    SetName("Dropdown");
    SetHeight(Metric(*this, "Size.Control", 22.0f));
    Ensure<UIFill>();
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    ia.Focusable = true;
    SetStyle("InputField");
    Horizontal(4.0f)->Padding(UIEdges(8.0f, 0.0f, 6.0f, 0.0f));
    Layout().Cross = UIAlign::Center;

    m_text = Ctx().CreateIn<Label>(this, std::string());
    m_text->SetStretch(true, false);
    m_text->SetEllipsis(true);

    UIElement* arrow = Ctx().CreateIn<UIElement>(this);
    arrow->SetName("Arrow");
    arrow->SetSize({10.0f, 10.0f});
    arrow->Ensure<UIIcon>().Name = icons::Icons::Name(Icon::ChevronDown);

    m_popup = Ctx().Create<Popup>();
    m_popup->SetName("Dropdown menu");
    OnClick([this] {
        if (m_popup->IsOpen()) { m_popup->Close(); return; }
        // Ширина списка — по ширине самого поля: список уже поля выглядит
        // обрезанным, шире — наезжает на соседей.
        m_popup->SetWidth(std::max(80.0f, Size().x));
        m_popup->OpenUnder(this);
    });

    Refresh();
}

void Dropdown::Refresh() {
    if (!m_text) return;
    m_index = m_items.empty() ? 0 : std::min(std::max(m_index, 0), (int)m_items.size() - 1);
    m_text->SetText(m_items.empty() ? std::string() : m_items[(size_t)m_index]);

    if (!m_popup) return;
    for (UIElement* child : m_popup->Children()) Ctx().Destroy(child);
    for (size_t i = 0; i < m_items.size(); ++i) {
        const int index = (int)i;
        m_popup->AddItem(m_items[i], [this, index] { SetIndex(index); });
    }
}

const std::string& Dropdown::Selected() const {
    static const std::string kEmpty;
    if (m_items.empty()) return kEmpty;
    return m_items[(size_t)std::min(std::max(m_index, 0), (int)m_items.size() - 1)];
}

Dropdown* Dropdown::SetItems(std::vector<std::string> items, int index) {
    m_items = std::move(items);
    m_index = index;
    Refresh();
    return this;
}

Dropdown* Dropdown::SetIndex(int index) {
    if (index == m_index) return this;
    m_index = index;
    if (m_text && !m_items.empty()) {
        m_index = std::min(std::max(m_index, 0), (int)m_items.size() - 1);
        m_text->SetText(m_items[(size_t)m_index]);
    }
    if (m_onChanged) m_onChanged(m_index);
    return this;
}

Dropdown* Dropdown::OnChanged(std::function<void(int)> fn) {
    m_onChanged = std::move(fn);
    return this;
}

// ============================================================================
//  NumericField
// ============================================================================

NumericField::NumericField(float value) : m_value(value) {}

void NumericField::OnAttach() {
    SetName("NumericField");
    SetHeight(Metric(*this, "Size.Control", 22.0f));
    Ensure<UIFill>();
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    ia.Focusable = true;
    ia.Draggable = true;
    ia.Cursor = "resize-ew";
    SetStyle("InputField");
    Horizontal(4.0f)->Padding(UIEdges(6.0f, 0.0f, 6.0f, 0.0f));
    Layout().Cross = UIAlign::Center;

    m_prefix = Ctx().CreateIn<Label>(this, std::string());
    m_prefix->SetName("Prefix");
    m_prefix->SetStyle("Caption");
    m_prefix->SetVisible(false);

    m_text = Ctx().CreateIn<Label>(this, std::string());
    m_text->SetStretch(true, false);
    m_text->SetAlign(UITextAlign::Right, UITextVAlign::Center);

    On(UIEventType::DragStart, [this](UIEvent&) { m_dragStart = m_value; });
    On(UIEventType::Drag, [this](UIEvent& e) {
        // Перетаскивание по горизонтали: правка позиции объекта делается сотни
        // раз за сеанс, и набирать цифры на каждую — это не работа.
        float next = m_dragStart + e.Delta.x * m_step;
        if (m_min != m_max) next = std::min(std::max(next, m_min), m_max);
        SetValue(next);
    });
    Show();
}

void NumericField::Show() {
    if (!m_text) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", m_value);
    m_text->SetText(buf);
}

NumericField* NumericField::SetValue(float value) {
    if (m_min != m_max) value = std::min(std::max(value, m_min), m_max);
    if (value == m_value) return this;
    m_value = value;
    Show();
    if (m_onChanged) m_onChanged(m_value);
    return this;
}

NumericField* NumericField::SetRange(float min, float max) {
    m_min = min;
    m_max = max;
    return this;
}

NumericField* NumericField::SetStep(float step) {
    m_step = step;
    return this;
}

NumericField* NumericField::SetLabel(const std::string& text) {
    m_label = text;
    if (m_prefix) {
        m_prefix->SetText(text);
        m_prefix->SetVisible(!text.empty());
    }
    return this;
}

NumericField* NumericField::OnChanged(std::function<void(float)> fn) {
    m_onChanged = std::move(fn);
    return this;
}

// ============================================================================
//  StatusBar
// ============================================================================

void StatusBar::OnAttach() {
    SetName("StatusBar");
    SetHeight(Metric(*this, "Size.StatusBar", 24.0f));
    SetStretch(true, false);
    Ensure<UIFill>();
    SetStyle("StatusBar");
    Horizontal(6.0f)->Padding(UIEdges(10.0f, 0.0f, 10.0f, 0.0f));
    Layout().Cross = UIAlign::Center;

    m_dot = Ctx().CreateIn<UIElement>(this);
    m_dot->SetName("Dot");
    m_dot->SetSize({7.0f, 7.0f});
    UIShape& shape = m_dot->Ensure<UIShape>();
    shape.Type = UIShape::Kind::Circle;
    shape.Color = UIColorFromHex("#4FB865");

    m_status = Ctx().CreateIn<Label>(this, std::string());
    m_status->SetName("Status");

    UIElement* spacer = Ctx().CreateIn<UIElement>(this);
    spacer->SetName("Spacer");
    spacer->SetStretch(true, false);

    m_right = Ctx().CreateIn<UIElement>(this);
    m_right->SetName("Fields");
    m_right->Horizontal(14.0f)->Padding(UIEdges::Uniform(0.0f));
    m_right->Layout().Cross = UIAlign::Center;
    m_right->Layout().FitWidth = true;
    m_right->Ensure<UITransform>().WidthMode = UISizeMode::Content;
}

StatusBar* StatusBar::SetStatus(const std::string& text, const UIColor& dot) {
    if (m_status) m_status->SetText(text);
    if (m_dot) m_dot->Ensure<UIShape>().Color = dot;
    Dirty(UIDirty_Visual);
    return this;
}

void StatusBar::SetField(const std::string& name, const std::string& value) {
    for (Field& f : m_fields) {
        if (f.Name != name) continue;
        f.Value->SetText(value);
        return;
    }
    // Поле заводится ОДИН раз. Пересоздавать пары «имя — значение» каждый кадр
    // значило бы пересчитывать раскладку всей строки ради двух цифр.
    UIElement* box = Ctx().CreateIn<UIElement>(m_right);
    box->SetName(name);
    box->Horizontal(5.0f)->Padding(UIEdges::Uniform(0.0f));
    box->Layout().Cross = UIAlign::Center;
    box->Layout().FitWidth = true;
    box->Ensure<UITransform>().WidthMode = UISizeMode::Content;

    Field f;
    f.Name = name;
    f.Caption = Ctx().CreateIn<Label>(box, name);
    f.Caption->SetStyle("Caption");
    f.Value = Ctx().CreateIn<Label>(box, value);
    m_fields.push_back(f);
}

} // namespace sage::ui::sui
