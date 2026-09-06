#include "sage/ui/sageui/UIContext.h"

#include "sage/core/Log.h"
#include "sage/ui/render/UIBackend.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIRegistry.h"
#include "sage/ui/sageui/UIWindow.h"
#include "sage/ui/visual/UIFill.h"

namespace sage::ui::sui {

namespace {

// Имена слоёв видны в дереве документа и в отладке — человек должен понимать,
// почему подсказка лежит не там же, где кнопка.
const char* LayerName(UILayer layer) {
    switch (layer) {
        case UILayer::Background: return "Background";
        case UILayer::Content: return "Content";
        case UILayer::Overlay: return "Overlay";
        case UILayer::Popup: return "Popup";
        case UILayer::Tooltip: return "Tooltip";
        case UILayer::Modal: return "Modal";
        case UILayer::Debug: return "Debug";
    }
    return "Layer";
}

} // namespace

UIContext::UIContext() {
    UIInitialize();
    RegisterBuiltinUIElements();

    // Корень — во весь экран и без раскладки: слои кладутся на него якорями,
    // и каждый занимает весь кадр. Раскладка на корне превратила бы слои в
    // столбец, что означало бы «подсказка ниже содержимого», а не «поверх».
    auto root = std::unique_ptr<UIElement>(new UIElement());
    m_root = root.get();
    Attach(std::move(root), kUIInvalidNode);
    m_root->SetName("Root")->SetStretch(true, true);
}

UIContext::~UIContext() {
    // Сначала элементы, потом документ: деструктор элемента снимает подписки с
    // шины, а шина живёт в рантайме.
    m_elements.clear();
}

void UIContext::Attach(std::unique_ptr<UIElement> element, UINodeId parent) {
    UINode* node = m_rt.Doc().Create("Element", parent);
    element->m_ctx = this;
    element->m_id = node->Id;
    UIElement* raw = element.get();
    m_elements[node->Id] = std::move(element);
    raw->OnAttach();
}

void UIContext::Forget(UINodeId id) {
    // Поддерево уходит целиком: узлы уже удалены документом, и элементы без
    // узлов — это указатели в никуда.
    for (auto it = m_elements.begin(); it != m_elements.end();) {
        if (!m_rt.Doc().Find(it->first)) it = m_elements.erase(it);
        else ++it;
    }
    (void)id;
}

void UIContext::Destroy(UIElement* element) {
    if (!element || element == m_root) return;
    const UINodeId id = element->NodeId();
    m_rt.Doc().Destroy(id);
    Forget(id);
}

UIElement* UIContext::Find(UINodeId id) const {
    auto it = m_elements.find(id);
    return it == m_elements.end() ? nullptr : it->second.get();
}

UIElement* UIContext::FindByName(const std::string& name) const {
    for (const auto& [id, e] : m_elements)
        if (e->Name() == name) return e.get();
    return nullptr;
}

UIElement* UIContext::CreateByName(const std::string& typeName) {
    std::unique_ptr<UIElement> made = UIElementRegistry::Instance().Make(typeName);
    if (!made) {
        // Имени нет в реестре — это опечатка или незагруженный плагин, и она
        // обязана быть видна: молча вернуть nullptr значит получить пустое
        // место в интерфейсе без единой строки в логе (§134).
        LOG_WARN("UI") << "неизвестный тип элемента \"" << typeName << "\"";
        return nullptr;
    }
    UIElement* raw = made.get();
    Attach(std::move(made), kUIInvalidNode);
    raw->SetName(typeName);
    return raw;
}

UIElement* UIContext::Layer(UILayer layer) {
    const int key = (int)layer;
    auto it = m_layers.find(key);
    if (it != m_layers.end()) return it->second;

    UIElement* e = Create<UIElement>();
    m_root->Add(e);
    e->SetName(LayerName(layer))->SetStretch(true, true);
    if (UINode* n = e->Node()) n->Layer = key;
    m_layers[key] = e;
    return e;
}

void UIContext::SetScreen(glm::vec2 pixels) { m_rt.SetScreen(pixels); }

glm::vec2 UIContext::Screen() const { return m_rt.Context().ScreenPixels; }

void UIContext::SetScale(float scale) {
    m_scale = scale;
    // Масштаб интерфейса — свойство ХОЛСТА, а не отдельного элемента: иначе
    // «увеличить интерфейс» означало бы обойти дерево и умножить каждый размер.
    // UserScale, а не подмена режима холста: режим отвечает на другой вопрос
    // («как экран соотносится с опорным разрешением»), и затирать его ради
    // пользовательской настройки значит терять адаптивность вместе с ней.
    m_rt.Doc().Canvas().UserScale = scale;
    m_rt.Doc().MarkDirty(UIDirty_Layout);
}

// Экранные пиксели один к одному. Для редактора и инструментов это
// единственный правильный режим: панель шириной 240 обязана быть 240 пикселями,
// а не «240 при опорном разрешении», иначе на другом мониторе всё разъезжается.
void UIContext::SetPixelPerfect() {
    m_rt.Doc().Canvas().Scale = UICanvasSettings::ScaleMode::Pixels;
    m_rt.Doc().MarkDirty(UIDirty_Layout);
}

// Опорное разрешение — для игрового интерфейса, который должен одинаково
// выглядеть на любом экране.
void UIContext::SetReference(glm::vec2 reference, float matchWidthOrHeight) {
    UICanvasSettings& canvas = m_rt.Doc().Canvas();
    canvas.Scale = UICanvasSettings::ScaleMode::ScaleWithSize;
    canvas.Reference = reference;
    canvas.MatchWidthOrHeight = matchWidthOrHeight;
    m_rt.Doc().MarkDirty(UIDirty_Layout);
}

float UIContext::Scale() const { return m_scale; }

// --- Всплывающие -------------------------------------------------------------

void UIContext::RegisterOpenPopup(Popup* popup) {
    for (Popup* p : m_popups)
        if (p == popup) return;
    m_popups.push_back(popup);
}

void UIContext::UnregisterOpenPopup(Popup* popup) {
    for (size_t i = 0; i < m_popups.size(); ++i) {
        if (m_popups[i] == popup) {
            m_popups.erase(m_popups.begin() + (long)i);
            return;
        }
    }
}

void UIContext::CloseAllPopups() {
    // По копии: Close() снимает себя из списка, и обход по живому вектору
    // пропустил бы половину меню.
    std::vector<Popup*> open = m_popups;
    for (Popup* p : open) p->Close();
    m_popups.clear();
}

// --- Подсказки ---------------------------------------------------------------

void UIContext::UpdateTooltip(float dt) {
    (void)dt;
    // Узел под курсором объявляет ключ подсказки — больше ему знать ничего не
    // нужно. Ни таймера, ни позиции, ни узла подсказки: это работа контекста.
    UINode* node = m_hovered != kUIInvalidNode ? m_rt.Doc().Find(m_hovered) : nullptr;
    const UIInteraction* ia = node ? node->Get<UIInteraction>() : nullptr;
    const bool wants = ia && !ia->TooltipKey.empty() && ia->Runtime.HoverTime >= m_tooltipDelay;

    if (!wants) {
        if (m_tooltip && m_tooltip->IsVisible()) m_tooltip->SetVisible(false);
        m_tooltipFor = kUIInvalidNode;
        return;
    }
    if (!m_tooltip) {
        Panel* box = Create<Panel>();
        Layer(UILayer::Tooltip)->Add(box);
        box->SetName("TooltipBox");
        box->SetStyle("Tooltip");
        box->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
        box->Vertical(0.0f)->Padding(UIEdges(8.0f, 5.0f, 8.0f, 5.0f));
        box->FitContent(true, true);
        // Подсказка НЕ ловит мышь: иначе она перекрывает то, о чём
        // рассказывает, и наведение начинает мигать.
        box->Ensure<UIInteraction>().Hit = UIHitShape::None;
        m_tooltip = box;
        m_tooltipText = CreateIn<Label>(box, std::string());
    }
    if (m_tooltipFor != m_hovered) {
        m_tooltipFor = m_hovered;
        const std::string text = m_rt.Context().Localize ? m_rt.Context().Localize(ia->TooltipKey)
                                                         : ia->TooltipKey;
        m_tooltipText->SetText(text);
    }
    m_tooltip->SetVisible(true);

    // Рядом с узлом, а не под курсором: подсказка, привязанная к курсору,
    // дрожит вместе с ним и мешает читать саму себя.
    UIRect host{};
    m_rt.Layout().RectOf(m_hovered, host);
    const UIRect tip = m_tooltip->Bounds();
    const glm::vec2 screen = m_rt.Context().ScreenPixels;
    glm::vec2 p{host.x, host.y + host.h + 6.0f};
    if (p.x + tip.w > screen.x) p.x = std::max(0.0f, screen.x - tip.w);
    if (p.y + tip.h > screen.y) p.y = std::max(0.0f, host.y - tip.h - 6.0f);
    const float scale = std::max(0.0001f, m_rt.Layout().CanvasScale());
    m_tooltip->SetPosition(p / scale);
}

void UIContext::Update(float dt) {
    // Элементы обновляются ДО раскладки и в порядке дерева: список, дорисовавший
    // строки, обязан попасть в ЭТУ раскладку, а не в следующую — иначе новая
    // строка появляется с опозданием на кадр, и это видно.
    //
    // По КОПИИ списка. Update элемента вправе создавать и удалять узлы — именно
    // этим занимается док, пересобирая свои области, — а Ordered() отдаёт
    // внутренний кэш документа. Обход его по ссылке во время правки дерева
    // означает работу с переехавшей памятью: половина элементов молча не
    // получает Update, и «панель посчиталась, но не появилась» становится
    // обычным делом.
    const std::vector<UINodeId> order = m_rt.Doc().Ordered();
    for (UINodeId id : order) {
        // Узел мог исчезнуть, пока обновлялся сосед.
        if (!m_rt.Doc().Find(id)) continue;
        if (UIElement* e = Find(id)) e->Update(dt);
    }
    UpdateTooltip(dt);
    m_rt.Update(dt);
}

UIInputReport UIContext::HandleInput(const UIInputFrame& input) {
    const UIInputReport r = m_rt.HandleInput(input);
    m_hovered = r.Hovered;

    // Щелчок МИМО открытого меню закрывает его. Проверяется по попаданию: если
    // курсор не над ни одним из открытых меню, а кнопку нажали — закрываем.
    // Ведёт это контекст, а не сами меню: два меню, каждое со своим
    // обработчиком на весь экран, закрывали бы друг друга по кругу.
    if (!m_popups.empty() && input.Buttons[0]) {
        bool inside = false;
        for (Popup* p : m_popups) {
            if (!p->IsVisible()) continue;
            UINodeId walk = r.Hovered;
            while (walk != kUIInvalidNode) {
                if (walk == p->NodeId()) { inside = true; break; }
                const UINode* n = m_rt.Doc().Find(walk);
                walk = n ? n->Parent : kUIInvalidNode;
            }
            if (inside) break;
        }
        if (!inside) CloseAllPopups();
    }
    // Escape закрывает верхнее меню. Обязателен наравне со щелчком: с
    // клавиатуры меню иначе не закрыть вовсе.
    for (int key : input.KeysDown) {
        if (key == 256 /* GLFW_KEY_ESCAPE */ && !m_popups.empty()) {
            m_popups.back()->Close();
            break;
        }
    }
    return r;
}

void UIContext::Render(UIRenderer& renderer, Framebuffer* root) {
    UIClassicBackend backend(renderer);
    backend.SetRootTarget(root);
    m_rt.Render(backend);
}

void UIContext::InstallEngineResources() {
    if (!m_resources) m_resources = std::make_unique<UIEngineResources>();
    m_resources->Install(m_rt.Context());
}

} // namespace sage::ui::sui
