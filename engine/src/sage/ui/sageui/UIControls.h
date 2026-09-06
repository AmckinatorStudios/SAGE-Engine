#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/sageui/UIWidgetsOO.h"
#include "sage/ui/sageui/UIWindow.h"

// ---------------------------------------------------------------------------
// ЭЛЕМЕНТЫ ОБОЛОЧКИ ИНСТРУМЕНТА (§4 ТЗ редактора).
//
// Всё, из чего складывается верх и низ окна редактора и его формы: полоса
// меню, тулбар, разделитель, кнопка-иконка, поле поиска, выпадающий список,
// числовое поле, строка состояния.
//
// ПОЧЕМУ ЭТО В SAGE UI, А НЕ В РЕДАКТОРЕ. Потому что ни один из них не знает
// про редактор ни строчки: тулбар — это ряд кнопок с разделителями, а не
// «Play/Pause/Stop». Как только «редакторские» элементы заводят в редакторе,
// второй инструмент движка (просмотрщик ассетов, отладчик) начинает писать их
// заново — и пишет чуть иначе.
//
// ПЛОТНОСТЬ БЕРЁТСЯ ИЗ ТЕМЫ, А НЕ ИЗ КОДА. Высота строки, поля и кнопки —
// числа Size.Row/Size.Control/Size.IconButton в наборе токенов. Пока каждый
// элемент выбирал высоту сам, соседние панели стояли «в разлинейку».
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

// --- Разделитель -------------------------------------------------------------
//
// Линия в один пиксель. Отдельный класс, а не «панель высотой 1», ровно затем,
// что толщина и цвет разделителя обязаны быть одинаковыми во всём инструменте.
class Separator : public UIElement {
public:
    explicit Separator(bool vertical = false);
    const char* TypeName() const override { return "Separator"; }
    void OnAttach() override;

private:
    bool m_vertical = false;
};

// --- Кнопка-иконка -----------------------------------------------------------
//
// Квадратная кнопка со значком движка и ОБЯЗАТЕЛЬНОЙ подсказкой: кнопка без
// подписи и без подсказки — это ребус, и в инструменте таких быть не должно.
class IconButton : public UIElement {
public:
    IconButton(std::string icon, std::string tooltip);
    IconButton(Icon icon, std::string tooltip);
    const char* TypeName() const override { return "IconButton"; }
    void OnAttach() override;

    IconButton* SetIcon(const std::string& icon);
    IconButton* SetIcon(Icon icon) { return SetIcon(std::string(icons::Icons::Name(icon))); }
    // Нажатое состояние: включённый режим, активный инструмент.
    IconButton* SetActive(bool active);
    bool Active() const { return m_active; }
    IconButton* OnPress(std::function<void()> fn);

private:
    std::string m_icon, m_tooltip;
    bool m_active = false;
    UIElement* m_glyph = nullptr;
};

// --- Тулбар ------------------------------------------------------------------
class Toolbar : public UIElement {
public:
    const char* TypeName() const override { return "Toolbar"; }
    void OnAttach() override;

    IconButton* AddIcon(const std::string& icon, const std::string& tooltip,
                        std::function<void()> onPress = nullptr);
    IconButton* AddIcon(Icon icon, const std::string& tooltip,
                        std::function<void()> onPress = nullptr) {
        return AddIcon(std::string(icons::Icons::Name(icon)), tooltip, std::move(onPress));
    }
    Button* AddButton(const std::string& text, std::function<void()> onPress = nullptr);
    // Разрыв между группами. Группы — это смысл: «проект», «отмена», «игра».
    void AddSeparator();
    // Растяжка: всё, что после неё, уезжает вправо.
    UIElement* AddSpacer();
};

// --- Полоса меню -------------------------------------------------------------
//
// Заголовки открывают всплывающее меню (Popup). Меню строится ОДИН раз при
// добавлении пунктов, а не на каждое открытие: пункты меню редактора не
// меняются от кадра к кадру, а пересборка теряла бы подсветку под курсором.
class MenuBar : public UIElement {
public:
    const char* TypeName() const override { return "MenuBar"; }
    void OnAttach() override;

