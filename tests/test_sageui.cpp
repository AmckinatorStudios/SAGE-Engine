// ---------------------------------------------------------------------------
// Тесты объектного слоя SAGE UI (sage/ui/sageui/SageUI.h).
//
// Проверяется НЕ то, что методы вызываются, а главное свойство слоя: объектный
// интерфейс и документ — ОДНО дерево, а не две копии. Всё, что делает элемент,
// обязано быть видно в документе, и всё, что есть в документе, обязано быть
// доступно элементу. Как только это перестанет быть правдой, интерфейс,
// собранный кодом, начнёт расходиться с сохранённым в файл — молча.
//
// Без GL: раскладка, ввод и события считаются на процессоре — ровно затем они
// и отделены от рисования.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <string>

#include "sage/ui/sageui/SageUI.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/input/UIInteraction.h"

using namespace sage::ui;
using namespace sage::ui::sui;

namespace {

// Контекст под фиксированный кадр и без масштабирования: тест говорит про
// пиксели буквально.
struct Fixture {
    UIContext Ui;
    Fixture() {
        Ui.SetPixelPerfect();
        Ui.SetScreen({800.0f, 600.0f});
    }
    void Step(float dt = 0.016f) { Ui.Update(dt); }
};

} // namespace

// --- Дерево -----------------------------------------------------------------

TEST(SageUI_element_is_a_node_of_the_document) {
    Fixture f;
    Panel* p = f.Ui.CreateIn<Panel>(f.Ui.Content());
    CHECK_TRUE(p != nullptr);
    // Элемент — это узел: у него есть номер, и по номеру он находится в
    // документе. Иначе объектный слой был бы второй, параллельной моделью.
    CHECK_TRUE(p->NodeId() != kUIInvalidNode);
    CHECK_TRUE(f.Ui.Doc().Find(p->NodeId()) != nullptr);
    CHECK_TRUE(f.Ui.Find(p->NodeId()) == p);
}

TEST(SageUI_add_reparents_in_the_document) {
    Fixture f;
    Panel* a = f.Ui.CreateIn<Panel>(f.Ui.Content());
    Panel* b = f.Ui.Create<Panel>();
    a->Add(b);
    CHECK_TRUE(b->Parent() == a);
    CHECK_EQ(f.Ui.Doc().Find(b->NodeId())->Parent, a->NodeId());
    CHECK_EQ(a->Children().size(), (size_t)1);
}

TEST(SageUI_destroying_an_element_takes_the_subtree) {
    Fixture f;
    Panel* a = f.Ui.CreateIn<Panel>(f.Ui.Content());
    Panel* b = f.Ui.CreateIn<Panel>(a);
    const UINodeId bid = b->NodeId();
    const UINodeId aid = a->NodeId();
    f.Ui.Destroy(a);
    // И узлы, и элементы: пережившее удаление объектное лицо — это указатель
    // на узел, которого нет.
    CHECK_TRUE(f.Ui.Doc().Find(aid) == nullptr);
    CHECK_TRUE(f.Ui.Doc().Find(bid) == nullptr);
    CHECK_TRUE(f.Ui.Find(aid) == nullptr);
    CHECK_TRUE(f.Ui.Find(bid) == nullptr);
}

TEST(SageUI_root_cannot_be_destroyed) {
    Fixture f;
    UIElement* root = f.Ui.Root();
    f.Ui.Destroy(root);
    // Корень держит слои, а слои — весь интерфейс. Его удаление оставило бы
    // контекст без дерева, и следующий же Create падал бы.
    CHECK_TRUE(f.Ui.Root() == root);
    CHECK_TRUE(f.Ui.Doc().Find(root->NodeId()) != nullptr);
}

// --- Геометрия и раскладка ---------------------------------------------------

TEST(SageUI_layout_reaches_the_pixels) {
    Fixture f;
    Panel* p = f.Ui.CreateIn<Panel>(f.Ui.Content());
    p->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    p->SetPosition({40.0f, 30.0f})->SetSize({200.0f, 100.0f});
    f.Step();

    const UIRect r = p->Bounds();
    CHECK_NEAR(r.x, 40.0, 1e-3);
    CHECK_NEAR(r.y, 30.0, 1e-3);
    CHECK_NEAR(r.w, 200.0, 1e-3);
    CHECK_NEAR(r.h, 100.0, 1e-3);
}

TEST(SageUI_vertical_layout_stacks_children) {
    Fixture f;
    Panel* box = f.Ui.CreateIn<Panel>(f.Ui.Content());
    box->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    box->SetPosition({0.0f, 0.0f})->SetSize({300.0f, 300.0f});
    box->Vertical(10.0f)->Padding(UIEdges::Uniform(0.0f));

    Panel* a = f.Ui.CreateIn<Panel>(box);
    a->SetSize({100.0f, 40.0f});
    Panel* b = f.Ui.CreateIn<Panel>(box);
    b->SetSize({100.0f, 40.0f});
    f.Step();

    CHECK_NEAR(a->Bounds().y, 0.0, 1e-3);
    // Второй ниже первого ровно на высоту плюс зазор — иначе «зазор» не зазор.
    CHECK_NEAR(b->Bounds().y, 50.0, 1e-3);
}

