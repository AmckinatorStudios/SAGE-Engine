#include "sage/ui/sageui/UIViews.h"

#include <algorithm>

#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIIcon.h"

namespace sage::ui::sui {

namespace {

float Metric(UIElement& e, const char* token, float fallback) {
    return e.Ctx().Theme().Tokens.Number(token, fallback);
}

} // namespace

// ============================================================================
//  Tree
// ============================================================================

void Tree::OnAttach() {
    SetName("Tree");
    SetStretch(true, true);
    m_scroll = Ctx().CreateIn<ScrollView>(this);
    m_scroll->SetName("Rows");
    m_scroll->SetStretch(true, true);
    // Строки вплотную: это список, а не форма. Зазор между строками дерева
    // сцены съедает по строке на каждые десять.
    m_scroll->Content()->Layout().Gap = {0.0f, 0.0f};
}

UIElement* Tree::Content() const { return m_scroll ? m_scroll->Content() : nullptr; }

Tree::RowUI& Tree::EnsureRow(size_t index) {
    const float rowH = Metric(*this, "Size.Row", 22.0f);
    while (m_rows.size() <= index) {
        RowUI row;
        row.Box = Ctx().CreateIn<UIElement>(m_scroll->Content());
        row.Box->SetName("Row");
        row.Box->SetHeight(rowH);
        row.Box->SetStretch(true, false);
        row.Box->Horizontal(4.0f)->Padding(UIEdges(4.0f, 0.0f, 6.0f, 0.0f));
        row.Box->Layout().Cross = UIAlign::Center;
        row.Box->Ensure<UIFill>();
        row.Box->SetStyle("Row");
        UIInteraction& ia = row.Box->Ensure<UIInteraction>();
        ia.Hit = UIHitShape::Rect;

        // Треугольник раскрытия — своя кнопка: щелчок по нему НЕ выделяет
        // строку. Иначе развернуть ветку означало бы заодно потерять выделение.
        row.Arrow = Ctx().CreateIn<UIElement>(row.Box);
        row.Arrow->SetName("Arrow");
        row.Arrow->SetSize({12.0f, 12.0f});
        row.Arrow->Ensure<UIIcon>();
        row.Arrow->Ensure<UIInteraction>().Hit = UIHitShape::Rect;

        row.Icon = Ctx().CreateIn<UIElement>(row.Box);
        row.Icon->SetName("Icon");
        row.Icon->SetSize({14.0f, 14.0f});
        row.Icon->Ensure<UIIcon>();

        row.Text = Ctx().CreateIn<Label>(row.Box, std::string());
        row.Text->SetStretch(true, false);
        row.Text->SetEllipsis(true);

        row.Eye = Ctx().CreateIn<IconButton>(row.Box, std::string("eye"), std::string("Видимость"));
        row.Eye->SetSize({16.0f, 16.0f});

        const size_t at = m_rows.size();
        row.Box->OnClickEvent([this, at](UIEvent& e) {
            if (at >= m_rows.size() || !m_onSelect) return;
            if (e.Clicks >= 2 && m_onActivate) { m_onActivate(m_rows[at].Id); return; }
            m_onSelect(m_rows[at].Id, e.Ctrl || e.Shift);
        });
        row.Arrow->OnClick([this, at] {
            if (at < m_rows.size() && m_onToggle) m_onToggle(m_rows[at].Id);
        });
        row.Eye->OnPress([this, at] {
            if (at < m_rows.size() && m_onVisibility) m_onVisibility(m_rows[at].Id);
        });
        m_rows.push_back(row);
    }
    return m_rows[index];
}

void Tree::Apply(RowUI& row, const TreeItem& item) {
    row.Id = item.Id;
    row.Box->SetVisible(true);
    // Вложенность — отступом слева. Он растёт с уровнем, и по нему видно, что
    // во что вложено, без единой линии-направляющей.
    row.Box->Padding(UIEdges(4.0f + (float)item.Depth * 12.0f, 0.0f, 6.0f, 0.0f));

    UIIcon& arrow = row.Arrow->Ensure<UIIcon>();
    arrow.Name = item.HasChildren ? (item.Expanded ? "chevron-down" : "chevron-right") : "";
    row.Arrow->SetVisible(item.HasChildren);

    UIIcon& icon = row.Icon->Ensure<UIIcon>();
    icon.Name = item.Icon;
    icon.Color = item.Tint;
    row.Icon->SetVisible(!item.Icon.empty());

    row.Text->SetText(item.Text);
    // Скрытый объект показан приглушённо: «выключено» должно быть видно в самом
    // списке, а не только по значку глаза.
    row.Text->SetStyle(item.Visible ? "Label" : "Caption");

    // Выделение — состоянием узла, а не своим цветом: подсветка обязана
    // меняться вместе с темой.
    if (UIInteraction* ia = row.Box->Get<UIInteraction>()) {
        if (item.Selected) ia->Runtime.Flags |= UIState_Selected;
        else ia->Runtime.Flags &= ~(uint32_t)UIState_Selected;
    }
    row.Eye->SetIcon(item.Visible ? "eye" : "eye-off");
    row.Box->Dirty(UIDirty_Style);
}

void Tree::SetItems(std::vector<TreeItem> items) {
    m_items = std::move(items);
    for (size_t i = 0; i < m_items.size(); ++i) Apply(EnsureRow(i), m_items[i]);
    // Лишние строки ПРЯЧУТСЯ, а не удаляются: список сцены прыгает туда-сюда
    // при каждом сворачивании ветки.
    for (size_t i = m_items.size(); i < m_rows.size(); ++i) m_rows[i].Box->SetVisible(false);
}

Tree* Tree::OnSelect(std::function<void(const std::string&, bool)> fn) {
    m_onSelect = std::move(fn);
    return this;
}
Tree* Tree::OnToggle(std::function<void(const std::string&)> fn) {
    m_onToggle = std::move(fn);
    return this;
}
Tree* Tree::OnVisibility(std::function<void(const std::string&)> fn) {
    m_onVisibility = std::move(fn);
    return this;
}
Tree* Tree::OnActivate(std::function<void(const std::string&)> fn) {
    m_onActivate = std::move(fn);
    return this;
}

// ============================================================================
//  PropertyGrid
// ============================================================================

void PropertyGrid::OnAttach() {
    SetName("PropertyGrid");
    SetStretch(true, true);
    m_scroll = Ctx().CreateIn<ScrollView>(this);
    m_scroll->SetName("Sections");
    m_scroll->SetStretch(true, true);
    m_scroll->Content()->Layout().Gap = {0.0f, 2.0f};
}

UIElement* PropertyGrid::Content() const { return m_scroll ? m_scroll->Content() : nullptr; }

UIElement* PropertyGrid::AddSection(const std::string& title, bool expanded) {
    UIElement* box = Ctx().CreateIn<UIElement>(m_scroll->Content());
    box->SetName(title);
    box->SetStretch(true, false);
    box->Ensure<UITransform>().HeightMode = UISizeMode::Content;
    box->Vertical(0.0f)->Padding(UIEdges::Uniform(0.0f));
    box->Layout().FitHeight = true;
    box->Layout().Cross = UIAlign::Stretch;

    // Шапка секции: треугольник и название. Сворачивается — иначе инспектор
    // объекта с шестью компонентами не помещается ни в какой экран.
    UIElement* head = Ctx().CreateIn<UIElement>(box);
    head->SetName("Head");
    head->SetHeight(Metric(*this, "Size.Row", 22.0f));
    head->SetStretch(true, false);
    head->Horizontal(4.0f)->Padding(UIEdges(6.0f, 0.0f, 6.0f, 0.0f));
    head->Layout().Cross = UIAlign::Center;
    head->Ensure<UIFill>();
    head->SetStyle("SectionHead");
    head->Ensure<UIInteraction>().Hit = UIHitShape::Rect;

    UIElement* arrow = Ctx().CreateIn<UIElement>(head);
    arrow->SetName("Arrow");
    arrow->SetSize({12.0f, 12.0f});
    arrow->Ensure<UIIcon>().Name = expanded ? "chevron-down" : "chevron-right";

    Label* caption = Ctx().CreateIn<Label>(head, title);
    caption->SetStretch(true, false);

    UIElement* body = Ctx().CreateIn<UIElement>(box);
    body->SetName("Body");
    body->SetStretch(true, false);
    body->Ensure<UITransform>().HeightMode = UISizeMode::Content;
    body->Vertical(2.0f)->Padding(UIEdges(0.0f, 3.0f, 0.0f, 5.0f));
    body->Layout().FitHeight = true;
    body->Layout().Cross = UIAlign::Stretch;
    body->SetVisible(expanded);

    head->OnClick([arrow, body] {
        const bool open = !body->IsVisible();
        body->SetVisible(open);
        arrow->Ensure<UIIcon>().Name = open ? "chevron-down" : "chevron-right";
        arrow->Dirty(UIDirty_Visual);
    });
    return body;
}

UIElement* PropertyGrid::AddRow(UIElement* section, const std::string& label) {
    if (!section) return nullptr;
    UIElement* row = Ctx().CreateIn<UIElement>(section);
    row->SetName(label);
    row->SetHeight(Metric(*this, "Size.Row", 22.0f));
    row->SetStretch(true, false);
    row->Horizontal(6.0f)->Padding(UIEdges(8.0f, 0.0f, 8.0f, 0.0f));
    row->Layout().Cross = UIAlign::Center;

    Label* caption = Ctx().CreateIn<Label>(row, label);
    caption->SetName("Label");
    // Ширина колонки подписей одна на всю таблицу: пока каждая строка выбирала
    // её сама, поля стояли лесенкой.
    caption->SetWidth(m_labelWidth);
    caption->SetEllipsis(true);
    caption->SetStyle("Caption");

    UIElement* slot = Ctx().CreateIn<UIElement>(row);
    slot->SetName("Value");
    slot->SetStretch(true, false);
    slot->Horizontal(4.0f)->Padding(UIEdges::Uniform(0.0f));
    slot->Layout().Cross = UIAlign::Center;
    return slot;
}

UIElement* PropertyGrid::AddWide(UIElement* section) {
    if (!section) return nullptr;
    UIElement* row = Ctx().CreateIn<UIElement>(section);
    row->SetName("Wide");
    row->SetHeight(Metric(*this, "Size.Row", 22.0f) + 4.0f);
    row->SetStretch(true, false);
    row->Horizontal(4.0f)->Padding(UIEdges(8.0f, 0.0f, 8.0f, 0.0f));
    row->Layout().Cross = UIAlign::Center;
    return row;
}

PropertyGrid* PropertyGrid::SetLabelWidth(float width) {
    m_labelWidth = width;
    return this;
}

// ============================================================================
//  AssetView
// ============================================================================

void AssetView::OnAttach() {
    SetName("AssetView");
    SetStretch(true, true);
    m_scroll = Ctx().CreateIn<ScrollView>(this);
    m_scroll->SetName("Cards");
    m_scroll->SetStretch(true, true);
    // Карточки — сеткой с переносом: ширина панели меняется, и число карточек
    // в ряду обязано меняться вместе с ней.
    UILayout& l = m_scroll->Content()->Layout();
    l.Kind = UILayout::Mode::Wrap;
    l.Gap = {8.0f, 8.0f};
    l.Padding = UIEdges::Uniform(8.0f);
    l.FitHeight = true;
}

UIElement* AssetView::Content() const { return m_scroll ? m_scroll->Content() : nullptr; }

AssetView::CardUI& AssetView::EnsureCard(size_t index) {
    while (m_cards.size() <= index) {
        CardUI card;
        card.Box = Ctx().CreateIn<UIElement>(m_scroll->Content());
        card.Box->SetName("Card");
        card.Box->SetSize(m_cardSize);
        card.Box->Vertical(3.0f)->Padding(UIEdges::Uniform(6.0f));
        // Подписи не вылезают за карточку: длинное имя обрезается многоточием
        // внутри неё, а не рисуется поверх соседней.
        card.Box->ClipChildren(true, 3.0f);
        card.Box->Layout().Cross = UIAlign::Stretch;
        card.Box->Ensure<UIFill>();
        card.Box->SetStyle("Card");
        card.Box->Ensure<UIInteraction>().Hit = UIHitShape::RoundedRect;

        card.Cover = Ctx().CreateIn<UIElement>(card.Box);
        card.Cover->SetName("Cover");
        card.Cover->SetHeight(m_cardSize.x - 12.0f);
        card.Cover->SetStretch(true, false);
        card.Cover->Ensure<UIFill>();
        card.Cover->SetStyle("Cover");

        card.Icon = Ctx().CreateIn<UIElement>(card.Cover);
        UITransform& it = card.Icon->Ensure<UITransform>();
        it.AnchorMin = it.AnchorMax = {0.5f, 0.5f};
        it.Pivot = {0.5f, 0.5f};
        it.Size = {30.0f, 30.0f};
        card.Icon->Ensure<UIIcon>();

        card.Name = Ctx().CreateIn<Label>(card.Box, std::string());
        card.Name->SetEllipsis(true);
        card.Name->SetAlign(UITextAlign::Center, UITextVAlign::Top);
        card.Kind = Ctx().CreateIn<Label>(card.Box, std::string());
        card.Kind->SetStyle("Caption");
        card.Kind->SetEllipsis(true);
        card.Kind->SetAlign(UITextAlign::Center, UITextVAlign::Top);

        const size_t at = m_cards.size();
        card.Box->OnClickEvent([this, at](UIEvent& e) {
            if (at >= m_cards.size()) return;
            if (e.Clicks >= 2 && m_onActivate) { m_onActivate(m_cards[at].Id); return; }
            if (m_onSelect) m_onSelect(m_cards[at].Id);
        });
        m_cards.push_back(card);
    }
    return m_cards[index];
}

void AssetView::SetItems(std::vector<AssetItem> items) {
    m_items = std::move(items);
    for (size_t i = 0; i < m_items.size(); ++i) {
        const AssetItem& item = m_items[i];
        CardUI& card = EnsureCard(i);
        card.Id = item.Id;
        card.Box->SetVisible(true);
        card.Name->SetText(item.Name);
        card.Kind->SetText(item.Kind);
        card.Icon->Ensure<UIIcon>().Name = item.Icon;
        if (UIInteraction* ia = card.Box->Get<UIInteraction>()) {
            if (item.Selected) ia->Runtime.Flags |= UIState_Selected;
            else ia->Runtime.Flags &= ~(uint32_t)UIState_Selected;
        }
        card.Box->Dirty(UIDirty_Style);
    }
    for (size_t i = m_items.size(); i < m_cards.size(); ++i) m_cards[i].Box->SetVisible(false);
}

AssetView* AssetView::SetCardSize(glm::vec2 size) {
    // Высота ниже обложки плюс двух строк — это обрезанная подпись. Поднимаем
    // молча: карточка, у которой не видно имени, бесполезна.
    size.y = std::max(size.y, size.x - 12.0f + 40.0f);
    m_cardSize = size;
    for (CardUI& c : m_cards) {
        c.Box->SetSize(size);
        c.Cover->SetHeight(size.x - 12.0f);
    }
    return this;
}

AssetView* AssetView::OnSelect(std::function<void(const std::string&)> fn) {
    m_onSelect = std::move(fn);
    return this;
}

AssetView* AssetView::OnActivate(std::function<void(const std::string&)> fn) {
    m_onActivate = std::move(fn);
    return this;
}

} // namespace sage::ui::sui