    // Заголовок меню. Возвращает всплывающее, в которое кладут пункты.
    Popup* AddMenu(const std::string& title);
    // Слева от заголовков: имя приложения, значок.
    Label* SetBrand(const std::string& text);
    // Справа: поиск, тема, что угодно.
    UIElement* Right();

private:
    UIElement* m_left = nullptr;
    UIElement* m_right = nullptr;
    Label* m_brand = nullptr;
};

// --- Поле поиска -------------------------------------------------------------
//
// Не просто поле ввода: значок слева и крестик справа, появляющийся, только
// когда есть что стирать. Пустой крестик — обещание действия, которого нет.
class SearchBox : public UIElement {
public:
    explicit SearchBox(std::string placeholder = {});
    const char* TypeName() const override { return "SearchBox"; }
    void OnAttach() override;
    void Update(float dt) override;

    const std::string& Value() const;
    SearchBox* SetValue(const std::string& text);
    SearchBox* OnChanged(std::function<void(const std::string&)> fn);

private:
    std::string m_placeholder;
    TextInput* m_input = nullptr;
    UIElement* m_clear = nullptr;
    std::function<void(const std::string&)> m_onChanged;
};

// --- Выпадающий список -------------------------------------------------------
class Dropdown : public UIElement {
public:
    explicit Dropdown(std::vector<std::string> items = {}, int index = 0);
    const char* TypeName() const override { return "Dropdown"; }
    void OnAttach() override;

    int Index() const { return m_index; }
    const std::string& Selected() const;
    Dropdown* SetItems(std::vector<std::string> items, int index = 0);
    Dropdown* SetIndex(int index);
    Dropdown* OnChanged(std::function<void(int)> fn);

private:
    void Refresh();

    std::vector<std::string> m_items;
    int m_index = 0;
    Label* m_text = nullptr;
    Popup* m_popup = nullptr;
    std::function<void(int)> m_onChanged;
};

// --- Числовое поле -----------------------------------------------------------
//
// Поле с ПЕРЕТАСКИВАНИЕМ: тянешь по горизонтали — значение меняется. Без этого
// правка позиции объекта превращается в набор цифр с клавиатуры, а её делают
// сотни раз за сеанс.
class NumericField : public UIElement {
public:
    explicit NumericField(float value = 0.0f);
    const char* TypeName() const override { return "NumericField"; }
    void OnAttach() override;

    float Value() const { return m_value; }
    NumericField* SetValue(float value);
    NumericField* SetRange(float min, float max);
    NumericField* SetStep(float step);   // сколько единиц на пиксель перетаскивания
    NumericField* SetLabel(const std::string& text); // «X», «Y», «Z»
    NumericField* OnChanged(std::function<void(float)> fn);

private:
    void Show();

    float m_value = 0.0f;
    float m_min = 0.0f, m_max = 0.0f;   // 0,0 — без границ
    float m_step = 0.01f;
    float m_dragStart = 0.0f;
    std::string m_label;
    Label* m_prefix = nullptr;
    Label* m_text = nullptr;
    std::function<void(float)> m_onChanged;
};

// --- Строка состояния --------------------------------------------------------
class StatusBar : public UIElement {
public:
    const char* TypeName() const override { return "StatusBar"; }
    void OnAttach() override;

    // Слева: точка состояния и текст.
    StatusBar* SetStatus(const std::string& text, const UIColor& dot);
    // Справа: пары «имя — значение». Заводятся один раз, обновляются по имени;
    // пересоздавать их каждый кадр значило бы пересчитывать раскладку строки
    // ради двух изменившихся цифр.
    void SetField(const std::string& name, const std::string& value);

private:
    struct Field {
        std::string Name;
        Label* Caption = nullptr;
        Label* Value = nullptr;
    };

    UIElement* m_dot = nullptr;
    Label* m_status = nullptr;
    UIElement* m_right = nullptr;
    std::vector<Field> m_fields;
};

} // namespace sage::ui::sui
