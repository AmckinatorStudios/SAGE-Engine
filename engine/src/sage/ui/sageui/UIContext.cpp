#include "sage/ui/sageui/UIContext.h"

#include "sage/core/Log.h"
#include "sage/ui/render/UIBackend.h"
#include "sage/ui/sageui/UIRegistry.h"

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

void UIContext::Update(float dt) {
    // Элементы обновляются ДО раскладки и в порядке дерева: список, дорисовавший
    // строки, обязан попасть в ЭТУ раскладку, а не в следующую — иначе новая
    // строка появляется с опозданием на кадр, и это видно.
    for (UINodeId id : m_rt.Doc().Ordered()) {
        if (UIElement* e = Find(id)) e->Update(dt);
    }
    m_rt.Update(dt);
}

UIInputReport UIContext::HandleInput(const UIInputFrame& input) {
    return m_rt.HandleInput(input);
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