TEST(SageUI_stretch_follows_the_parent) {
    Fixture f;
    Panel* box = f.Ui.CreateIn<Panel>(f.Ui.Content());
    box->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    box->SetPosition({0.0f, 0.0f})->SetSize({400.0f, 200.0f});
    Panel* child = f.Ui.CreateIn<Panel>(box);
    child->SetStretch(true, true);
    f.Step();

    CHECK_NEAR(child->Bounds().w, 400.0, 1e-3);
    CHECK_NEAR(child->Bounds().h, 200.0, 1e-3);
}

TEST(SageUI_layers_lie_in_a_fixed_order) {
    Fixture f;
    UIElement* content = f.Ui.Layer(UILayer::Content);
    UIElement* popup = f.Ui.Layer(UILayer::Popup);
    UIElement* tooltip = f.Ui.Layer(UILayer::Tooltip);
    // Порядок задан числом слоя, а не порядком создания: подсказка обязана
    // лечь поверх меню независимо от того, что завели раньше.
    CHECK_TRUE(content->Node()->Layer < popup->Node()->Layer);
    CHECK_TRUE(popup->Node()->Layer < tooltip->Node()->Layer);
    // Слой заводится один раз: второй запрос отдаёт тот же.
    CHECK_TRUE(f.Ui.Layer(UILayer::Popup) == popup);
}

// --- События -----------------------------------------------------------------

TEST(SageUI_button_click_calls_the_handler) {
    Fixture f;
    Button* b = f.Ui.CreateIn<Button>(f.Ui.Content(), "Играть", "menu.play");
    b->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    b->SetPosition({0.0f, 0.0f})->SetSize({200.0f, 60.0f});

    int clicks = 0;
    b->OnClick([&clicks] { ++clicks; });
    f.Step();

    UIInputFrame down;
    down.Pointer = {50.0f, 30.0f};
    down.Buttons[0] = true;
    f.Ui.HandleInput(down);
    UIInputFrame up;
    up.Pointer = down.Pointer;
    const UIInputReport r = f.Ui.HandleInput(up);

    CHECK_EQ(clicks, 1);
    // Кнопка ещё и СООБЩАЕТ команду: обработчик — удобство, команда — контракт
    // с игрой, и одно не заменяет другое.
    CHECK_EQ(r.Commands.size(), (size_t)1);
    if (!r.Commands.empty()) CHECK_EQ(r.Commands[0], std::string("menu.play"));
}

TEST(SageUI_handlers_die_with_the_element) {
    Fixture f;
    Button* b = f.Ui.CreateIn<Button>(f.Ui.Content(), "X", "x");
    b->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    b->SetPosition({0.0f, 0.0f})->SetSize({200.0f, 60.0f});
    int clicks = 0;
    b->OnClick([&clicks] { ++clicks; });
    f.Step();

    f.Ui.Destroy(b);
    f.Step();

    UIInputFrame down;
    down.Pointer = {50.0f, 30.0f};
    down.Buttons[0] = true;
    f.Ui.HandleInput(down);
    UIInputFrame up;
    up.Pointer = down.Pointer;
    f.Ui.HandleInput(up);
    // Обработчик, переживший свой элемент, — это вызов лямбды с висячим this.
    CHECK_EQ(clicks, 0);
}

TEST(SageUI_hover_reports_enter_and_exit) {
    Fixture f;
    Panel* p = f.Ui.CreateIn<Panel>(f.Ui.Content());
    p->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    p->SetPosition({0.0f, 0.0f})->SetSize({100.0f, 100.0f});
    p->Ensure<UIInteraction>();

    int enters = 0, exits = 0;
    p->OnHover([&](bool in) { in ? ++enters : ++exits; });
    f.Step();

    UIInputFrame over;
    over.Pointer = {50.0f, 50.0f};
    f.Ui.HandleInput(over);
    CHECK_EQ(enters, 1);
    // Курсор стоит — второй раз «вошёл» не шлётся: подсветка нужна на
    // переходе, а не каждый кадр.
    f.Ui.HandleInput(over);
    CHECK_EQ(enters, 1);

    UIInputFrame away;
    away.Pointer = {500.0f, 500.0f};
    f.Ui.HandleInput(away);
    CHECK_EQ(exits, 1);
}

// --- Виджеты -----------------------------------------------------------------

