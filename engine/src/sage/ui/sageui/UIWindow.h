#pragma once
#include <functional>
#include <string>

#include "sage/ui/sageui/UIWidgetsOO.h"

// ---------------------------------------------------------------------------
// ОКНА, ДИАЛОГИ, ВСПЛЫВАЮЩИЕ И ПОДСКАЗКИ (§26, §33, этап 7 ТЗ).
//
// ГЛАВНОЕ, ЧТО ЗДЕСЬ НАДО ПОНЯТЬ: окно — это ОБЫЧНЫЙ ЭЛЕМЕНТ. Не второй вид
// интерфейса, не отдельная подсистема с собственной отрисовкой и собственным
// вводом. Заголовок — узел, тело — узел, крестик — обычная кнопка. Перетаскивание
// идёт теми же событиями Drag, что и у ползунка, а «поверх всех» — тем же полем
// Order, что и у любого соседа.
//
// Почему это записано так подробно. Именно здесь чаще всего заводят вторую
// систему: у окна появляется своё «рисование рамки», свой «хит-тест заголовка»,
// свой «порядок окон» — и через полгода оказывается, что маски, эффекты, темы и
// сохранение в файл работают везде, кроме окон. В §26 ТЗ это сказано прямо:
// плавающее окно и пристыкованная панель обязаны быть одним и тем же UIElement.
//
// ЧТО ЧЕМ ОТЛИЧАЕТСЯ:
//   Window  — рамка с заголовком: двигается, тянется за угол, поднимается
//             щелчком. Живёт в любом слое.
//   Dialog  — окно в слое Modal поверх затемнения: пока оно открыто, ввод под
//             ним не проходит.
//   Popup   — то, что закрывается щелчком мимо: меню, выпадающий список.
//   Tooltip — подсказка у курсора; её никто не создаёт руками, она появляется
//             сама по TooltipKey узла.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

class Window : public Panel {
public:
    explicit Window(std::string title = {});

    const char* TypeName() const override { return "Window"; }
    void OnAttach() override;
    void Update(float dt) override;

    Window* SetTitle(const std::string& title);
    const std::string& Title() const;

    // Содержимое кладут СЮДА, а не в само окно: у окна есть заголовок, и
    // положенное «в окно» встало бы в столбец рядом с ним.
    UIElement* Body() const { return m_body; }

    Window* SetMovable(bool on);
    Window* SetResizable(bool on);
    Window* SetClosable(bool on);
    Window* SetMinSize(glm::vec2 size);

    // Поставить по центру родителя. Считается по УЖЕ посчитанной раскладке:
    // до первого кадра размеров родителя не существует, и «центр» был бы враньём.
    Window* Center();

    Window* OnClose(std::function<void()> fn);
    void Close();
    bool IsOpen() const { return IsVisible(); }
    void Open();

    // Поднять поверх соседей по слою. Не меняет слой: окно инструментов не
    // должно всплывать поверх модального диалога только потому, что его нажали.
    void Raise();

protected:
    UIElement* TitleBar() const { return m_titleBar; }

private:
    void InstallDrag();
    void InstallResize();

    std::string m_title;
    UIElement* m_titleBar = nullptr;
    Label* m_titleLabel = nullptr;
    Button* m_close = nullptr;
    UIElement* m_body = nullptr;
    UIElement* m_grip = nullptr;

    std::function<void()> m_onClose;
    glm::vec2 m_dragOrigin{0.0f, 0.0f};
    glm::vec2 m_sizeOrigin{0.0f, 0.0f};
    glm::vec2 m_minSize{160.0f, 80.0f};
    bool m_movable = true;
    bool m_resizable = true;
    bool m_centerRequested = false;
};

// Модальный диалог: затемнение на весь кадр плюс окно по центру.
//
// Затемнение — не украшение, а РАБОТА: это оно перехватывает ввод. Без него
// «модальность» пришлось бы проверять в каждом обработчике под окном, и один
// забытый обработчик означал бы кнопку, нажимаемую сквозь диалог.
class Dialog : public Window {
public:
    explicit Dialog(std::string title = {});

    const char* TypeName() const override { return "Dialog"; }
    void OnAttach() override;

    // Ряд кнопок внизу. Возвращает саму кнопку — чтобы можно было настроить.
    Button* AddButton(const std::string& text, std::function<void()> onPress);
    UIElement* Buttons() const { return m_buttons; }

    // Готовый вопрос «да/нет». Самый частый диалог в редакторе, и собирать его
    // руками в десятый раз — верный способ получить десять разных диалогов.
    static Dialog* Confirm(UIContext& ui, const std::string& title,
                           const std::string& text, const std::string& okText,
                           std::function<void()> onOk);

private:
    UIElement* m_dim = nullptr;
    UIElement* m_buttons = nullptr;
};

// Всплывающее: меню, выпадающий список, палитра. Закрывается щелчком мимо и
// клавишей Escape — оба способа обязательны, иначе меню однажды остаётся на
// экране навсегда.
class Popup : public Panel {
public:
    const char* TypeName() const override { return "Popup"; }
    void OnAttach() override;
    void Update(float dt) override;

    // Открыть так, чтобы верхний левый угол был в этой ЭКРАННОЙ точке. Если
    // не влезает — сдвигается внутрь кадра: меню, наполовину уехавшее за край,
    // бесполезно.
    void OpenAt(glm::vec2 screenPoint);
    // Открыть под элементом — обычный случай для выпадающего списка.
    void OpenUnder(UIElement* anchor);
    void Close();
    bool IsOpen() const { return IsVisible(); }

    // Пункт меню: подпись и действие. Возвращает кнопку — можно добавить
    // значок или сочетание клавиш.
    Button* AddItem(const std::string& text, std::function<void()> onPress);
    void AddSeparator();

    Popup* OnClosed(std::function<void()> fn);

private:
    friend class UIContext;
    void PlaceInside(glm::vec2 topLeft);

    std::function<void()> m_onClosed;
    glm::vec2 m_pending{0.0f, 0.0f};
    bool m_needsPlacement = false;
};

} // namespace sage::ui::sui
