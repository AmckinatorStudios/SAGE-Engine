#include "AiGraphPanel.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

#include "../EditorHost.h"
#include "../Project.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "AiGraphDetail.h"
#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/core/Paths.h"

// Окно графа: левая панель (граф и его переменные), правая (узел), палитра
// узлов, меню узла, окна «Открыть» и «Сохранить как».

namespace fs = std::filesystem;
using sage::ai::GraphKind;
using sage::ai::GraphNode;
using sage::ai::NodeType;
using sage::vars::Value;
using namespace sage::editor::aigraph;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const char* KindTitle(GraphKind k) {
    switch (k) {
        case GraphKind::Behavior: return T("Behaviour graph");
        case GraphKind::Task: return T("Task graph");
        case GraphKind::Function: return T("Function graph");
        case GraphKind::Script: return T("Visual script");
    }
    return "";
}

} // namespace

void AiGraphPanel::DrawGraphSide(EditorHost& host) {
    ImGui::TextDisabled("%s", T("Graph"));
    std::string name = m_doc.Name;
    ImGui::SetNextItemWidth(-1.0f);
    if (InputStr("##gname", name, T("Name — the task is called by it"))) {
        EditBegin();
        // Точки останова — по имени графа: переезжают вместе с ним.
        sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
        const auto breaks = dbg.Breakpoints();
        for (const auto& [g, node] : breaks)
            if (g == m_doc.Name) {
                dbg.ToggleBreakpoint(g, node);
                dbg.ToggleBreakpoint(name, node);
            }
        m_doc.Name = name;
        MarkChanged();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##gkind", KindTitle(m_doc.Kind))) {
        for (GraphKind k : m_mode.Kinds)
            if (ImGui::Selectable(KindTitle(k), m_doc.Kind == k)) {
                PushUndo();
                m_doc.Kind = k;
                MarkChanged();
            }
        ImGui::EndCombo();
    }
    std::string desc = m_doc.Description;
    ImGui::SetNextItemWidth(-1.0f);
    if (InputStr("##gdesc", desc, T("What this graph does"))) {
        EditBegin();
        m_doc.Description = desc;
        MarkChanged();
    }
    if (m_doc.Kind == GraphKind::Task) {
        // Каналы персонажа, которые задача занимает (через запятую), и можно ли
        // её прервать — правила параллельного выполнения (TaskSignature).
        std::string channels;
        for (const std::string& c : m_doc.Channels) channels += (channels.empty() ? "" : ", ") + c;
        ImGui::SetNextItemWidth(-1.0f);
        if (InputStr("##gchan", channels, T("Channels it occupies: legs, voice…"))) {
            EditBegin();
            m_doc.Channels.clear();
            std::string cur;
            for (char ch : channels + ",") {
                if (ch == ',') {
                    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                    if (!cur.empty()) m_doc.Channels.push_back(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            MarkChanged();
        }
        bool interruptible = m_doc.Interruptible;
        if (ImGui::Checkbox(T("May be interrupted"), &interruptible)) {
            PushUndo();
            m_doc.Interruptible = interruptible;
            MarkChanged();
        }
    }
    if (m_doc.Kind == GraphKind::Function) {
        std::string cat = m_doc.Category;
        ImGui::SetNextItemWidth(-1.0f);
        if (InputStr("##gcat", cat, T("Palette section"))) {
            EditBegin();
            m_doc.Category = cat;
            MarkChanged();
        }
    }
    ImGui::Spacing();
    DrawVarList(host, T("Inputs"), m_doc.Inputs, "in");
    DrawVarList(host, T("Outputs"), m_doc.Outputs, "out");
    DrawVarList(host, T("Local variables"), m_doc.Locals, "loc");
    ImGui::Spacing();
    if (m_mode.AiTools)
        ImGui::TextWrapped("%s", T("Instance (the memory of the character) and Shared variables are read and written "
                                   "by name with Get/Set Variable."));
    else
        ImGui::TextWrapped("%s", T("Instance — variables of the object (the Visual Script component), Shared — data "
                                   "shared by all scripts; read and written by name with Get/Set Variable."));
    if (!m_libError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", m_libError.c_str());
}

void AiGraphPanel::DrawNodeSide(EditorHost& host) {
    sage::ai::World* lib = Library();
    if (m_selected.size() != 1 || !lib) {
        ImGui::TextDisabled("%s", T("Node"));
        ImGui::TextWrapped("%s", m_selected.empty() ? T("Select a node to see what it does and to set its values.")
                                                    : T("Several nodes are selected."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", T("Right click on the canvas — add a node. Drag from a pin — a wire; "
                                   "release it in the empty space to pick a node that fits. "
                                   "Del — delete, Ctrl+D — duplicate, Ctrl+C/V — copy, F9 — breakpoint."));
        return;
    }
    GraphNode* n = m_doc.FindNode(*m_selected.begin());
    if (!n) return;
    const NodeType* type = TypeOf(*n);
    ImGui::TextUnformatted(type ? Tr(type->Title) : n->Type.c_str());
    ImGui::TextDisabled("%s  #%d", n->Type.c_str(), n->Id);
    if (!type) {
        ImGui::TextWrapped("%s", T("This node is not in the library: load the Lua file that declares it "
                                   "(assets/ai) and reload the library."));
        return;
    }
    if (!type->Description.empty()) ImGui::TextWrapped("%s", Tr(type->Description));
    if (type->Origin != "C++") ImGui::TextDisabled("%s %s", T("Declared in:"), type->Origin.c_str());

    // Настройки узла.
    if (!type->Settings.empty() && EditorTheme::SectionHeader(T("Settings"))) {
        for (const sage::ai::SettingSpec& st : type->Settings) {
            ImGui::PushID(st.Name.c_str());
            ImGui::TextUnformatted(Tr(st.Title.empty() ? st.Name : st.Title));
            ImGui::SetNextItemWidth(-1.0f);
            auto it = n->Values.find(st.Name);
            Value v = it != n->Values.end() ? it->second : st.Default;
            bool changed = false;
            std::vector<std::string> choices = st.Choices;
            if (st.Kind == "task") choices = lib->TaskNames();
            else if (st.Kind == "function") {
                for (const std::string& t : lib->TaskNames())
                    if (auto g = lib->FindGraph(t); g && g->Kind == GraphKind::Function) choices.push_back(t);
            }
            else if (st.Kind == "operation") choices = lib->OperationNames();
            else if (st.Kind == "type") {
                for (const sage::ai::TypeInfo* t : lib->Types().All())
                    if (lib->Types().IsStruct(t->Id) && t->Id != sage::ai::type::Struct) choices.push_back(t->Id);
            } else if (st.Kind == "var") {
                const std::string scope = n->Values.count("scope") ? n->Values.at("scope").AsString() : "local";
                const std::vector<sage::ai::GraphVar>* list = scope == "local"    ? &m_doc.Locals
                                                              : scope == "input"  ? &m_doc.Inputs
                                                              : scope == "output" ? &m_doc.Outputs
                                                                                  : nullptr;
                if (list)
                    for (const sage::ai::GraphVar& gv : *list) choices.push_back(gv.Name);
            }
            if (st.Kind == "bool") {
                bool b = v.AsBool();
                if (ImGui::Checkbox("##b", &b)) { v = Value(b); changed = true; }
            } else if (st.Kind == "int") {
                int i = v.AsInt();
                if (ImGui::DragInt("##i", &i, 0.1f, 0, 64)) { v = Value(i); changed = true; }
            } else if (st.Kind == "float") {
                float f = v.AsFloat();
                if (ImGui::DragFloat("##f", &f, 0.01f)) { v = Value(f); changed = true; }
            } else if (!choices.empty() && st.Kind != "event" && st.Kind != "string") {
                const std::string cur = v.AsString();
                if (ImGui::BeginCombo("##c", cur.empty() ? T("— choose —") : cur.c_str())) {
                    for (const std::string& c : choices)
                        if (ImGui::Selectable(c.c_str(), c == cur)) { v = Value(c); changed = true; }
                    ImGui::EndCombo();
                }
                // Имя можно и вписать: переменная памяти экземпляра заранее не объявлена.
                if (st.Kind == "var") {
                    std::string s = v.AsString();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (InputStr("##vs", s, T("or type a name"))) { v = Value(s); changed = true; }
                }
            } else {
                std::string s = v.AsString();
                if (InputStr("##s", s)) { v = Value(s); changed = true; }
            }
            if (changed) {
                EditBegin();
                n->Values[st.Name] = v;
                MarkChanged();
            }
            ImGui::PopID();
        }
    }

    // Значения несоединённых входов.
    const sage::ai::NodePins pins = PinsOf(*n);
    bool header = false;
    for (const sage::ai::PinSpec& p : pins.Inputs) {
        if (p.Type == sage::ai::type::Exec || m_doc.LinkInto(n->Id, p.Name)) continue;
        if (!header) {
            header = true;
            if (!EditorTheme::SectionHeader(T("Inputs"))) break;
        }
        ImGui::PushID(p.Name.c_str());
        ImGui::TextUnformatted(p.Title.empty() ? p.Name.c_str() : Tr(p.Title));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", p.Type.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        auto it = n->Values.find(p.Name);
        Value v = lib->Types().Convert(it != n->Values.end() ? it->second : p.Default, p.Type);
        if (ValueField(host, "v", p.Type, v, lib->Types())) {
            EditBegin();
            n->Values[p.Name] = v;
            MarkChanged();
        }
        if (it != n->Values.end()) {
            ImGui::SameLine();
            if (EditorIcons::IconOnlyButton("undo", T("Back to the default"))) {
                PushUndo();
                n->Values.erase(p.Name);
                MarkChanged();
            }
        }
        ImGui::PopID();
    }

    if (EditorTheme::SectionHeader(T("Comment"), 0)) {
        std::string c = n->Comment;
        ImGui::SetNextItemWidth(-1.0f);
        if (InputStr("##comment", c, T("Shown above the node"))) {
            EditBegin();
            n->Comment = c;
            MarkChanged();
        }
    }
    sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
    bool bp = dbg.HasBreakpoint(m_doc.Name, n->Id);
    if (ImGui::Checkbox(T("Breakpoint (F9)"), &bp)) dbg.ToggleBreakpoint(m_doc.Name, n->Id);
    if (const std::string g = LinkedGraph(*n); !g.empty() && EditorIcons::Button("open", T("Open the graph")))
        OpenTaskGraph(host, g);

    // Отладка: значения пинов этого узла в показанном выполнении.
    if (sage::ai::RunDebug* rd = dbg.FindRun(m_debugRun)) {
        if (EditorTheme::SectionHeader(T("Debug values"))) {
            ImGui::TextDisabled("%s %d", T("Steps:"), rd->NodeHits.count(n->Id) ? rd->NodeHits[n->Id] : 0);
            for (const auto& [key, value] : rd->Pins)
                if (key.first == n->Id) ImGui::Text("%s = %s", key.second.c_str(), value.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Палитра узлов: поиск по заголовку, разделу, ключевым словам; провод,
// отпущенный в пустоту, оставляет только подходящие узлы.
// ---------------------------------------------------------------------------
void AiGraphPanel::DrawPalette(EditorHost&) {
    if (m_paletteRequest) {
        ImGui::OpenPopup("###AiGraphPalette");
        m_paletteRequest = false;
        m_search[0] = 0;
    }
    Sage::UI::MenuScope menu;
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("###AiGraphPalette")) return;
    sage::ai::World* lib = Library();
    if (!lib) ImGui::CloseCurrentPopup();
    else DrawPaletteList(*lib);
    ImGui::EndPopup();
}

void AiGraphPanel::DrawPaletteList(sage::ai::World& libRef) {
    sage::ai::World* lib = &libRef;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##aig_search", T("Search nodes…"), m_search, sizeof(m_search));
    const std::string query = Lower(m_search);

    // Узлы по разделам, с учётом провода и поиска.
    std::map<std::string, std::vector<const NodeType*>> byCat;
    for (const NodeType* t : lib->Nodes().All()) {
        // Только узлы этого вида графа: задачи ИИ не предлагаются в Visual
        // Scripting, On Update — в графе ИИ, в функции — только общие.
        if (!sage::ai::NodeAllowed(t->Domains, m_doc.Kind)) continue;
        if (!query.empty()) {
            std::string hay = Lower(t->Id + " " + t->Title + " " + t->Category + " " + Tr(t->Title) + " " +
                                    TrPath(t->Category));
            for (const std::string& k : t->Keywords) hay += " " + Lower(k);
            if (hay.find(query) == std::string::npos) continue;
        }
        if (m_paletteFromLink) {
            GraphNode probe;
            probe.Type = t->Id;
            const sage::ai::GraphEnv env{lib, &lib->Types(), &m_doc};
            const sage::ai::NodePins pins = sage::ai::ResolvePins(*t, probe, env);
            bool fits = false;
            for (const sage::ai::PinSpec& p : m_paletteLink.Output ? pins.Inputs : pins.Outputs) {
                const std::string from = m_paletteLink.Output ? m_paletteLink.Type : p.Type;
                const std::string to = m_paletteLink.Output ? p.Type : m_paletteLink.Type;
                if (lib->Types().CanConnect(from, to)) {
                    fits = true;
                    break;
                }
            }
            if (!fits) continue;
        }
        byCat[t->Category].push_back(t);
    }
    ImGui::BeginChild("##aig_list", ImVec2(0, 0));
    std::string chosen;
    for (auto& [cat, list] : byCat) {
        const std::string catTitle = TrPath(cat);
        const bool open = !query.empty() || m_paletteFromLink
                              ? true
                              : ImGui::TreeNodeEx(cat.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", catTitle.c_str());
        if (!open) continue;
        if (!query.empty() || m_paletteFromLink) ImGui::TextDisabled("%s", catTitle.c_str());
        for (const NodeType* t : list) {
            if (ImGui::Selectable(Tr(t->Title))) chosen = t->Id;
            if (ImGui::IsItemHovered() && !t->Description.empty()) ImGui::SetTooltip("%s", Tr(t->Description));
        }
        if (query.empty() && !m_paletteFromLink) ImGui::TreePop();
    }
    if (byCat.empty()) ImGui::TextDisabled("%s", T("Nothing found"));
    ImGui::EndChild();
    if (!chosen.empty()) {
        PushUndo();
        const int id = AddNodeAt(chosen, ToGraph(m_paletteAt));
        // Провод, с которого открыли палитру, — к первому подходящему пину.
        if (m_paletteFromLink) {
            if (GraphNode* n = m_doc.FindNode(id)) {
                const sage::ai::NodePins pins = PinsOf(*n);
                for (const sage::ai::PinSpec& p : m_paletteLink.Output ? pins.Inputs : pins.Outputs) {
                    PinHit other{id, p.Name, !m_paletteLink.Output, p.Type, ImVec2()};
                    if (CanLink(m_paletteLink, other, nullptr)) {
                        Link(m_paletteLink, other);
                        break;
                    }
                }
            }
        }
        m_selected = {id};
        MarkChanged();
        m_paletteFromLink = false;
        ImGui::CloseCurrentPopup();
    }
}

void AiGraphPanel::DrawNodeMenu(EditorHost& host) {
    if (m_menuRequest) {
        ImGui::OpenPopup("###AiGraphNodeMenu");
        m_menuRequest = false;
    }
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("###AiGraphNodeMenu")) return;
    GraphNode* n = m_doc.FindNode(m_menuNode);
    if (!n) ImGui::CloseCurrentPopup();
    sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
    if (n) {
        if (EditorIcons::MenuItem("debug", T("Toggle breakpoint"), "F9")) dbg.ToggleBreakpoint(m_doc.Name, n->Id);
        if (EditorIcons::MenuItem("copy", T("Duplicate"), "Ctrl+D")) Duplicate();
        if (EditorIcons::MenuItem("wire", T("Disconnect all wires"))) {
            PushUndo();
            const int id = n->Id;
            m_doc.Links.erase(std::remove_if(m_doc.Links.begin(), m_doc.Links.end(),
                                             [id](const sage::ai::GraphLink& l) {
                                                 return l.From.Node == id || l.To.Node == id;
                                             }),
                              m_doc.Links.end());
            MarkChanged();
        }
        if (const std::string g = LinkedGraph(*n); !g.empty() && EditorIcons::MenuItem("open", T("Open the graph")))
            OpenTaskGraph(host, g);
        if (EditorIcons::MenuItem("trash", T("Delete"), "Del")) DeleteSelection();
    }
    ImGui::EndPopup();
}

void AiGraphPanel::DrawOpenPopup(EditorHost& host) {
    if (m_openRequest) {
        ImGui::OpenPopup("###AiGraphOpen");
        m_openRequest = false;
    }
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("###AiGraphOpen")) return;
    ImGui::TextDisabled("%s", m_mode.AiTools ? T("Graphs of the project (assets/ai)")
                                             : T("Visual scripts of the project (assets/visual)"));
    const std::string dir = AiDir(host);
    int count = 0;
    std::error_code ec;
    if (!dir.empty() && fs::exists(sage::PathFromUtf8(dir), ec)) {
        for (auto it = fs::recursive_directory_iterator(sage::PathFromUtf8(dir), ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (sage::PathToUtf8(it->path().extension()) != m_mode.Extension) continue;
            ++count;
            const std::string rel = sage::PathToUtf8(fs::relative(it->path(), sage::PathFromUtf8(dir), ec));
            if (EditorIcons::MenuItem("wire", rel.c_str())) {
                if (m_dirty && m_hasGraph) Save(host);
                Open(host, sage::PathToUtf8(it->path()));
            }
        }
    }
    if (!count) ImGui::TextDisabled("%s", T("No graphs yet — create one."));
    ImGui::EndPopup();
}

void AiGraphPanel::DrawSaveAsPopup(EditorHost& host) {
    if (m_saveAsRequest) {
        ImGui::OpenPopup("###AiGraphSaveAs");
        m_saveAsRequest = false;
    }
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("###AiGraphSaveAs")) return;
    ImGui::TextUnformatted(m_mode.AiTools ? T("Graph name (the file goes to assets/ai)")
                                          : T("Script name (the file goes to assets/visual)"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(260.0f);
    const bool enter = ImGui::InputText("##aig_name", m_saveName, sizeof(m_saveName),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ImGui::Button(T("Save"))) {
        if (SaveAs(host, m_saveName)) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