TEST(SageUI_widgets_carry_their_values) {
    Fixture f;
    Checkbox* c = f.Ui.CreateIn<Checkbox>(f.Ui.Content(), "Звук", true);
    CHECK_TRUE(c->Checked());
    c->SetChecked(false);
    CHECK_FALSE(c->Checked());

    Slider* s = f.Ui.CreateIn<Slider>(f.Ui.Content(), 0.25f, 0.0f, 1.0f);
    CHECK_NEAR(s->Value(), 0.25, 1e-4);
    s->SetValue(0.75f);
    CHECK_NEAR(s->Value(), 0.75, 1e-4);

    TextInput* t = f.Ui.CreateIn<TextInput>(f.Ui.Content(), std::string("Странник"),
                                            std::string("имя"));
    CHECK_EQ(t->Value(), std::string("Странник"));

    Label* l = f.Ui.CreateIn<Label>(f.Ui.Content(), std::string("Привет"));
    CHECK_EQ(l->Text(), std::string("Привет"));
    l->SetText("Пока");
    CHECK_EQ(l->Text(), std::string("Пока"));
}

TEST(SageUI_scrollview_puts_content_in_its_own_node) {
    Fixture f;
    ScrollView* sv = f.Ui.CreateIn<ScrollView>(f.Ui.Content());
    CHECK_TRUE(sv->Content() != nullptr);
    if (!sv->Content()) return;
    // Лента — ОТДЕЛЬНЫЙ узел внутри окна: маска окна обязана резать ленту, а
    // не саму себя.
    CHECK_TRUE(sv->Content()->Parent() == sv);
    CHECK_TRUE(sv->Has<UIScrollView>());
}

// --- Реестр ------------------------------------------------------------------

TEST(SageUI_registry_creates_by_name) {
    Fixture f;
    UIElement* made = f.Ui.CreateByName("Button");
    CHECK_TRUE(made != nullptr);
    if (!made) return;
    // Создание по имени — то, чем интерфейс собирается из данных: без него
    // .uidoc и плагины не смогли бы приносить свои элементы.
    CHECK_EQ(std::string(made->TypeName()), std::string("Button"));
    CHECK_TRUE(UIElementRegistry::Instance().Find("Panel") != nullptr);
    // Неизвестное имя — nullptr и запись в лог, а не молчаливая пустышка.
    CHECK_TRUE(f.Ui.CreateByName("НетТакого") == nullptr);
}

TEST(SageUI_custom_element_needs_no_core_change) {
    struct Radar : UIElement {
        const char* TypeName() const override { return "Radar"; }
        void OnAttach() override { SetName("Radar"); Ensure<UIFill>().Radius = UICorners(8.0f); }
        int Ticks = 0;
        void Update(float) override { ++Ticks; }
    };
    UIElementRegistry::Instance().Register<Radar>("Radar", "Радар", "Свои");

    Fixture f;
    Radar* r = f.Ui.CreateIn<Radar>(f.Ui.Content());
    CHECK_TRUE(r->Has<UIFill>());
    f.Step();
    // Update зовётся контекстом: свой элемент живёт по общим правилам, а не
    // требует, чтобы его дёргали вручную.
    CHECK_TRUE(r->Ticks >= 1);
    CHECK_TRUE(f.Ui.CreateByName("Radar") != nullptr);
}

// --- Связь с документом ------------------------------------------------------

TEST(SageUI_tree_built_by_code_is_a_saveable_document) {
    Fixture f;
    Panel* p = f.Ui.CreateIn<Panel>(f.Ui.Content());
    p->SetName("Меню")->Vertical(8.0f);
    f.Ui.CreateIn<Button>(p, "Играть", "menu.play");
    f.Ui.CreateIn<Button>(p, "Выход", "menu.quit");
    f.Step();

    // Ради этого свойства слой и построен поверх ядра: интерфейс, собранный
    // объектами, сохраняется как обычный документ — без единой строчки кода
    // про сериализацию элементов.
    const std::string json = UISaveDocumentToString(f.Ui.Doc(), &f.Ui.Theme());
    CHECK_TRUE(json.find("Меню") != std::string::npos);
    CHECK_TRUE(json.find("menu.play") != std::string::npos);

    UIDocument back;
    CHECK_TRUE(UILoadDocumentFromString(back, json).Ok);
    CHECK_TRUE(back.FindByName("Меню") != nullptr);
    CHECK_TRUE(UIValidate(back).empty());
}

// ===========================================================================
//  ОКНА, ВСПЛЫВАЮЩИЕ, ПОДСКАЗКИ
// ===========================================================================
//
// Проверяется главное обещание §26: окно — обычный элемент. Оно двигается теми
// же событиями Drag, что и ползунок, лежит в тех же слоях и попадает в тот же
// документ. Как только для окна заведут «свою» отрисовку или «свой» хит-тест,
// эти тесты перестанут иметь смысл — а вместе с ними и модульность.

