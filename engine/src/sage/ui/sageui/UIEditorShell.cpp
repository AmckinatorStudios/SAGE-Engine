#include "sage/ui/sageui/UIEditorShell.h"

#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/visual/UIFill.h"

namespace sage::ui::sui {

void EditorShell::OnAttach() {
    SetName("EditorShell");
    SetStretch(true, true);
    Ensure<UIFill>();
    SetStyle("Shell");
    // Сверху вниз и без зазоров: полосы оболочки стыкуются вплотную, зазор
    // между ними показал бы фон приложения полосой — это выглядит как щель.
    Vertical(0.0f)->Padding(UIEdges::Uniform(0.0f));
    Layout().Cross = UIAlign::Stretch;

    m_menu = Ctx().CreateIn<MenuBar>(this);
    Ctx().CreateIn<Separator>(this, false);
    m_toolbar = Ctx().CreateIn<Toolbar>(this);
    Ctx().CreateIn<Separator>(this, false);

    // Док забирает всю оставшуюся высоту. Именно он, а не «середина с полями»:
    // §8 требует, чтобы вьюпорт занимал всё доступное место между панелями.
    m_dock = Ctx().CreateIn<DockSpace>(this);
    m_dock->SetStretch(true, true);

    Ctx().CreateIn<Separator>(this, false);
    m_status = Ctx().CreateIn<StatusBar>(this);
}

DockPanel* EditorShell::AddPanel(DockPanel* panel, DockSide side, const std::string& nearId) {
    return m_dock ? m_dock->Add(panel, side, nearId) : nullptr;
}

void EditorShell::BuildWindowMenu(const std::string& title) {
    if (!m_menu || !m_dock) return;
    if (!m_windowMenu) m_windowMenu = m_menu->AddMenu(title);

    for (UIElement* child : m_windowMenu->Children()) Ctx().Destroy(child);
    for (const std::string& id : m_dock->PanelIds()) {
        DockPanel* panel = m_dock->Find(id);
        const std::string caption = panel ? panel->Title() : id;
        // Пункт ВОЗВРАЩАЕТ панель, а не «включает»: закрытая панель уехала из
        // раскладки, и просто пометить её видимой некуда.
        m_windowMenu->AddItem(caption, [this, id] { m_dock->Focus(id); });
    }
}

} // namespace sage::ui::sui
