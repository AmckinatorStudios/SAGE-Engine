#include "PanelRegistry.h"

#include <algorithm>
#include <cctype>

#include <cfloat>
#include "imgui.h"
#include "imgui_internal.h"
#include "EditorTheme.h"

#include "EditorIcons.h"
#include "Localization.h"
#include "PanelWindowId.h"
#include "PanelWindows.h"
#include "ui/Commands.h"
#include "ui/UI.h"

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

const PanelPlace& PanelRegistry::PlaceIn(const PanelSpec& spec, EditorWorkspace workspace) {
    switch (workspace) {
        case EditorWorkspace::Interface: return spec.InterfaceDock;
        case EditorWorkspace::Animation: return spec.AnimationDock;
        default: return spec.SceneDock;
    }
}

bool PanelRegistry::InSpace(const PanelSpec& spec, EditorWorkspace workspace) {
    return (spec.Spaces & (1u << (unsigned)workspace)) != 0;
}

PanelRegistry::ResolvedLimits PanelRegistry::LimitsFor(const PanelSpec& spec, EditorWorkspace workspace,
                                                       float scale) {
    const PanelPlace& place = PlaceIn(spec, workspace);
    const bool side = place.Slot == PanelDock::Left || place.Slot == PanelDock::Right;
    auto pick = [&](float v, float dflt) { return v < 0.0f ? dflt : v; };
    ResolvedLimits r;
    r.Min[0] = pick(spec.Limits.MinW, kDefaultMinWidth) * scale;
    r.Min[1] = pick(spec.Limits.MinH, kDefaultMinHeight) * scale;
    const float maxW = pick(spec.Limits.MaxW, side ? kDefaultSideMaxWidth : 0.0f);
    const float maxH = pick(spec.Limits.MaxH, 0.0f);
    r.Max[0] = maxW > 0.0f ? std::max(maxW * scale, r.Min[0]) : FLT_MAX;
    r.Max[1] = maxH > 0.0f ? std::max(maxH * scale, r.Min[1]) : FLT_MAX;
    return r;
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
        const bool wrap = e.Spec.Detachable && *open;
        if (wrap) panelwindows::Before(id.c_str());
        // Пределы размера — перед Begin панели: докинг читает их у окна (см.
        // DockNodeCalcLimit в imgui.cpp). Не доставшиеся окну (Draw ничего не
        // открыл) снимаем, чтобы не ограничить чужое окно следующим Begin.
        if (*open) {
            const ResolvedLimits lim = LimitsFor(e.Spec, workspace, EditorTheme::EffectiveScale());
            ImGui::SetNextWindowSizeConstraints(ImVec2(lim.Min[0], lim.Min[1]), ImVec2(lim.Max[0], lim.Max[1]));
        }
        draw(host, open, id);
        // Не доставшееся окну (Draw ничего не открыл) снимаем и тут: класс окна и док
        // «вернуть в редактор» достались бы чужому Begin следующей панели.
        if (*open)
            ImGui::GetCurrentContext()->NextWindowData.HasFlags &=
                ~(ImGuiNextWindowDataFlags_HasSizeConstraint | ImGuiNextWindowDataFlags_HasWindowClass |
                  ImGuiNextWindowDataFlags_HasDock);
    }
}

bool PanelRegistry::HasMenuItems(PanelSection section) const {
    for (const auto& e : m_entries)
        if (e->Spec.Section == section && e->Spec.InMenu) return true;
    return false;
}

void PanelRegistry::DrawMenuSection(PanelSection section) {
    for (const PanelSpec* s : All()) {
        if (s->Section != section || !s->InMenu) continue;
        Entry* e = FindEntry(s->Id);
        EditorIcons::MenuItemToggle(s->Icon.c_str(), T(s->Title.c_str()), e->Flag());
    }
}

bool PanelRegistry::AnyFloating(EditorWorkspace workspace) const {
    for (const auto& e : m_entries) {
        if (!e->Spec.Detachable || !InSpace(e->Spec, workspace) || !*e->Flag()) continue;
        if (panelwindows::IsFloating(WindowId(e->Spec, workspace).c_str())) return true;
    }
    return false;
}

void PanelRegistry::ReturnFloating(EditorWorkspace workspace) {
    for (const auto& e : m_entries) {
        if (!e->Spec.Detachable || !InSpace(e->Spec, workspace) || !*e->Flag()) continue;
        const std::string id = WindowId(e->Spec, workspace);
        if (panelwindows::IsFloating(id.c_str())) panelwindows::RequestReturn(id.c_str());
    }
}

void PanelRegistry::ForEachDocked(EditorWorkspace workspace,
                                  const std::function<void(const std::string&, PanelDock)>& dock) const {
    std::vector<const Entry*> list;
    for (const auto& e : m_entries) {
        const PanelPlace& p = PlaceIn(e->Spec, workspace);
        if (p.Slot != PanelDock::None && InSpace(e->Spec, workspace)) list.push_back(e.get());
    }
    auto place = [workspace](const Entry* e) { return PlaceIn(e->Spec, workspace); };
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

std::map<std::string, bool> PanelRegistry::VisibilitySnapshot() const {
    std::map<std::string, bool> out;
    for (const auto& e : m_entries) out[e->Spec.Id] = *e->Flag();
    return out;
}

void PanelRegistry::ApplyVisibility(const std::map<std::string, bool>& visible) {
    for (auto& e : m_entries) {
        auto it = visible.find(e->Spec.Id);
        if (it != visible.end()) *e->Flag() = it->second;
    }
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