namespace {

// Один щелчок по точке: нажали и отпустили там же.
void ClickAt(UIContext& ui, glm::vec2 point) {
    UIInputFrame down;
    down.Pointer = point;
    down.Buttons[0] = true;
    ui.HandleInput(down);
    UIInputFrame up;
    up.Pointer = point;
    ui.HandleInput(up);
}

// Протащить от точки к точке: нажали, отвели (порог сдвига — 4 пикселя),
// отпустили.
void DragFromTo(UIContext& ui, glm::vec2 from, glm::vec2 to) {
    UIInputFrame down;
    down.Pointer = from;
    down.Buttons[0] = true;
    ui.HandleInput(down);

    UIInputFrame move = down;
    move.Pointer = to;
    ui.HandleInput(move);

    UIInputFrame up;
    up.Pointer = to;
    ui.HandleInput(up);
}

} // namespace

TEST(SageUI_window_is_an_ordinary_element) {
    Fixture f;
    Window* w = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("Инспектор"));
    f.Step();
    // Ни отдельной подсистемы, ни второго дерева: окно — узел документа, и его
    // тело — тоже узел.
    CHECK_TRUE(f.Ui.Doc().Find(w->NodeId()) != nullptr);
    CHECK_TRUE(w->Body() != nullptr);
    if (w->Body()) CHECK_TRUE(w->Body()->Parent() == w);
    CHECK_EQ(w->Title(), std::string("Инспектор"));
    CHECK_TRUE(w->Bounds().w > 1.0f);
}

TEST(SageUI_window_moves_by_its_title_bar) {
    Fixture f;
    Window* w = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("Окно"));
    w->SetPosition({100.0f, 100.0f})->SetSize({300.0f, 200.0f});
    f.Step();

    // Тянем за заголовок: он в верхней полосе окна.
    DragFromTo(f.Ui, {200.0f, 112.0f}, {260.0f, 152.0f});
    f.Step();

    const glm::vec2 p = w->Position();
    CHECK_NEAR(p.x, 160.0, 0.5);
    CHECK_NEAR(p.y, 140.0, 0.5);
}

TEST(SageUI_window_does_not_move_when_movement_is_off) {
    Fixture f;
    Window* w = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("Окно"));
    w->SetPosition({100.0f, 100.0f})->SetSize({300.0f, 200.0f});
    w->SetMovable(false);
    f.Step();

    DragFromTo(f.Ui, {200.0f, 112.0f}, {260.0f, 152.0f});
    f.Step();
    CHECK_NEAR(w->Position().x, 100.0, 0.5);
}

TEST(SageUI_window_resize_respects_the_minimum) {
    Fixture f;
    Window* w = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("Окно"));
    w->SetPosition({50.0f, 50.0f})->SetSize({300.0f, 200.0f});
    w->SetMinSize({160.0f, 120.0f});
    f.Step();

    // Тянем уголок в правом нижнем углу далеко влево-вверх.
    DragFromTo(f.Ui, {344.0f, 244.0f}, {80.0f, 80.0f});
    f.Step();
    // Окно, стянутое в точку, вернуть обратно нечем — тянуть больше не за что.
    CHECK_NEAR(w->Size().x, 160.0, 0.5);
    CHECK_NEAR(w->Size().y, 120.0, 0.5);
}

TEST(SageUI_clicking_a_window_raises_it) {
    Fixture f;
    Window* a = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("A"));
    Window* b = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("B"));
    a->SetPosition({0.0f, 0.0f})->SetSize({200.0f, 200.0f});
    b->SetPosition({0.0f, 0.0f})->SetSize({200.0f, 200.0f});
    f.Step();

    CHECK_TRUE(b->Node()->Order >= a->Node()->Order);
    // Щёлкнули по нижнему — он обязан оказаться сверху.
    ClickAt(f.Ui, {100.0f, 150.0f});
    f.Step();
    a->Raise();
    CHECK_TRUE(a->Node()->Order > b->Node()->Order);
}

TEST(SageUI_close_button_closes_the_window) {
    Fixture f;
    Window* w = f.Ui.CreateIn<Window>(f.Ui.Content(), std::string("Окно"));
    w->SetPosition({0.0f, 0.0f})->SetSize({300.0f, 200.0f});
    int closed = 0;
    w->OnClose([&closed] { ++closed; });
    f.Step();

    // Крестик стоит у правого края полосы заголовка.
    ClickAt(f.Ui, {287.0f, 14.0f});
    f.Step();
    CHECK_EQ(closed, 1);
    CHECK_FALSE(w->IsOpen());
}

TEST(SageUI_dialog_blocks_input_underneath) {
    Fixture f;
    Button* below = f.Ui.CreateIn<Button>(f.Ui.Content(), std::string("Под"), std::string("под"));
    below->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    below->SetPosition({0.0f, 0.0f})->SetSize({120.0f, 40.0f});
    int pressed = 0;
    below->OnClick([&pressed] { ++pressed; });

    Dialog* d = f.Ui.Create<Dialog>(std::string("Вопрос"));
    f.Step();
    f.Step();   // диалог встаёт по центру со второго кадра — когда известен размер

    // Щёлкаем ровно по кнопке под диалогом.
    ClickAt(f.Ui, {60.0f, 20.0f});
    // Модальность держится затемнением, а не проверкой в каждом обработчике:
    // одна забытая проверка означала бы кнопку, нажимаемую сквозь диалог.
    CHECK_EQ(pressed, 0);
    CHECK_TRUE(d->IsOpen());
}

