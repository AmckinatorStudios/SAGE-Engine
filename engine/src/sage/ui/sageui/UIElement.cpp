#include "sage/ui/sageui/UIElement.h"

#include "sage/ui/mask/UIMask.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/style/UIStyle.h"

namespace sage::ui::sui {

UIElement::~UIElement() {
    // Подписки снимаются здесь и только здесь. Обработчик, переживший свой
    // элемент, — это вызов лямбды с висячим this через кадр после удаления, и
    // ловится он отладчиком, а не глазами.
    if (m_ctx) {
        for (int s : m_subs) m_ctx->Events().Off(s);
    }
    m_subs.clear();
}

UIDocument& UIElement::Doc() const { return m_ctx->Doc(); }

UINode* UIElement::Node() const {
    return m_ctx ? m_ctx->Doc().Find(m_id) : nullptr;
}

UITransform& UIElement::Transform() { return Node()->Ensure<UITransform>(); }

void UIElement::Dirty(uint32_t flags) { m_ctx->Doc().MarkDirty(m_id, flags); }

// --- Дерево -----------------------------------------------------------------

UIElement* UIElement::Parent() const {
    const UINode* n = Node();
    return n ? m_ctx->Find(n->Parent) : nullptr;
}

std::vector<UIElement*> UIElement::Children() const {
    std::vector<UIElement*> out;
    const UINode* n = Node();
    if (!n) return out;
    out.reserve(n->Children.size());
    for (UINodeId id : n->Children) {
        // Узел без объектного лица пропускается, а не отдаётся как nullptr:
        // дерево документа шире дерева элементов (часть узлов приходит из
        // файла), и заставлять вызывающего проверять каждый — плохой обмен.
        if (UIElement* e = m_ctx->Find(id)) out.push_back(e);
    }
    return out;
}

UIElement* UIElement::Add(UIElement* child, int index) {
    if (child && child != this) m_ctx->Doc().Reparent(child->m_id, m_id, index);
    return child;
}

void UIElement::Remove(UIElement* child) {
    if (child && child->Parent() == this) m_ctx->Destroy(child);
}

void UIElement::RemoveFromParent() { m_ctx->Destroy(this); }

// --- Геометрия ---------------------------------------------------------------

UIElement* UIElement::SetPosition(glm::vec2 position) {
    Transform().Offset = position;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetSize(glm::vec2 size) {
    UITransform& t = Transform();
    t.Size = size;
    // Явно заданный размер означает Fixed по обеим осям: иначе «поставил
    // размер, а он не применился» — потому что ось осталась растянутой.
    t.WidthMode = UISizeMode::Fixed;
    t.HeightMode = UISizeMode::Fixed;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetWidth(float w) {
    UITransform& t = Transform();
    t.Size.x = w;
    t.WidthMode = UISizeMode::Fixed;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetHeight(float h) {
    UITransform& t = Transform();
    t.Size.y = h;
    t.HeightMode = UISizeMode::Fixed;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetAnchor(glm::vec2 min, glm::vec2 max) {
    UITransform& t = Transform();
    t.AnchorMin = min;
    t.AnchorMax = max;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetAnchor(UIAnchor preset) {
    Transform().SetAnchorPoint(preset);
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetPivot(glm::vec2 pivot) {
    Transform().Pivot = pivot;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetMargin(const UIEdges& margin) {
    Transform().Margin = margin;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::SetStretch(bool horizontal, bool vertical) {
    Transform().SetStretch(horizontal, vertical);
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::FitContent(bool width, bool height) {
    UITransform& t = Transform();
    if (width) t.WidthMode = UISizeMode::Content;
    if (height) t.HeightMode = UISizeMode::Content;
    // Контейнеру мало режима оси — ему нужен и флаг «умею считать содержимое».
    if (UILayout* l = Get<UILayout>()) {
        if (width) l->FitWidth = true;
        if (height) l->FitHeight = true;
    }
    Dirty(UIDirty_Layout);
    return this;
}

glm::vec2 UIElement::Position() const {
    const UITransform* t = Get<UITransform>();
    return t ? t->Offset : glm::vec2(0.0f);
}

glm::vec2 UIElement::Size() const {
    const UITransform* t = Get<UITransform>();
    return t ? t->Size : glm::vec2(0.0f);
}

UIRect UIElement::Bounds() const {
    UIRect r{};
    m_ctx->Runtime().Layout().RectOf(m_id, r);
    return r;
}

// --- Раскладка детей ---------------------------------------------------------

UILayout& UIElement::Layout() { return Node()->Ensure<UILayout>(); }

UIElement* UIElement::Horizontal(float gap) {
    UILayout& l = Layout();
    l.Kind = UILayout::Mode::Horizontal;
    l.Gap = {gap, gap};
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::Vertical(float gap) {
    UILayout& l = Layout();
    l.Kind = UILayout::Mode::Vertical;
    l.Gap = {gap, gap};
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::Grid(int columns, glm::vec2 gap) {
    UILayout& l = Layout();
    l.Kind = UILayout::Mode::Grid;
    l.Columns = columns;
    l.Gap = gap;
    // Ячейка занимает ячейку сетки: иначе дети сохраняют свой прежний размер и
    // лишь центрируются в клетках — то есть налезают друг на друга.
    l.Cross = UIAlign::Stretch;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::Stack() {
    Layout().Kind = UILayout::Mode::Overlay;
    Dirty(UIDirty_Layout);
    return this;
}

UIElement* UIElement::Padding(const UIEdges& padding) {
    Layout().Padding = padding;
    Dirty(UIDirty_Layout);
    return this;
}

// --- Состояние ---------------------------------------------------------------

UIElement* UIElement::SetVisible(bool visible) {
    if (UINode* n = Node()) {
        n->Visible = visible;
        Dirty(UIDirty_Visual);
    }
    return this;
}

bool UIElement::IsVisible() const {
    const UINode* n = Node();
    return n && n->Visible;
}

UIElement* UIElement::SetEnabled(bool enabled) {
    if (UINode* n = Node()) {
        n->Enabled = enabled;
        // Выключенный узел выпадает из раскладки, ввода и отрисовки целиком —
        // это не «серый цвет», а отсутствие. Потому Dirty_All.
        Dirty(UIDirty_All);
    }
    return this;
}

bool UIElement::IsEnabled() const {
    const UINode* n = Node();
    return n && n->Enabled;
}

UIElement* UIElement::SetOpacity(float opacity) {
    if (UINode* n = Node()) {
        n->Opacity = opacity;
        Dirty(UIDirty_Visual);
    }
    return this;
}

UIElement* UIElement::SetName(std::string name) {
    if (UINode* n = Node()) n->Name = std::move(name);
    return this;
}

const std::string& UIElement::Name() const {
    static const std::string kEmpty;
    const UINode* n = Node();
    return n ? n->Name : kEmpty;
}

UIElement* UIElement::SetStyle(const std::string& style) {
    Node()->Ensure<UIStyled>().Style = style;
    Dirty(UIDirty_Style);
    return this;
}

UIElement* UIElement::ClipChildren(bool clip, float radius) {
    if (!clip) {
        if (UINode* n = Node()) n->Remove<UIMask>();
        Dirty(UIDirty_All);
        return this;
    }
    UIMask& m = Node()->Ensure<UIMask>();
    m.Form = radius > 0.0f ? UIMask::Shape::RoundedRect : UIMask::Shape::Rect;
    m.Radius = UICorners(radius);
    Dirty(UIDirty_All);
    return this;
}

// --- События -----------------------------------------------------------------

int UIElement::On(UIEventType type, UIEventHandler handler) {
    const int id = m_ctx->Events().On(m_id, type, std::move(handler));
    m_subs.push_back(id);
    return id;
}

void UIElement::Off(int subscription) {
    m_ctx->Events().Off(subscription);
    for (size_t i = 0; i < m_subs.size(); ++i) {
        if (m_subs[i] == subscription) {
            m_subs.erase(m_subs.begin() + (long)i);
            return;
        }
    }
}

UIElement* UIElement::OnClick(std::function<void()> fn) {
    On(UIEventType::Click, [fn = std::move(fn)](UIEvent&) { if (fn) fn(); });
    return this;
}

UIElement* UIElement::OnClickEvent(UIEventHandler fn) {
    On(UIEventType::Click, std::move(fn));
    return this;
}

UIElement* UIElement::OnHover(std::function<void(bool)> fn) {
    On(UIEventType::PointerEnter, [fn](UIEvent&) { if (fn) fn(true); });
    On(UIEventType::PointerExit, [fn](UIEvent&) { if (fn) fn(false); });
    return this;
}

UIElement* UIElement::OnValueChanged(std::function<void(float)> fn) {
    On(UIEventType::ValueChanged, [fn = std::move(fn)](UIEvent& e) { if (fn) fn(e.Value); });
    return this;
}

} // namespace sage::ui::sui
