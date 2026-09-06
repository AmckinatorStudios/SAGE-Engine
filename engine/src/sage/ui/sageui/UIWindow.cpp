#include "sage/ui/sageui/UIWindow.h"

#include <algorithm>

#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIFill.h"

namespace sage::ui::sui {

// ============================================================================
//  Window
// ============================================================================

Window::Window(std::string title) : m_title(std::move(title)) {}

void Window::OnAttach() {
    Panel::OnAttach();
    SetName(m_title.empty() ? "Window" : m_title);
    SetStyle("Window");
    SetSize({420.0f, 300.0f});
    // Якорь в левый верх и свой Pivot: окно двигают в АБСОЛЮТНЫХ координатах,
    // и любая привязка к центру превращала бы перетаскивание в задачу с
    // пересчётом на каждый кадр.
    SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    Vertical(0.0f)->Padding(UIEdges::Uniform(0.0f));
    // Содержимое обрезается рамкой окна: иначе длинный список вылезает за края
    // и рисуется поверх соседних окон.
    ClipChildren(true, 8.0f);

    // --- Заголовок ---
    m_titleBar = Ctx().Create<Panel>();
    Add(m_titleBar);
    m_titleBar->SetName("TitleBar");
    m_titleBar->SetStyle("WindowTitle");
    m_titleBar->SetHeight(28.0f);
    m_titleBar->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    m_titleBar->Horizontal(6.0f)->Padding(UIEdges(10.0f, 0.0f, 6.0f, 0.0f));
    m_titleBar->Layout().Cross = UIAlign::Center;

    m_titleLabel = Ctx().CreateIn<Label>(m_titleBar, m_title);
    m_titleLabel->SetName("Title");
    m_titleLabel->SetAlign(UITextAlign::Left, UITextVAlign::Center);

    UIElement* spacer = Ctx().CreateIn<UIElement>(m_titleBar);
    spacer->SetName("Spacer");
    spacer->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;

    m_close = Ctx().CreateIn<Button>(m_titleBar, std::string("×"), std::string());
    m_close->SetName("Close");
    m_close->SetSize({22.0f, 22.0f});
    m_close->SetStyle("WindowClose");
    m_close->OnClick([this] { Close(); });

    // --- Тело ---
    m_body = Ctx().Create<UIElement>();
    Add(m_body);
    m_body->SetName("Body");
    UITransform& bt = m_body->Ensure<UITransform>();
    bt.WidthMode = UISizeMode::Stretch;
    bt.HeightMode = UISizeMode::Stretch;
    m_body->Vertical(6.0f)->Padding(UIEdges::Uniform(10.0f));

    // --- Уголок изменения размера ---
    //
    // Отдельным узлом поверх тела и вне раскладки: он обязан лежать в правом
    // нижнем углу окна, а не «после последнего ребёнка».
    m_grip = Ctx().CreateIn<UIElement>(this);
    m_grip->SetName("Grip");
    UITransform& gt = m_grip->Ensure<UITransform>();
    gt.IgnoreLayout = true;
    gt.AnchorMin = gt.AnchorMax = {1.0f, 1.0f};
    gt.Pivot = {1.0f, 1.0f};
    gt.Size = {16.0f, 16.0f};
    UIInteraction& gi = m_grip->Ensure<UIInteraction>();
    gi.Draggable = true;
    gi.Cursor = "resize-nwse";

    // Само окно ловит мышь: щелчок в любое место обязан поднимать его вперёд,
    // и он же не должен проходить сквозь окно на то, что под ним.
    UIInteraction& ia = Ensure<UIInteraction>();
    ia.Hit = UIHitShape::RoundedRect;
    ia.BlockRaycast = true;

    InstallDrag();
    InstallResize();

    On(UIEventType::PointerDown, [this](UIEvent&) { Raise(); });
}

void Window::InstallDrag() {
    UIInteraction& ia = m_titleBar->Ensure<UIInteraction>();
    ia.Draggable = true;
    ia.Cursor = "move";

    m_titleBar->On(UIEventType::DragStart, [this](UIEvent&) {
        // Запоминаем положение НА НАЧАЛО перетаскивания: Delta приходит от
        // точки нажатия, и складывать её с текущим смещением каждый кадр
        // значит ускоряться в геометрической прогрессии.
        m_dragOrigin = Position();
        Raise();
    });
    m_titleBar->On(UIEventType::Drag, [this](UIEvent& e) {
        if (!m_movable) return;
        // Delta — в экранных пикселях, а Offset — в логических единицах холста.
        // Без деления окно уезжает быстрее курсора ровно во столько раз, во
        // сколько масштабирован холст.
        const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
        SetPosition(m_dragOrigin + e.Delta / scale);
    });
}

void Window::InstallResize() {
    m_grip->On(UIEventType::DragStart, [this](UIEvent&) { m_sizeOrigin = Size(); });
    m_grip->On(UIEventType::Drag, [this](UIEvent& e) {
        if (!m_resizable) return;
        const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
        glm::vec2 size = m_sizeOrigin + e.Delta / scale;
        // Минимум — не косметика: окно, стянутое в точку, невозможно вернуть
        // обратно, потому что тянуть больше не за что.
        size.x = std::max(size.x, m_minSize.x);
        size.y = std::max(size.y, m_minSize.y);
        SetSize(size);
    });
}

void Window::Update(float dt) {
    (void)dt;
    if (!m_centerRequested) return;
    // Центрируем ПОСЛЕ того, как раскладка посчитала родителя: до первого кадра
    // его размеров не существует.
    UIElement* parent = Parent();
    if (!parent) return;
    const UIRect pr = parent->Bounds();
    if (pr.w <= 0.0f || pr.h <= 0.0f) return;
    const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
    const glm::vec2 size = Size();
    SetPosition({(pr.w / scale - size.x) * 0.5f, (pr.h / scale - size.y) * 0.5f});
    m_centerRequested = false;
}

Window* Window::SetTitle(const std::string& title) {
    m_title = title;
    if (m_titleLabel) m_titleLabel->SetText(title);
    SetName(title);
    return this;
}

const std::string& Window::Title() const { return m_title; }

Window* Window::SetMovable(bool on) {
    m_movable = on;
    if (m_titleBar) m_titleBar->Ensure<UIInteraction>().Draggable = on;
    return this;
}

Window* Window::SetResizable(bool on) {
    m_resizable = on;
    if (m_grip) m_grip->SetVisible(on);
    return this;
}

Window* Window::SetClosable(bool on) {
    if (m_close) m_close->SetVisible(on);
    return this;
}

Window* Window::SetMinSize(glm::vec2 size) {
    m_minSize = size;
    return this;
}

Window* Window::Center() {
    m_centerRequested = true;
    return this;
}

Window* Window::OnClose(std::function<void()> fn) {
    m_onClose = std::move(fn);
    return this;
}

void Window::Close() {
    SetVisible(false);
    if (m_onClose) m_onClose();
}

void Window::Open() {
    SetVisible(true);
    Raise();
}

void Window::Raise() {
    UIElement* parent = Parent();
    if (!parent) return;
    UINode* self = Node();
    if (!self) return;

    // Верх среди ЧУЖИХ, а не среди всех. Считая себя, окно с порядком 0 среди
    // таких же нулей решало бы, что оно уже наверху, — и не поднималось никогда.
    int top = 0;
    bool alone = true;
    for (UIElement* sibling : parent->Children()) {
        if (sibling == this) continue;
        const UINode* n = sibling->Node();
        if (!n) continue;
        alone = false;
        top = std::max(top, n->Order);
    }
    if (alone || self->Order > top) return;
    self->Order = top + 1;
    // Порядок — это порядок ОТРИСОВКИ и попаданий: без пометки поднятое окно
    // осталось бы под соседом до следующей случайной перестройки.
    Dirty(UIDirty_Hierarchy);
}

// ============================================================================
//  Dialog
// ============================================================================

Dialog::Dialog(std::string title) : Window(std::move(title)) {}

void Dialog::OnAttach() {
    Window::OnAttach();
    SetName("Dialog");
    SetResizable(false);
    SetSize({420.0f, 200.0f});

    // Затемнение — БРАТ окна в слое Modal, а не его ребёнок: ребёнок был бы
    // обрезан рамкой окна и затемнял бы само окно, а не всё под ним.
    m_dim = Ctx().CreateIn<Panel>(Ctx().Layer(UILayer::Modal));
    m_dim->SetName("Dim");
    m_dim->SetStretch(true, true);
    static_cast<Panel*>(m_dim)->SetColor(UIColor(0.0f, 0.0f, 0.0f, 0.45f));
    // Вот ради этой строки затемнение и существует: оно ловит мышь и потому
    // ввод под диалогом не проходит.
    UIInteraction& ia = m_dim->Ensure<UIInteraction>();
    ia.BlockRaycast = true;
    ia.Hit = UIHitShape::Rect;
    if (UINode* n = m_dim->Node()) n->Order = -1; // под окном того же слоя

    Ctx().Layer(UILayer::Modal)->Add(this);
    Center();

    // --- Ряд кнопок ---
    m_buttons = Ctx().CreateIn<UIElement>(Body());
    m_buttons->SetName("Buttons");
    UITransform& t = m_buttons->Ensure<UITransform>();
    t.WidthMode = UISizeMode::Stretch;
    t.HeightMode = UISizeMode::Fixed;
    t.Size.y = 32.0f;
    m_buttons->Horizontal(8.0f)->Padding(UIEdges::Uniform(0.0f));
    m_buttons->Layout().Main = UIAlign::End;

    OnClose([this] {
        // Диалог уходит вместе со своим затемнением. Забытое затемнение — это
        // прозрачная плёнка поверх редактора, которая молча ест все щелчки.
        if (m_dim) Ctx().Destroy(m_dim);
        m_dim = nullptr;
        Ctx().Destroy(this);
    });
}

Button* Dialog::AddButton(const std::string& text, std::function<void()> onPress) {
    Button* b = Ctx().CreateIn<Button>(m_buttons, text, std::string());
    b->SetSize({110.0f, 30.0f});
    b->OnClick([this, onPress = std::move(onPress)] {
        if (onPress) onPress();
        Close();
    });
    return b;
}

Dialog* Dialog::Confirm(UIContext& ui, const std::string& title, const std::string& text,
                        const std::string& okText, std::function<void()> onOk) {
    Dialog* d = ui.Create<Dialog>(title);
    Label* message = ui.CreateIn<Label>(d->Body(), text);
    message->SetWrap(true);
    message->Ensure<UITransform>().HeightMode = UISizeMode::Stretch;
    // Порядок кнопок один и тот же во всём редакторе: отказ слева, действие
    // справа. Диалоги с переставленными кнопками — способ получить случайное
    // «да» вместо «нет».
    d->AddButton("Отмена", nullptr);
    d->AddButton(okText, std::move(onOk))->SetStyle("ButtonPrimary");
    return d;
}

// ============================================================================
//  Popup
// ============================================================================

void Popup::OnAttach() {
    Panel::OnAttach();
    SetName("Popup");
    SetStyle("Popup");
    SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    SetSize({180.0f, 40.0f});
    Vertical(2.0f)->Padding(UIEdges::Uniform(4.0f));
    // По содержимому: меню из трёх пунктов и меню из тридцати — разной высоты,
    // и задавать её числом значит однажды обрезать половину пунктов.
    FitContent(false, true);
    Ensure<UIInteraction>().BlockRaycast = true;
    SetVisible(false);
}

void Popup::Update(float) {
    if (!m_needsPlacement) return;
    // Размер меню известен только после раскладки: до неё «влезает ли оно в
    // кадр» — вопрос без ответа.
    const UIRect r = Bounds();
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    PlaceInside(m_pending);
    m_needsPlacement = false;
}

void Popup::PlaceInside(glm::vec2 topLeft) {
    const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
    const glm::vec2 screen = Ctx().Screen();
    const UIRect r = Bounds();
    glm::vec2 p = topLeft;
    // Не влезает справа/снизу — сдвигаем внутрь. Именно сдвигаем, а не
    // отражаем: отражённое меню появляется не там, куда нажали, и человек
    // теряет его из виду.
    if (p.x + r.w > screen.x) p.x = std::max(0.0f, screen.x - r.w);
    if (p.y + r.h > screen.y) p.y = std::max(0.0f, screen.y - r.h);
    SetPosition(p / scale);
}

void Popup::OpenAt(glm::vec2 screenPoint) {
    Ctx().Layer(UILayer::Popup)->Add(this);
    SetVisible(true);
    m_pending = screenPoint;
    m_needsPlacement = true;
    // Ставим сразу по грубой оценке, чтобы меню не мигало в углу первый кадр;
    // Update поправит, когда узнает размер.
    const float scale = std::max(0.0001f, Ctx().Runtime().Layout().CanvasScale());
    SetPosition(screenPoint / scale);
    Ctx().RegisterOpenPopup(this);
}

void Popup::OpenUnder(UIElement* anchor) {
    if (!anchor) return;
    const UIRect r = anchor->Bounds();
    OpenAt({r.x, r.y + r.h + 2.0f});
}

void Popup::Close() {
    if (!IsVisible()) return;
    SetVisible(false);
    Ctx().UnregisterOpenPopup(this);
    if (m_onClosed) m_onClosed();
}

Button* Popup::AddItem(const std::string& text, std::function<void()> onPress) {
    Button* b = Ctx().CreateIn<Button>(this, text, std::string());
    b->SetStyle("MenuItem");
    b->SetHeight(24.0f);
    b->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    if (b->Caption()) b->Caption()->SetAlign(UITextAlign::Left, UITextVAlign::Center);
    b->OnClick([this, onPress = std::move(onPress)] {
        // Сначала закрыть, потом действие: действие может открыть другое меню,
        // и закрытие после него закрыло бы уже новое.
        Close();
        if (onPress) onPress();
    });
    return b;
}

void Popup::AddSeparator() {
    Panel* line = Ctx().CreateIn<Panel>(this);
    line->SetName("Separator");
    line->SetHeight(1.0f);
    line->Ensure<UITransform>().WidthMode = UISizeMode::Stretch;
    line->SetColor(UIColor(1.0f, 1.0f, 1.0f, 0.10f));
}

Popup* Popup::OnClosed(std::function<void()> fn) {
    m_onClosed = std::move(fn);
    return this;
}

} // namespace sage::ui::sui