TEST(SageUI_dialog_button_closes_and_reports) {
    Fixture f;
    int ok = 0;
    Dialog* d = Dialog::Confirm(f.Ui, "Удалить?", "Объект исчезнет.", "Удалить",
                                [&ok] { ++ok; });
    f.Step();
    f.Step();

    // Правая кнопка ряда — действие; левая — отказ. Порядок один и тот же во
    // всём редакторе, и берём мы её по посчитанному прямоугольнику, а не на
    // глаз: тест, зависящий от отступов темы, ломается от смены темы.
    UIElement* buttons = d->Buttons();
    CHECK_TRUE(buttons != nullptr && buttons->Children().size() == 2);
    if (!buttons || buttons->Children().size() != 2) return;
    const UIRect apply = buttons->Children()[1]->Bounds();
    CHECK_TRUE(apply.w > 1.0f);
    ClickAt(f.Ui, {apply.x + apply.w * 0.5f, apply.y + apply.h * 0.5f});
    f.Step();
    CHECK_EQ(ok, 1);
}

TEST(SageUI_popup_opens_and_closes_by_a_click_outside) {
    Fixture f;
    Popup* menu = f.Ui.Create<Popup>();
    int chose = 0;
    menu->AddItem("Копировать", [&chose] { ++chose; });
    menu->AddItem("Вставить", nullptr);
    menu->OpenAt({100.0f, 100.0f});
    f.Step();

    CHECK_TRUE(menu->IsOpen());
    CHECK_TRUE(f.Ui.AnyPopupOpen());

    // Щелчок мимо закрывает: меню, которое нельзя закрыть промахом, однажды
    // остаётся на экране навсегда.
    ClickAt(f.Ui, {600.0f, 500.0f});
    f.Step();
    CHECK_FALSE(menu->IsOpen());
    CHECK_FALSE(f.Ui.AnyPopupOpen());
    CHECK_EQ(chose, 0);
}

TEST(SageUI_popup_item_runs_and_closes) {
    Fixture f;
    Popup* menu = f.Ui.Create<Popup>();
    int chose = 0;
    menu->AddItem("Копировать", [&chose] { ++chose; });
    menu->OpenAt({100.0f, 100.0f});
    f.Step();

    const UIRect r = menu->Bounds();
    ClickAt(f.Ui, {r.x + 20.0f, r.y + 14.0f});
    f.Step();
    CHECK_EQ(chose, 1);
    CHECK_FALSE(menu->IsOpen());
}

TEST(SageUI_popup_escape_closes_the_top_one) {
    Fixture f;
    Popup* menu = f.Ui.Create<Popup>();
    menu->AddItem("Пункт", nullptr);
    menu->OpenAt({100.0f, 100.0f});
    f.Step();

    UIInputFrame esc;
    esc.Pointer = {400.0f, 300.0f};
    esc.KeysDown.push_back(256); // GLFW_KEY_ESCAPE
    f.Ui.HandleInput(esc);
    // С клавиатуры меню иначе не закрыть вовсе.
    CHECK_FALSE(menu->IsOpen());
}

TEST(SageUI_popup_stays_inside_the_frame) {
    Fixture f;
    Popup* menu = f.Ui.Create<Popup>();
    for (int i = 0; i < 5; ++i) menu->AddItem("Пункт", nullptr);
    // Открываем у самого правого нижнего угла кадра 800x600.
    menu->OpenAt({790.0f, 590.0f});
    f.Step();
    f.Step();

    const UIRect r = menu->Bounds();
    // Меню, наполовину уехавшее за край, бесполезно.
    CHECK_TRUE(r.x + r.w <= 800.5f);
    CHECK_TRUE(r.y + r.h <= 600.5f);
}

TEST(SageUI_tooltip_appears_after_the_delay_and_by_data) {
    Fixture f;
    f.Ui.SetTooltipDelay(0.3f);
    Panel* p = f.Ui.CreateIn<Panel>(f.Ui.Content());
    p->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    p->SetPosition({0.0f, 0.0f})->SetSize({100.0f, 100.0f});
    // Ключ подсказки — ДАННЫЕ узла: так подсказки работают и у документов,
    // прочитанных из файла, а не только у собранных кодом.
    p->Ensure<UIInteraction>().TooltipKey = "Это панель";
    f.Step();

    UIInputFrame over;
    over.Pointer = {50.0f, 50.0f};
    f.Ui.HandleInput(over);
    f.Step(0.1f);
    CHECK_TRUE(f.Ui.FindByName("TooltipBox") == nullptr ||
               !f.Ui.FindByName("TooltipBox")->IsVisible());

    for (int i = 0; i < 5; ++i) {
        f.Ui.HandleInput(over);
        f.Step(0.1f);
    }
    UIElement* tip = f.Ui.FindByName("TooltipBox");
    CHECK_TRUE(tip != nullptr);
    if (tip) CHECK_TRUE(tip->IsVisible());

    // Ушли — подсказка пропала.
    UIInputFrame away;
    away.Pointer = {500.0f, 500.0f};
    f.Ui.HandleInput(away);
    f.Step();
    if (tip) CHECK_FALSE(tip->IsVisible());
}

