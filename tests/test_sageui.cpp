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
