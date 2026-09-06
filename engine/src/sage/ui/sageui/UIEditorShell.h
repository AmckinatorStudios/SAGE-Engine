#pragma once
#include <string>

#include "sage/ui/sageui/UIDock.h"
#include "sage/ui/sageui/UIViews.h"

// ---------------------------------------------------------------------------
// ОБОЛОЧКА ИНСТРУМЕНТА (§2, §14 ТЗ редактора).
//
// Складывает верх, низ и середину окна: полоса меню, тулбар, область докинга,
// строка состояния. Это ВСЁ, что она делает.
//
// ЧЕГО ЗДЕСЬ НЕТ И БЫТЬ НЕ МОЖЕТ: ни сцены, ни объектов, ни ассетов. Оболочка
// не знает, что за панели в ней стоят, — она знает, что панели бывают. §14
// требует ровно этого: «каждая панель — самостоятельный модуль и регистрируется
// в оболочке». Обратное — оболочка, знающая про инспектор, — и есть тот самый
// один огромный EditorLayer, которого просили не делать.
//
// Поэтому оболочка живёт в SAGE UI, а не в редакторе: второй инструмент движка
// (просмотрщик ассетов, отладчик сети) собирается из неё же.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

class EditorShell : public UIElement {
public:
    const char* TypeName() const override { return "EditorShell"; }
    void OnAttach() override;

    MenuBar* Menu() const { return m_menu; }
    Toolbar* Tools() const { return m_toolbar; }
    DockSpace* Dock() const { return m_dock; }
    StatusBar* Status() const { return m_status; }

    // Зарегистрировать панель. Оболочка кладёт её в док и заводит пункт в
    // меню «Окно», чтобы закрытую панель было чем вернуть.
    DockPanel* AddPanel(DockPanel* panel, DockSide side = DockSide::Center,
                        const std::string& nearId = {});
    // Меню «Окно» заполняется само по зарегистрированным панелям — иначе новую
    // панель приходится дописывать в двух местах, и однажды допишут в одно.
    void BuildWindowMenu(const std::string& title);

private:
    MenuBar* m_menu = nullptr;
    Toolbar* m_toolbar = nullptr;
    DockSpace* m_dock = nullptr;
    StatusBar* m_status = nullptr;
    Popup* m_windowMenu = nullptr;
};

} // namespace sage::ui::sui