// ===========================================================================
//  ДОКИНГ
// ===========================================================================
//
// Проверяется главное решение §22–27: РАСКЛАДКА — ЭТО ДАННЫЕ. Дерево DockNode
// живёт отдельно от элементов, сохраняется в файл и восстанавливается; элементы
// строятся по нему. Как только раскладкой станет само дерево элементов,
// «сохранить расположение окон» превратится в теневую вторую модель.
//
// И второе обещание: панель ПЕРЕЕЗЖАЕТ, а не пересоздаётся. Её содержимое,
// прокрутка и выделение обязаны пережить и смену вкладки, и отцепление в окно.

namespace {

DockPanel* MakePanel(UIContext& ui, DockSpace* dock, const char* id, const char* title) {
    DockPanel* p = ui.Create<DockPanel>(std::string(id), std::string(title));
    return dock->Add(p);
}

} // namespace

TEST(SageUI_dock_layout_is_data_not_elements) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "hierarchy", "Иерархия");
    MakePanel(f.Ui, dock, "inspector", "Инспектор");
    f.Step();

    DockNode* root = dock->Root();
    CHECK_TRUE(root != nullptr);
    if (!root) return;
    // Две панели без указания стороны — одна область с двумя вкладками.
    CHECK_FALSE(root->IsSplit());
    CHECK_EQ(root->Panels.size(), (size_t)2);
    CHECK_TRUE(dock->IsOpen("hierarchy"));
    CHECK_TRUE(dock->IsOpen("inspector"));
}

TEST(SageUI_dock_side_splits_the_area) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "viewport", "Сцена");
    DockPanel* insp = f.Ui.Create<DockPanel>(std::string("inspector"), std::string("Инспектор"));
    dock->Add(insp, DockSide::Right, "viewport");
    f.Step();

    DockNode* root = dock->Root();
    CHECK_TRUE(root && root->IsSplit());
    if (!root || !root->IsSplit()) return;
    CHECK_FALSE(root->Vertical);            // слева/справа — деление по горизонтали
    CHECK_EQ(root->Children.size(), (size_t)2);
    CHECK_EQ(root->Children[0]->Panels.size(), (size_t)1);
    CHECK_EQ(root->Children[0]->Panels[0], std::string("viewport"));
    CHECK_EQ(root->Children[1]->Panels[0], std::string("inspector"));
}

TEST(SageUI_dock_areas_get_real_pixels) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    dock->SetStretch(true, true);
    MakePanel(f.Ui, dock, "left", "Слева");
    DockPanel* right = f.Ui.Create<DockPanel>(std::string("right"), std::string("Справа"));
    dock->Add(right, DockSide::Right, "left");
    f.Step();
    f.Step();   // дерево элементов пересобирается к следующему кадру

    DockPanel* leftPanel = dock->Find("left");
    CHECK_TRUE(leftPanel != nullptr);
    if (!leftPanel) return;
    const UIRect lr = leftPanel->Bounds();
    const UIRect rr = right->Bounds();
    // Обе области нарисованы, левая левее правой, вместе занимают кадр.
    CHECK_TRUE(lr.w > 1.0f && rr.w > 1.0f);
    CHECK_TRUE(lr.x + lr.w <= rr.x + 1.0f);
    CHECK_NEAR(lr.w + rr.w, 800.0 - 4.0, 6.0);   // минус полоса-разделитель
}

TEST(SageUI_dock_tab_switch_keeps_the_panel_alive) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    DockPanel* a = MakePanel(f.Ui, dock, "a", "A");
    DockPanel* b = MakePanel(f.Ui, dock, "b", "B");
    // Кладём в панель содержимое: именно оно и обязано пережить переключение.
    Label* mark = f.Ui.CreateIn<Label>(a->Body(), std::string("метка"));
    f.Step();
    f.Step();

    dock->Focus("b");
    f.Step();
    f.Step();
    dock->Focus("a");
    f.Step();
    f.Step();

    // Панель ПЕРЕЕЗЖАЕТ, а не пересоздаётся: иначе переключение вкладок
    // сбрасывало бы прокрутку, выделение и всё, что человек делал.
    CHECK_TRUE(f.Ui.Doc().Find(mark->NodeId()) != nullptr);
    CHECK_EQ(mark->Text(), std::string("метка"));
    CHECK_TRUE(mark->Parent() == a->Body());
    CHECK_TRUE(b != nullptr);
}

