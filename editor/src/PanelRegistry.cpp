#include "PanelRegistry.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

#include "EditorIcons.h"
#include "Localization.h"
#include "PanelWindowId.h"
#include "PanelWindows.h"
#include "ui/Commands.h"

namespace sage::editor {

struct PanelRegistry::Entry {
    PanelSpec Spec;
    bool Own = false;     // флаг видимости, если его не дали снаружи
    int Order = 0;        // порядок регистрации — порядок в меню внутри раздела
    bool* Flag() { return Spec.Visible ? Spec.Visible : &Own; }
    const bool* Flag() const { return Spec.Visible ? Spec.Visible : &Own; }
};

PanelRegistry::PanelRegistry() = default;
PanelRegistry::~PanelRegistry() = default;

namespace {

std::string CommandId(const PanelSpec& s) {
    if (!s.Command.empty()) return s.Command;
    std::string id = "panel.";
    for (char c : s.Id) id += (char)std::tolower((unsigned char)c);
    return id;
}

} // namespace

PanelRegistry::Entry* PanelRegistry::FindEntry(const std::string& id) {
    for (auto& e : m_entries)
        if (e->Spec.Id == id) return e.get();
    return nullptr;
}

const PanelRegistry::Entry* PanelRegistry::FindEntry(const std::string& id) const {
    return const_cast<PanelRegistry*>(this)->FindEntry(id);
}

void PanelRegistry::Register(PanelSpec spec) {
    if (spec.Id.empty()) return;
    if (Entry* old = FindEntry(spec.Id)) {
        // Замена (плагин перезагрузился): место в меню и видимость остаются.
        const bool was = *old->Flag();
        old->Spec = std::move(spec);
        if (!old->Spec.Visible) old->Own = was;
        AddCommand(*old);
        return;
    }
    auto e = std::make_unique<Entry>();
    e->Spec = std::move(spec);
    e->Own = e->Spec.DefaultVisible;
    e->Order = m_nextOrder++;
    AddCommand(*e);
    m_entries.push_back(std::move(e));
}

bool PanelRegistry::Unregister(const std::string& id) {
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i]->Spec.Id != id) continue;
        if (m_commands) m_commands->Remove(CommandId(m_entries[i]->Spec));
        m_entries.erase(m_entries.begin() + (long)i);
        return true;
    }
    return false;
}

const PanelSpec* PanelRegistry::Find(const std::string& id) const {
    const Entry* e = FindEntry(id);
    return e ? &e->Spec : nullptr;
}

std::vector<const PanelSpec*> PanelRegistry::All() const {
    std::vector<const Entry*> sorted;
    for (const auto& e : m_entries) sorted.push_back(e.get());
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
        if (a->Spec.Section != b->Spec.Section) return a->Spec.Section < b->Spec.Section;
        return a->Order < b->Order;
    });
    std::vector<const PanelSpec*> out;
    for (const Entry* e : sorted) out.push_back(&e->Spec);
    return out;
}

size_t PanelRegistry::Count() const { return m_entries.size(); }

bool* PanelRegistry::Visible(const std::string& id) {
    Entry* e = FindEntry(id);
    return e ? e->Flag() : nullptr;
}

bool PanelRegistry::IsVisible(const std::string& id) const {
    const Entry* e = FindEntry(id);
    return e && *e->Flag();
}

void PanelRegistry::SetVisible(const std::string& id, bool visible) {
    if (bool* f = Visible(id)) *f = visible;
}

void PanelRegistry::Toggle(const std::string& id) {
    if (bool* f = Visible(id)) *f = !*f;
}

std::string PanelRegistry::WindowId(const PanelSpec& spec, EditorWorkspace workspace) {
    return spec.PerSpaceWindow ? panelid::For(spec.Id.c_str(), workspace) : spec.Id;
}

bool PanelRegistry::InSpace(const PanelSpec& spec, EditorWorkspace workspace) {
    return (spec.Spaces & (1u << (unsigned)workspace)) != 0;
}

