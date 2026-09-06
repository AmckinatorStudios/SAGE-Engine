#include "sage/ui/sageui/UIDock.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/sageui/UIRegistry.h"
#include "sage/ui/visual/UIFill.h"

using json = nlohmann::json;

namespace sage::ui::sui {

// ============================================================================
//  DockPanel
// ============================================================================

DockPanel::DockPanel(std::string id, std::string title)
    : m_id(std::move(id)), m_title(std::move(title)) {}

void DockPanel::OnAttach() {
    SetName(m_title.empty() ? m_id : m_title);
    SetStretch(true, true);
    // Своя подложка: без неё видно фон области дока, и вкладка не читается как
    // «принадлежащая» тому, что под ней.
    Ensure<UIFill>();
    SetStyle("DockPanel");
    Vertical(0.0f)->Padding(UIEdges::Uniform(0.0f));
    // Панель обрезает своё содержимое: длинный список не должен рисоваться
    // поверх соседней области дока.
    ClipChildren(true);

    m_body = Ctx().CreateIn<UIElement>(this);
    m_body->SetName("Body");
    m_body->SetStretch(true, true);
    m_body->Vertical(4.0f)->Padding(UIEdges::Uniform(6.0f));
}

DockPanel* DockPanel::SetTitle(std::string title) {
    m_title = std::move(title);
    SetName(m_title);
    return this;
}

// ============================================================================
//  DockSpace
// ============================================================================

void DockSpace::OnAttach() {
    SetName("DockSpace");
    SetStretch(true, true);

    m_host = Ctx().CreateIn<UIElement>(this);
    m_host->SetName("DockHost");
    m_host->SetStretch(true, true);

    // Стоянка: сюда уезжают закрытые и переезжающие панели. Выключена целиком,
    // поэтому не считается, не рисуется и не ловит мышь (§«выключенный узел не
    // участвует ни в чём»). Без неё закрытую панель пришлось бы удалять, а
    // вместе с ней — её содержимое и состояние.
    m_parking = Ctx().CreateIn<UIElement>(this);
    m_parking->SetName("Parking");
    m_parking->SetEnabled(false);

    m_root = std::make_unique<DockNode>();
}

void DockSpace::Update(float) {
    if (!m_dirty) return;
    m_dirty = false;
    Rebuild();
}

// --- Регистрация панелей -----------------------------------------------------

DockPanel* DockSpace::Add(DockPanel* panel, DockSide side, const std::string& nearId) {
    if (!panel) return nullptr;
    for (DockPanel* p : m_panels)
        if (p->Id() == panel->Id()) return p;   // повторная регистрация — не вторая панель
    m_panels.push_back(panel);
    m_parking->Add(panel);
    Insert(panel->Id(), side, nearId);
    m_dirty = true;
    return panel;
}

DockPanel* DockSpace::Find(const std::string& id) const {
    for (DockPanel* p : m_panels)
        if (p->Id() == id) return p;
    return nullptr;
}

std::vector<std::string> DockSpace::PanelIds() const {
    std::vector<std::string> out;
    out.reserve(m_panels.size());
    for (DockPanel* p : m_panels) out.push_back(p->Id());
    return out;
}

bool DockSpace::IsOpen(const std::string& id) const {
    return FindTabsOf(id) != nullptr || IsFloating(id);
}

void DockSpace::Focus(const std::string& id) {
    if (IsFloating(id)) {
        for (const Floating& f : m_floating)
            if (f.Id == id && f.Win) f.Win->Open();
        return;
    }
    DockNode* tabs = FindTabsOf(id);
    if (!tabs) {
        // Закрытая панель возвращается в раскладку. «Окно > Консоль» обязано её
        // ВЕРНУТЬ, а не включить невидимо где-то в закрытой области.
        Insert(id, DockSide::Center, {});
        tabs = FindTabsOf(id);
    }
    if (!tabs) return;
    for (size_t i = 0; i < tabs->Panels.size(); ++i) {
        if (tabs->Panels[i] == id) {
            tabs->Active = (int)i;
            break;
        }
    }
    m_dirty = true;
}

void DockSpace::Close(const std::string& id) {
    if (IsFloating(id)) {
        for (size_t i = 0; i < m_floating.size(); ++i) {
            if (m_floating[i].Id != id) continue;
            if (DockPanel* p = Find(id)) m_parking->Add(p);
            if (m_floating[i].Win) Ctx().Destroy(m_floating[i].Win);
            m_floating.erase(m_floating.begin() + (long)i);
            break;
        }
        return;
    }
    RemoveFromLayout(id);
    if (DockPanel* p = Find(id)) m_parking->Add(p);
    m_dirty = true;
}

// --- Плавающие окна ----------------------------------------------------------

bool DockSpace::IsFloating(const std::string& id) const {
    for (const Floating& f : m_floating)
        if (f.Id == id) return true;
    return false;
}

void DockSpace::Float(const std::string& id, glm::vec2 at) {
    DockPanel* panel = Find(id);
    if (!panel || IsFloating(id)) return;

    RemoveFromLayout(id);
    Window* win = Ctx().CreateIn<Window>(Ctx().Layer(UILayer::Overlay), panel->Title());
    win->SetPosition(at)->SetSize({360.0f, 280.0f});
    // Панель ПЕРЕЕЗЖАЕТ в окно, а не создаётся заново: её содержимое,
    // прокрутка и выделение остаются на месте.
    win->Body()->Add(panel);
    win->OnClose([this, id] { Dock(id); });

    m_floating.push_back({id, win});
    m_dirty = true;
}

void DockSpace::Dock(const std::string& id, DockSide side, const std::string& nearId) {
    for (size_t i = 0; i < m_floating.size(); ++i) {
        if (m_floating[i].Id != id) continue;
        DockPanel* panel = Find(id);
        if (panel) m_parking->Add(panel);
        if (m_floating[i].Win) Ctx().Destroy(m_floating[i].Win);
        m_floating.erase(m_floating.begin() + (long)i);
        break;
    }
    Insert(id, side, nearId);
    m_dirty = true;
}

// --- Дерево раскладки --------------------------------------------------------

DockNode* DockSpace::FindTabsOf(const std::string& id, DockNode* from) const {
    DockNode* node = from ? from : m_root.get();
    if (!node) return nullptr;
    if (!node->IsSplit()) {
        for (const std::string& p : node->Panels)
            if (p == id) return node;
        return nullptr;
    }
    for (const auto& c : node->Children)
        if (DockNode* found = FindTabsOf(id, c.get())) return found;
    return nullptr;
}

DockNode* DockSpace::FindParentOf(const DockNode* child, DockNode* from) const {
    DockNode* node = from ? from : m_root.get();
    if (!node || !node->IsSplit()) return nullptr;
    for (const auto& c : node->Children) {
        if (c.get() == child) return node;
        if (DockNode* found = FindParentOf(child, c.get())) return found;
    }
    return nullptr;
}

void DockSpace::RemoveFromLayout(const std::string& id) {
    DockNode* tabs = FindTabsOf(id);
    if (!tabs) return;
    for (size_t i = 0; i < tabs->Panels.size(); ++i) {
        if (tabs->Panels[i] != id) continue;
        tabs->Panels.erase(tabs->Panels.begin() + (long)i);
        tabs->Active = std::min(tabs->Active, (int)tabs->Panels.size() - 1);
        if (tabs->Active < 0) tabs->Active = 0;
        break;
    }
    Prune(m_root.get());
}

// Убрать опустевшие области: разделитель с одним живым ребёнком схлопывается в
// этого ребёнка. Иначе после закрытия панели в раскладке остаётся пустая
// половина экрана, и вернуть её человеку нечем.
void DockSpace::Prune(DockNode* node) {
    if (!node || !node->IsSplit()) return;
    for (const auto& c : node->Children) Prune(c.get());

    const bool a = node->Children.size() > 0 &&
                   (node->Children[0]->IsSplit() || !node->Children[0]->Empty());
    const bool b = node->Children.size() > 1 &&
                   (node->Children[1]->IsSplit() || !node->Children[1]->Empty());
    if (a && b) return;
    if (!a && !b) {
        node->Type = DockNode::Kind::Tabs;
        node->Children.clear();
        node->Panels.clear();
        return;
    }
    // Схлопываем в единственного живого ребёнка, забирая его содержимое.
    std::unique_ptr<DockNode> keep = std::move(node->Children[a ? 0 : 1]);
    node->Type = keep->Type;
    node->Vertical = keep->Vertical;
    node->Ratio = keep->Ratio;
    node->Panels = std::move(keep->Panels);
    node->Active = keep->Active;
    node->Children = std::move(keep->Children);
}

void DockSpace::Insert(const std::string& id, DockSide side, const std::string& nearId) {
    if (!m_root) m_root = std::make_unique<DockNode>();

    DockNode* target = nearId.empty() ? nullptr : FindTabsOf(nearId);
    if (!target) {
        // Без цели — в первую попавшуюся область вкладок. Для пустого дока это
        // корень, и он же станет местом для всего остального.
        DockNode* node = m_root.get();
        while (node->IsSplit() && !node->Children.empty()) node = node->Children[0].get();
        target = node;
    }

    if (side == DockSide::Center) {
        target->Panels.push_back(id);
        target->Active = (int)target->Panels.size() - 1;
        return;
    }

    // Деление: цель превращается в разделитель, её содержимое уезжает в одного
    // ребёнка, новая панель — в другого.
    auto moved = std::make_unique<DockNode>();
    moved->Type = target->Type;
    moved->Vertical = target->Vertical;
    moved->Ratio = target->Ratio;
    moved->Panels = std::move(target->Panels);
    moved->Active = target->Active;
    moved->Children = std::move(target->Children);

    auto fresh = std::make_unique<DockNode>();
    fresh->Panels.push_back(id);

    const bool first = side == DockSide::Left || side == DockSide::Top;
    target->Type = DockNode::Kind::Split;
    target->Vertical = side == DockSide::Top || side == DockSide::Bottom;
    target->Ratio = first ? 0.3f : 0.7f;
    target->Panels.clear();
    target->Active = 0;
    target->Children.clear();
    target->Children.push_back(first ? std::move(fresh) : std::move(moved));
    target->Children.push_back(first ? std::move(moved) : std::move(fresh));
}

void DockSpace::ResetLayout() {
    m_root = std::make_unique<DockNode>();
    for (DockPanel* p : m_panels) {
        if (IsFloating(p->Id())) continue;
        m_root->Panels.push_back(p->Id());
    }
    m_root->Active = 0;
    m_dirty = true;
}

// --- Построение элементов ----------------------------------------------------

void DockSpace::Rebuild() {
    // Панели сначала уезжают на стоянку — целиком и все. Так узел панели точно
    // не окажется ребёнком удаляемой области: удалить область вместе с чужой
    // панелью значит потерять её содержимое.
    for (DockPanel* p : m_panels) {
        if (IsFloating(p->Id())) continue;
        m_parking->Add(p);
    }
    for (UIElement* child : m_host->Children()) Ctx().Destroy(child);
    if (m_root) BuildNode(*m_root, m_host);
}

UIElement* DockSpace::BuildNode(DockNode& node, UIElement* parent) {
    return node.IsSplit() ? BuildSplit(node, parent) : BuildTabs(node, parent);
}

UIElement* DockSpace::BuildSplit(DockNode& node, UIElement* parent) {
    UIElement* box = Ctx().CreateIn<UIElement>(parent);
    box->SetName(node.Vertical ? "SplitV" : "SplitH");
    box->SetStretch(true, true);
    if (node.Children.size() < 2) return box;

    const float half = m_splitter * 0.5f;
    // Доли, а не пиксели: область обязана делиться в той же пропорции при любом
    // размере окна. Пиксельный размер первой половины после смены разрешения
    // означал бы «панель слева всегда 300, даже если экран стал 320».
    UIElement* a = BuildNode(*node.Children[0], box);
    UITransform& at = a->Ensure<UITransform>();
    at.AnchorMin = {0.0f, 0.0f};
    at.AnchorMax = node.Vertical ? glm::vec2{1.0f, node.Ratio} : glm::vec2{node.Ratio, 1.0f};
    at.WidthMode = at.HeightMode = UISizeMode::Stretch;
    at.Margin = node.Vertical ? UIEdges(0.0f, 0.0f, 0.0f, half) : UIEdges(0.0f, 0.0f, half, 0.0f);

    UIElement* b = BuildNode(*node.Children[1], box);
    UITransform& bt = b->Ensure<UITransform>();
    bt.AnchorMin = node.Vertical ? glm::vec2{0.0f, node.Ratio} : glm::vec2{node.Ratio, 0.0f};
    bt.AnchorMax = {1.0f, 1.0f};
    bt.WidthMode = bt.HeightMode = UISizeMode::Stretch;
    bt.Margin = node.Vertical ? UIEdges(0.0f, half, 0.0f, 0.0f) : UIEdges(half, 0.0f, 0.0f, 0.0f);

    // --- Полоса-разделитель ---
    Panel* grip = Ctx().CreateIn<Panel>(box);
    grip->SetName("Splitter");
    grip->SetStyle("Splitter");
    UITransform& gt = grip->Ensure<UITransform>();
    gt.AnchorMin = node.Vertical ? glm::vec2{0.0f, node.Ratio} : glm::vec2{node.Ratio, 0.0f};
    gt.AnchorMax = node.Vertical ? glm::vec2{1.0f, node.Ratio} : glm::vec2{node.Ratio, 1.0f};
    gt.Pivot = {0.5f, 0.5f};
    if (node.Vertical) {
        gt.WidthMode = UISizeMode::Stretch;
        gt.Size.y = m_splitter;
    } else {
        gt.HeightMode = UISizeMode::Stretch;
        gt.Size.x = m_splitter;
    }
    UIInteraction& gi = grip->Ensure<UIInteraction>();
    gi.Draggable = true;
    gi.Cursor = node.Vertical ? "resize-ns" : "resize-ew";

    DockNode* model = &node;
    // Ratio меняется в МОДЕЛИ, а дерево пересобирается. Двигать элементы
    // напрямую значило бы, что сохранённая раскладка не совпадает с видимой.
    grip->On(UIEventType::Drag, [this, model, box](UIEvent& e) {
        const UIRect area = box->Bounds();
        const float span = model->Vertical ? area.h : area.w;
        if (span <= 1.0f) return;
        const float pos = model->Vertical ? e.Pointer.y - area.y : e.Pointer.x - area.x;
        // Минимум с каждой стороны: область, стянутую в ноль, вернуть нечем —
        // хватать больше не за что.
        const float minFrac = std::min(0.45f, 24.0f / span);
        model->Ratio = std::min(1.0f - minFrac, std::max(minFrac, pos / span));
        Invalidate();
    });
    return box;
}

UIElement* DockSpace::BuildTabs(DockNode& node, UIElement* parent) {
    UIElement* box = Ctx().CreateIn<UIElement>(parent);
    box->SetName("DockArea");
    box->SetStretch(true, true);
    box->Vertical(0.0f)->Padding(UIEdges::Uniform(0.0f));
    if (node.Panels.empty()) return box;

    node.Active = std::min(std::max(node.Active, 0), (int)node.Panels.size() - 1);

    // --- Полоса вкладок ---
    Panel* strip = Ctx().CreateIn<Panel>(box);
    strip->SetName("TabStrip");
    strip->SetStyle("TabStrip");
    strip->SetHeight(26.0f);
    strip->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    strip->Horizontal(2.0f)->Padding(UIEdges(4.0f, 3.0f, 4.0f, 0.0f));

    DockNode* model = &node;
    for (size_t i = 0; i < node.Panels.size(); ++i) {
        DockPanel* panel = Find(node.Panels[i]);
        const std::string title = panel ? panel->Title() : node.Panels[i];
        Button* tab = Ctx().CreateIn<Button>(strip, title, std::string());
        tab->SetName("Tab");
        tab->SetHeight(22.0f);
        tab->FitToText(12.0f);
        // Активная вкладка отличается СТИЛЕМ, а не набором полей: перекрасить
        // все вкладки редактора должно быть правкой в одном месте.
        tab->SetStyle((int)i == node.Active ? "TabActive" : "Tab");
        const int index = (int)i;
        tab->OnClick([this, model, index] {
            model->Active = index;
            Invalidate();
        });
        // Средняя кнопка закрывает вкладку — привычка из любого редактора.
        const std::string id = node.Panels[i];
        tab->On(UIEventType::PointerDown, [this, id](UIEvent& e) {
            if (e.Button == 2) Close(id);
        });

        // Вкладку можно утащить в другую область или наружу — в своё окно.
        tab->Ensure<UIInteraction>().Draggable = true;
        tab->On(UIEventType::DragStart, [this, id](UIEvent&) { BeginDrag(id); });
        tab->On(UIEventType::Drag, [this](UIEvent& e) { DragTo(e.Pointer); });
        tab->On(UIEventType::DragEnd, [this](UIEvent& e) { EndDrag(e.Pointer); });
    }

    // --- Содержимое активной вкладки ---
    UIElement* content = Ctx().CreateIn<UIElement>(box);
    content->SetName("DockContent");
    UITransform& ct = content->Ensure<UITransform>();
    ct.WidthMode = UISizeMode::Stretch;
    ct.HeightMode = UISizeMode::Stretch;

    if (DockPanel* active = Find(node.Panels[(size_t)node.Active])) content->Add(active);
    return box;
}

// --- Перетаскивание панелей (§24) --------------------------------------------

void DockSpace::BeginDrag(const std::string& id) {
    if (!Find(id)) return;
    m_drag = id;
    m_dropTarget.clear();
    m_dropSide = DockSide::Center;
}

// Область под курсором и ближайший к нему край.
//
// Пятая часть стороны с каждого края — это «пристыковать сюда», середина —
// «стать вкладкой». Пропорция, а не пиксели: в узкой панели полоса в 40
// пикселей заняла бы её целиком, и «стать вкладкой» стало бы недостижимо.
bool DockSpace::HitArea(glm::vec2 point, std::string& outPanelId, DockSide& outSide) const {
    for (DockPanel* p : m_panels) {
        if (p->Id() == m_drag || IsFloating(p->Id()) || !FindTabsOf(p->Id())) continue;
        const UIRect r = p->Bounds();
        if (r.w <= 1.0f || r.h <= 1.0f) continue;
        if (point.x < r.x || point.y < r.y || point.x > r.x + r.w || point.y > r.y + r.h)
            continue;

        const float fx = (point.x - r.x) / r.w;
        const float fy = (point.y - r.y) / r.h;
        const float edge = 0.2f;
        outPanelId = p->Id();
        if (fx < edge) outSide = DockSide::Left;
        else if (fx > 1.0f - edge) outSide = DockSide::Right;
        else if (fy < edge) outSide = DockSide::Top;
        else if (fy > 1.0f - edge) outSide = DockSide::Bottom;
        else outSide = DockSide::Center;
        return true;
    }
    return false;
}

void DockSpace::DragTo(glm::vec2 point) {
    if (m_drag.empty()) return;
    std::string id;
    DockSide side = DockSide::Center;
    if (HitArea(point, id, side)) {
        m_dropTarget = id;
        m_dropSide = side;
    } else {
        m_dropTarget.clear();
    }
    UpdateDropHint();
}

// Подсветка места броска. Показывать её обязательно: бросок вслепую — это
// раскладка, которую человек каждый раз получает не ту, что хотел, и правит
// обратно.
void DockSpace::UpdateDropHint() {
    if (m_dropTarget.empty()) {
        if (m_hint) m_hint->SetVisible(false);
        return;
    }
    DockPanel* target = Find(m_dropTarget);
    if (!target) return;
    if (!m_hint) {
        Panel* box = Ctx().CreateIn<Panel>(Ctx().Layer(UILayer::Overlay));
        box->SetName("DropHint");
        box->SetStyle("DropHint");
        box->SetColor(UIColor(0.95f, 0.76f, 0.20f, 0.28f));
        box->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
        // Подсветка не ловит мышь: иначе она перекрыла бы область, которую
        // подсвечивает, и следующий кадр показал бы другое место.
        box->Ensure<UIInteraction>().Hit = UIHitShape::None;
        m_hint = box;
    }
    const UIRect r = target->Bounds();
    UIRect hint = r;
    switch (m_dropSide) {
        case DockSide::Left:   hint.w = r.w * 0.3f; break;
        case DockSide::Right:  hint.x = r.x + r.w * 0.7f; hint.w = r.w * 0.3f; break;
        case DockSide::Top:    hint.h = r.h * 0.3f; break;
        case DockSide::Bottom: hint.y = r.y + r.h * 0.7f; hint.h = r.h * 0.3f; break;
        case DockSide::Center: break;
    }
    const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
    m_hint->SetVisible(true);
    m_hint->SetPosition({hint.x / scale, hint.y / scale});
    m_hint->SetSize({hint.w / scale, hint.h / scale});
}

void DockSpace::EndDrag(glm::vec2 point) {
    if (m_drag.empty()) return;
    const std::string id = m_drag;
    m_drag.clear();
    if (m_hint) m_hint->SetVisible(false);

    std::string target;
    DockSide side = DockSide::Center;
    if (!HitArea(point, target, side)) {
        // Бросок мимо всех областей — отцепить в отдельное окно. Это привычный
        // жест: вытащил панель за пределы редактора — получил окно.
        m_dropTarget.clear();
        Float(id, point);
        return;
    }
    // Бросок на самого себя ничего не меняет: пересобирать раскладку ради
    // этого — значит мигать экраном на каждом промахе.
    if (target == id) return;

    RemoveFromLayout(id);
    Insert(id, side, target);
    m_dropTarget.clear();
    m_dirty = true;
}

// --- Раскладка как данные ----------------------------------------------------

namespace {

json NodeToJson(const DockNode& node) {
    json j;
    if (node.IsSplit()) {
        j["type"] = "split";
        j["vertical"] = node.Vertical;
        j["ratio"] = node.Ratio;
        json kids = json::array();
        for (const auto& c : node.Children) kids.push_back(NodeToJson(*c));
        j["children"] = std::move(kids);
    } else {
        j["type"] = "tabs";
        j["panels"] = node.Panels;
        j["active"] = node.Active;
    }
    return j;
}

std::unique_ptr<DockNode> NodeFromJson(const json& j) {
    auto node = std::make_unique<DockNode>();
    if (!j.is_object()) return node;
    if (j.value("type", std::string("tabs")) == "split") {
        node->Type = DockNode::Kind::Split;
        node->Vertical = j.value("vertical", false);
        node->Ratio = j.value("ratio", 0.5f);
        if (j.contains("children") && j["children"].is_array())
            for (const json& c : j["children"]) node->Children.push_back(NodeFromJson(c));
        // Разделитель ровно с двумя детьми: файл, где их больше или меньше,
        // испорчен, и достраивать его догадками — значит получить раскладку,
        // которую человек не узнает.
        while (node->Children.size() > 2) node->Children.pop_back();
        while (node->Children.size() < 2) node->Children.push_back(std::make_unique<DockNode>());
    } else {
        if (j.contains("panels") && j["panels"].is_array())
            for (const json& p : j["panels"])
                if (p.is_string()) node->Panels.push_back(p.get<std::string>());
        node->Active = j.value("active", 0);
    }
    return node;
}

} // namespace

std::string DockSpace::SaveLayout() const {
    json root;
    root["version"] = 1;
    root["splitter"] = m_splitter;
    if (m_root) root["layout"] = NodeToJson(*m_root);
    json floating = json::array();
    for (const Floating& f : m_floating) {
        json w;
        w["panel"] = f.Id;
        if (f.Win) {
            w["x"] = f.Win->Position().x;
            w["y"] = f.Win->Position().y;
            w["w"] = f.Win->Size().x;
            w["h"] = f.Win->Size().y;
        }
        floating.push_back(std::move(w));
    }
    root["floating"] = std::move(floating);
    return root.dump(2);
}

bool DockSpace::LoadLayout(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("layout")) {
        // Битый файл раскладки — не повод падать и не повод молчать: редактор
        // открывается со стандартной раскладкой, а причина написана в логе.
        LOG_WARN("UI") << "раскладка дока не прочитана — беру стандартную";
        return false;
    }
    // Плавающие окна прошлого сеанса убираем: их положение приедет из файла.
    for (const Floating& f : m_floating) {
        if (DockPanel* p = Find(f.Id)) m_parking->Add(p);
        if (f.Win) Ctx().Destroy(f.Win);
    }
    m_floating.clear();

    m_splitter = j.value("splitter", 4.0f);
    m_root = NodeFromJson(j["layout"]);

    // Панели, которых в файле нет, — это новые панели редактора, появившиеся
    // после того, как человек сохранил раскладку. Терять их нельзя: иначе
    // новая панель не появится ни у кого, кто хоть раз двигал окна.
    for (DockPanel* p : m_panels)
        if (!FindTabsOf(p->Id())) Insert(p->Id(), DockSide::Center, {});

    if (j.contains("floating") && j["floating"].is_array()) {
        for (const json& w : j["floating"]) {
            const std::string id = w.value("panel", std::string());
            if (id.empty() || !Find(id)) continue;
            Float(id, {w.value("x", 80.0f), w.value("y", 80.0f)});
            for (Floating& f : m_floating) {
                if (f.Id == id && f.Win)
                    f.Win->SetSize({w.value("w", 360.0f), w.value("h", 280.0f)});
            }
        }
    }
    m_dirty = true;
    return true;
}

} // namespace sage::ui::sui