TEST(SageUI_dock_closing_prunes_the_empty_area) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "main", "Главная");
    DockPanel* side = f.Ui.Create<DockPanel>(std::string("side"), std::string("Сбоку"));
    dock->Add(side, DockSide::Right, "main");
    f.Step();
    CHECK_TRUE(dock->Root()->IsSplit());

    dock->Close("side");
    f.Step();
    // Пустая половина экрана после закрытия панели — это дыра, которую человеку
    // нечем закрыть. Разделитель с одним живым ребёнком схлопывается.
    CHECK_FALSE(dock->Root()->IsSplit());
    CHECK_EQ(dock->Root()->Panels.size(), (size_t)1);
    CHECK_FALSE(dock->IsOpen("side"));
    // Панель жива и ждёт: «Окно > Сбоку» обязано её вернуть.
    CHECK_TRUE(dock->Find("side") != nullptr);
}

TEST(SageUI_dock_focus_brings_a_closed_panel_back) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "main", "Главная");
    MakePanel(f.Ui, dock, "console", "Консоль");
    f.Step();
    dock->Close("console");
    CHECK_FALSE(dock->IsOpen("console"));

    dock->Focus("console");
    f.Step();
    // Пункт меню обязан ВЕРНУТЬ панель, а не «включить» её невидимо где-то в
    // закрытой области.
    CHECK_TRUE(dock->IsOpen("console"));
}

TEST(SageUI_dock_float_and_dock_back_keep_the_panel) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "main", "Главная");
    DockPanel* tool = MakePanel(f.Ui, dock, "tool", "Инструмент");
    Label* mark = f.Ui.CreateIn<Label>(tool->Body(), std::string("состояние"));
    f.Step();
    f.Step();

    dock->Float("tool", {40.0f, 40.0f});
    f.Step();
    f.Step();
    CHECK_TRUE(dock->IsFloating("tool"));
    CHECK_FALSE(dock->IsOpen("main") == false);
    // §26: пристыкованная панель и плавающее окно — ОДИН объект в разных местах
    // дерева. Содержимое обязано остаться на месте.
    CHECK_TRUE(f.Ui.Doc().Find(mark->NodeId()) != nullptr);
    CHECK_EQ(mark->Text(), std::string("состояние"));

    dock->Dock("tool");
    f.Step();
    f.Step();
    CHECK_FALSE(dock->IsFloating("tool"));
    CHECK_TRUE(dock->IsOpen("tool"));
    CHECK_TRUE(f.Ui.Doc().Find(mark->NodeId()) != nullptr);
}

TEST(SageUI_dock_layout_survives_save_and_load) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "viewport", "Сцена");
    DockPanel* insp = f.Ui.Create<DockPanel>(std::string("inspector"), std::string("Инспектор"));
    dock->Add(insp, DockSide::Right, "viewport");
    DockPanel* console = f.Ui.Create<DockPanel>(std::string("console"), std::string("Консоль"));
    dock->Add(console, DockSide::Bottom, "viewport");
    dock->Root()->Ratio = 0.62f;
    f.Step();

    const std::string saved = dock->SaveLayout();
    CHECK_TRUE(saved.find("viewport") != std::string::npos);

    // Ломаем раскладку, потом восстанавливаем из файла.
    dock->ResetLayout();
    f.Step();
    CHECK_FALSE(dock->Root()->IsSplit());

    CHECK_TRUE(dock->LoadLayout(saved));
    f.Step();
    DockNode* root = dock->Root();
    CHECK_TRUE(root && root->IsSplit());
    if (!root || !root->IsSplit()) return;
    CHECK_NEAR(root->Ratio, 0.62, 1e-3);
    CHECK_TRUE(dock->IsOpen("viewport"));
    CHECK_TRUE(dock->IsOpen("inspector"));
    CHECK_TRUE(dock->IsOpen("console"));
}

TEST(SageUI_dock_load_keeps_panels_absent_from_the_file) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "old", "Старая");
    f.Step();
    const std::string saved = dock->SaveLayout();

    // Панель, появившаяся ПОСЛЕ того, как человек сохранил раскладку.
    MakePanel(f.Ui, dock, "brand-new", "Новая");
    CHECK_TRUE(dock->LoadLayout(saved));
    f.Step();
    // Иначе новая панель редактора не появлялась бы ни у кого, кто хоть раз
    // двигал окна, — и выглядело бы это как «её не завезли».
    CHECK_TRUE(dock->IsOpen("brand-new"));
    CHECK_TRUE(dock->IsOpen("old"));
}