void PanelRegistry::DrawPanels(EditorHost& host, EditorWorkspace workspace) {
    // По индексу, а не итератором: Draw панели вправе зарегистрировать или
    // снять другую панель (плагин перезагрузил себя кнопкой в своём окне).
    for (size_t i = 0; i < m_entries.size(); ++i) {
        Entry& e = *m_entries[i];
        if (!e.Spec.Draw || !InSpace(e.Spec, workspace)) continue;
        bool* open = e.Flag();
        if (!*open && !e.Spec.DrawWhenHidden) continue;
        const std::string id = WindowId(e.Spec, workspace);
        // Копия функции: Unregister из самого Draw уничтожил бы её посреди вызова.
        const auto draw = e.Spec.Draw;
        const bool wrap = e.Spec.WrapWindow && *open;
        if (wrap) panelwindows::Before(id.c_str());
        draw(host, open, id);
        if (wrap) panelwindows::After(id.c_str());
    }
}

void PanelRegistry::DrawMenuSection(PanelSection section) {
    for (const PanelSpec* s : All()) {
        if (s->Section != section) continue;
        Entry* e = FindEntry(s->Id);
        EditorIcons::MenuItemToggle(s->Icon.c_str(), T(s->Title.c_str()), e->Flag());
    }
}

void PanelRegistry::DrawDetachMenu(EditorWorkspace workspace) {
    for (const PanelSpec* s : All()) {
        if (!s->Detachable) continue;
        // Имя окна — ТЕКУЩЕГО пространства: у общих панелей их два, и галочка
        // обязана относиться к тому окну, которое человек сейчас видит.
        const std::string id = WindowId(*s, workspace);
        const bool detached = panelwindows::Detached(id.c_str());
        if (EditorIcons::MenuItemSelected("window", T(s->Title.c_str()), detached)) {
            panelwindows::SetDetached(id.c_str(), !detached);
            // Вытащенная закрытая панель обязана появиться — иначе галочка
            // ничего не меняет на экране.
            if (!detached) *FindEntry(s->Id)->Flag() = true;
        }
    }
}

void PanelRegistry::ForEachDocked(EditorWorkspace workspace,
                                  const std::function<void(const std::string&, PanelDock)>& dock) const {
    std::vector<const Entry*> list;
    for (const auto& e : m_entries) {
        const PanelPlace& p = workspace == EditorWorkspace::Interface ? e->Spec.InterfaceDock : e->Spec.SceneDock;
        if (p.Slot != PanelDock::None && InSpace(e->Spec, workspace)) list.push_back(e.get());
    }
    auto place = [workspace](const Entry* e) {
        return workspace == EditorWorkspace::Interface ? e->Spec.InterfaceDock : e->Spec.SceneDock;
    };
    std::stable_sort(list.begin(), list.end(),
                     [&](const Entry* a, const Entry* b) { return place(a).Order < place(b).Order; });
    for (const Entry* e : list) dock(WindowId(e->Spec, workspace), place(e).Slot);
}

void PanelRegistry::Restore() {
    for (auto& e : m_entries)
        if (e->Spec.InLayout) *e->Flag() = true;
}

bool PanelRegistry::AnyVisible() const {
    for (const auto& e : m_entries)
        if (*e->Flag()) return true;
    return false;
}

void PanelRegistry::CloseAll() {
    for (auto& e : m_entries) *e->Flag() = false;
}

void PanelRegistry::AttachCommands(Sage::UI::CommandRegistry& commands, std::string category) {
    m_commands = &commands;
    m_category = std::move(category);
    for (const auto& e : m_entries) AddCommand(*e);
}

void PanelRegistry::AddCommand(const Entry& e) {
    if (!m_commands) return;
    const std::string id = CommandId(e.Spec);
    const std::string panel = e.Spec.Id;
    m_commands->Remove(id);
    m_commands->Add({id, T(e.Spec.Title.c_str()), m_category, "", e.Spec.Icon, {},
                     [this, panel] { Toggle(panel); }});
}

} // namespace sage::editor
