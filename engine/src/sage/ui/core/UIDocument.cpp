#include "sage/ui/core/UIDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/layout/UITransform.h"

namespace sage::ui {

// Версия схемы документа. Растёт при КАЖДОМ несовместимом изменении формата;
// совместимые (новое свойство со значением по умолчанию) её не трогают — они
// по определению читаются старыми файлами (§65).
static constexpr int kUIFormatVersion = 1;

int UIDocument::FormatVersion() { return kUIFormatVersion; }

UIDocument::UIDocument() {
    // Реестр компонентов обязан быть готов до создания первого узла: узлу
    // сразу нужен UITransform.
    RegisterBuiltinUIComponents();
}

std::string UIDocument::MakeGuid() {
    // Не UUID v4 и намеренно: документу нужна СТАБИЛЬНАЯ уникальность внутри
    // себя, а не глобальная. Счётчик плюс имя документа дают её без источника
    // случайности, а значит — воспроизводимо в тестах и в сравнении файлов.
    char buf[40];
    std::snprintf(buf, sizeof(buf), "n%08llx", (unsigned long long)(++m_guidCounter));
    return buf;
}

UINode* UIDocument::Create(const std::string& name, UINodeId parent) {
    auto node = std::make_unique<UINode>();
    node->Id = m_nextId++;
    node->Guid = MakeGuid();
    node->Name = name.empty() ? "Node" : name;
    node->Ensure<UITransform>(); // §6: узел без прямоугольника негде рисовать

    const UINodeId id = node->Id;
    UINode* raw = node.get();
    m_nodes.emplace(id, std::move(node));

    if (parent != kUIInvalidNode) {
        if (UINode* p = Find(parent)) {
            raw->Parent = parent;
            p->Children.push_back(id);
        } else {
            m_roots.push_back(id);
        }
    } else {
        m_roots.push_back(id);
    }

    m_orderValid = false;
    MarkDirty(UIDirty_Hierarchy | UIDirty_Layout);
    return raw;
}

UINodeId UIDocument::CreateWidget(std::string_view widgetId, UINodeId parent) {
    return UIWidgetRegistry::Instance().Build(widgetId, *this, parent);
}

UINode* UIDocument::Find(UINodeId id) {
    auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second.get();
}
const UINode* UIDocument::Find(UINodeId id) const {
    auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second.get();
}

UINode* UIDocument::FindByGuid(std::string_view guid) {
    for (auto& [id, node] : m_nodes)
        if (node->Guid == guid) return node.get();
    return nullptr;
}

UINode* UIDocument::FindByName(std::string_view name) {
    for (UINodeId id : Ordered()) {
        UINode* n = Find(id);
        if (n && n->Name == name) return n;
    }
    return nullptr;
}

UINode* UIDocument::FindByPath(std::string_view path) {
    // Путь "Root/Panel/Title": каждый шаг ищется СРЕДИ ДЕТЕЙ предыдущего, а не
    // по всему документу. Иначе два "Title" в разных панелях сделали бы путь
    // неоднозначным — то есть бесполезным для анимаций и привязок.
    UINode* current = nullptr;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string_view part =
            path.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (part.empty()) return nullptr;

        UINode* next = nullptr;
        if (!current) {
            for (UINodeId id : m_roots) {
                UINode* n = Find(id);
                if (n && n->Name == part) { next = n; break; }
            }
        } else {
            for (UINodeId id : current->Children) {
                UINode* n = Find(id);
                if (n && n->Name == part) { next = n; break; }
            }
        }
        if (!next) return nullptr;
        current = next;
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return current;
}

std::string UIDocument::PathOf(UINodeId id) const {
    std::string out;
    const UINode* n = Find(id);
    while (n) {
        out = out.empty() ? n->Name : n->Name + "/" + out;
        n = n->Parent == kUIInvalidNode ? nullptr : Find(n->Parent);
    }
    return out;
}

void UIDocument::Detach(UINode& node) {
    std::vector<UINodeId>* list = &m_roots;
    if (node.Parent != kUIInvalidNode) {
        if (UINode* p = Find(node.Parent)) list = &p->Children;
    }
    list->erase(std::remove(list->begin(), list->end(), node.Id), list->end());
}

void UIDocument::CollectSubtree(UINodeId id, std::vector<UINodeId>& out) const {
    const UINode* n = Find(id);
    if (!n) return;
    out.push_back(id);
    for (UINodeId c : n->Children) CollectSubtree(c, out);
}

void UIDocument::Destroy(UINodeId id) {
    UINode* node = Find(id);
    if (!node) return;
    Detach(*node);

    std::vector<UINodeId> doomed;
    CollectSubtree(id, doomed);
    for (UINodeId d : doomed) m_nodes.erase(d);

    m_orderValid = false;
    MarkDirty(UIDirty_Hierarchy | UIDirty_Layout);
}

UINodeId UIDocument::Duplicate(UINodeId id, UINodeId newParent) {
    const UINode* src = Find(id);
    if (!src) return kUIInvalidNode;
    if (newParent == kUIInvalidNode) newParent = src->Parent;

    UINode* copy = Create(src->Name, newParent);
    // Копируются ВСЕ свойства узла и все компоненты через Clone — в том числе
    // чужие, о которых движок ничего не знает. Список «что копировать» тут
    // невозможен по устройству, а значит, не может устареть.
    copy->Enabled = src->Enabled;
    copy->Visible = src->Visible;
    copy->Opacity = src->Opacity;
    copy->Blend = src->Blend;
    copy->Layer = src->Layer;
    copy->Order = src->Order;
    copy->Locked = src->Locked;
    copy->EditorHidden = src->EditorHidden;
    copy->PrefabSource = src->PrefabSource;
    copy->PrefabRoot = src->PrefabRoot;
    copy->Components.clear();
    for (const auto& c : src->Components) copy->Components.push_back(c->Clone());

    const UINodeId copyId = copy->Id;
    // Дети копируются по номерам, снятым ЗАРАНЕЕ: Create ниже меняет
    // m_nodes, и держать указатель на src через эти вызовы нельзя.
    const std::vector<UINodeId> children = src->Children;
    for (UINodeId child : children) Duplicate(child, copyId);
    return copyId;
}

bool UIDocument::IsAncestorOf(UINodeId ancestor, UINodeId node) const {
    const UINode* n = Find(node);
    while (n && n->Parent != kUIInvalidNode) {
        if (n->Parent == ancestor) return true;
        n = Find(n->Parent);
    }
    return false;
}

bool UIDocument::Reparent(UINodeId id, UINodeId newParent, int index) {
    UINode* node = Find(id);
    if (!node) return false;
    // §135: узел внутри собственного поддерева — не «странная вёрстка», а
    // бесконечный обход при первой же отрисовке. Отказ, а не молчаливое
    // исправление: иначе перетаскивание в редакторе делает не то, что просили.
    if (newParent == id || IsAncestorOf(id, newParent)) return false;
    if (newParent != kUIInvalidNode && !Find(newParent)) return false;

    Detach(*node);
    node->Parent = newParent;
    std::vector<UINodeId>* list = &m_roots;
    if (newParent != kUIInvalidNode) list = &Find(newParent)->Children;
    if (index < 0 || index > (int)list->size()) list->push_back(id);
    else list->insert(list->begin() + index, id);

    m_orderValid = false;
    MarkDirty(UIDirty_Hierarchy | UIDirty_Layout);
    return true;
}

bool UIDocument::Move(UINodeId id, int index) {
    UINode* node = Find(id);
    if (!node) return false;
    return Reparent(id, node->Parent, index);
}

const std::vector<UINodeId>& UIDocument::Ordered() const {
    if (!m_orderValid) RebuildOrder();
    return m_order;
}

void UIDocument::RebuildOrder() const {
    m_order.clear();
    m_order.reserve(m_nodes.size());
    // Обход в глубину: родитель раньше детей. Именно этот порядок нужен и
    // раскладке (родитель обязан быть посчитан первым), и рисованию (потомок
    // поверх предка при равных слоях).
    std::vector<UINodeId> stack(m_roots.rbegin(), m_roots.rend());
    while (!stack.empty()) {
        const UINodeId id = stack.back();
        stack.pop_back();
        auto it = m_nodes.find(id);
        if (it == m_nodes.end()) continue;
        m_order.push_back(id);
        const auto& children = it->second->Children;
        for (auto rit = children.rbegin(); rit != children.rend(); ++rit) stack.push_back(*rit);
    }
    m_orderValid = true;
}

void UIDocument::Traverse(UINodeId root, const std::function<bool(UINode&)>& fn) {
    UINode* n = Find(root);
    if (!n) return;
    if (!fn(*n)) return;
    const std::vector<UINodeId> children = n->Children;
    for (UINodeId c : children) Traverse(c, fn);
}

void UIDocument::TraverseConst(UINodeId root, const std::function<bool(const UINode&)>& fn) const {
    const UINode* n = Find(root);
    if (!n) return;
    if (!fn(*n)) return;
    for (UINodeId c : n->Children) TraverseConst(c, fn);
}

void UIDocument::MarkDirty(UINodeId id, uint32_t flags) {
    m_dirty |= flags;
    // Список изменившихся узлов ограничен: если изменилось «слишком многое»,
    // дешевле пересчитать всё, чем вести список размером с документ (§88).
    if (m_dirtyNodes.size() < 256) m_dirtyNodes.push_back(id);
}

float UIDocument::ScaleFor(glm::vec2 screen) const {
    const UICanvasSettings& c = m_canvas;
    float scale = 1.0f;
    switch (c.Scale) {
        case UICanvasSettings::ScaleMode::Pixels: scale = 1.0f; break;
        case UICanvasSettings::ScaleMode::Physical: scale = c.DpiScale; break;
        case UICanvasSettings::ScaleMode::ScaleWithSize: {
            if (c.Reference.x <= 0.0f || c.Reference.y <= 0.0f) { scale = 1.0f; break; }
            const float sx = screen.x / c.Reference.x;
            const float sy = screen.y / c.Reference.y;
            const float t = UIClamp01(c.MatchWidthOrHeight);
            switch (c.Aspect) {
                // Fit — вписать целиком: берётся МЕНЬШИЙ множитель, иначе часть
                // интерфейса уезжает за край экрана с другим соотношением сторон.
                case UICanvasSettings::AspectMode::Fit: scale = std::min(sx, sy); break;
                // Crop — заполнить: больший множитель, край срезается.
                case UICanvasSettings::AspectMode::Crop: scale = std::max(sx, sy); break;
                // Expand — смешать по MatchWidthOrHeight. Геометрическое среднее,
                // а не арифметическое: масштабы перемножаются, и среднее между
                // «в два раза шире» и «в два раза уже» обязано давать единицу.
                default: scale = std::pow(sx, 1.0f - t) * std::pow(sy, t); break;
            }
            break;
        }
    }
    scale *= (c.UserScale > 0.0f ? c.UserScale : 1.0f);
    return scale > 0.0001f ? scale : 0.0001f;
}

UIRect UIDocument::ViewportFor(glm::vec2 screen) const {
    const float s = ScaleFor(screen);
    UIRect r{0.0f, 0.0f, screen.x / s, screen.y / s};
    if (m_canvas.RespectSafeArea) {
        // Безопасная область задана в ПИКСЕЛЯХ экрана (её сообщает система), а
        // вёрстка идёт в логических единицах — переводим здесь, один раз.
        const UIEdges& sa = m_canvas.SafeArea;
        r = UIDeflate(r, UIEdges(sa.L / s, sa.T / s, sa.R / s, sa.B / s));
    }
    return r;
}

void UIDocument::Clear() {
    m_nodes.clear();
    m_roots.clear();
    m_order.clear();
    m_orderValid = true;
    m_nextId = 1;
    m_guidCounter = 0;
    m_dirty = UIDirty_All;
    m_dirtyNodes.clear();
}

} // namespace sage::ui