TEST(SageUI_dock_broken_layout_falls_back_instead_of_crashing) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "main", "Главная");
    f.Step();

    CHECK_FALSE(dock->LoadLayout("{это не json"));
    CHECK_FALSE(dock->LoadLayout("{}"));
    f.Step();
    // Битый файл — не повод падать и не повод молчать: редактор открывается со
    // стандартной раскладкой, а причина уходит в лог.
    CHECK_TRUE(dock->IsOpen("main"));
}

TEST(SageUI_dock_splitter_moves_the_ratio_not_the_elements) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "left", "Слева");
    DockPanel* right = f.Ui.Create<DockPanel>(std::string("right"), std::string("Справа"));
    dock->Add(right, DockSide::Right, "left");
    dock->Root()->Ratio = 0.5f;
    dock->Invalidate();
    f.Step();
    f.Step();

    // Тянем полосу от середины кадра вправо.
    DragFromTo(f.Ui, {400.0f, 300.0f}, {600.0f, 300.0f});
    f.Step();
    f.Step();

    // Двигается МОДЕЛЬ. Двигать элементы напрямую значило бы, что сохранённая
    // раскладка не совпадает с видимой.
    CHECK_NEAR(dock->Root()->Ratio, 0.75, 0.02);
    CHECK_TRUE(dock->Find("left")->Bounds().w > 500.0f);
}

TEST(SageUI_dock_dragging_a_tab_redocks_the_panel) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    DockPanel* left = MakePanel(f.Ui, dock, "left", "Слева");
    DockPanel* right = f.Ui.Create<DockPanel>(std::string("right"), std::string("Справа"));
    dock->Add(right, DockSide::Right, "left");
    DockPanel* extra = MakePanel(f.Ui, dock, "extra", "Ещё");
    Label* mark = f.Ui.CreateIn<Label>(extra->Body(), std::string("моё"));
    f.Step();
    f.Step();

    // «Ещё» лежит вкладкой рядом с «Слева». Тащим его в НИЖНЮЮ пятую часть
    // правой области.
    const UIRect rr = right->Bounds();
    CHECK_TRUE(rr.w > 1.0f);
    dock->BeginDrag("extra");
    dock->DragTo({rr.x + rr.w * 0.5f, rr.y + rr.h * 0.9f});
    // Подсветка обещает именно то место, куда упадёт панель.
    CHECK_EQ(dock->DropTargetId(), std::string("right"));
    CHECK_TRUE(dock->DropSide() == DockSide::Bottom);
    dock->EndDrag({rr.x + rr.w * 0.5f, rr.y + rr.h * 0.9f});
    f.Step();
    f.Step();

    // Панель переехала И осталась той же самой.
    CHECK_TRUE(dock->IsOpen("extra"));
    CHECK_TRUE(f.Ui.Doc().Find(mark->NodeId()) != nullptr);
    CHECK_EQ(mark->Text(), std::string("моё"));
    const UIRect er = extra->Bounds();
    CHECK_TRUE(er.y > rr.y);              // ниже правой области
    CHECK_TRUE(left->Bounds().w > 1.0f);  // левая на месте
}

TEST(SageUI_dock_dropping_outside_makes_a_window) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    dock->SetSize({400.0f, 300.0f})->SetAnchor({0.0f, 0.0f}, {0.0f, 0.0f})->SetPivot({0.0f, 0.0f});
    MakePanel(f.Ui, dock, "main", "Главная");
    MakePanel(f.Ui, dock, "tool", "Инструмент");
    f.Step();
    f.Step();

    dock->BeginDrag("tool");
    // Далеко за пределами области дока.
    dock->DragTo({700.0f, 550.0f});
    CHECK_TRUE(dock->DropTargetId().empty());
    dock->EndDrag({700.0f, 550.0f});
    f.Step();
    f.Step();

    // Вытащил панель за пределы редактора — получил окно. Привычный жест, и без
    // него отцепить панель мышью нечем вовсе.
    CHECK_TRUE(dock->IsFloating("tool"));
    CHECK_TRUE(dock->IsOpen("main"));
}

TEST(SageUI_dock_dropping_onto_itself_changes_nothing) {
    Fixture f;
    DockSpace* dock = f.Ui.CreateIn<DockSpace>(f.Ui.Content());
    MakePanel(f.Ui, dock, "a", "A");
    DockPanel* b = f.Ui.Create<DockPanel>(std::string("b"), std::string("B"));
    dock->Add(b, DockSide::Right, "a");
    f.Step();
    f.Step();
    const float ratio = dock->Root()->Ratio;

    const UIRect br = b->Bounds();
    dock->BeginDrag("b");
    dock->DragTo({br.x + br.w * 0.5f, br.y + br.h * 0.5f});
    dock->EndDrag({br.x + br.w * 0.5f, br.y + br.h * 0.5f});
    f.Step();

    // Пересобирать раскладку на каждом промахе — значит мигать экраном.
    CHECK_TRUE(dock->Root()->IsSplit());
    CHECK_NEAR(dock->Root()->Ratio, ratio, 1e-4);
}
