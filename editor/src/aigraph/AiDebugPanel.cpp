#include "AiDebugPanel.h"

#include <filesystem>
#include <functional>
#include <map>
#include <vector>

#include "../EditorHost.h"
#include "../PanelRegistry.h"
#include "../Project.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "AiGraphPanel.h"
#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/core/Paths.h"
#include "sage/scripting/ScriptEngine.h"

namespace {

ImVec4 StatusColor(const std::string& s) {
    if (s == "running") return ImVec4(0.45f, 0.8f, 1.0f, 1.0f);
    if (s == "succeeded") return ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
    if (s == "failed") return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
}

const char* StatusTitle(const std::string& s) {
    if (s == "running") return T("running");
    if (s == "succeeded") return T("succeeded");
    if (s == "failed") return T("failed");
    if (s == "cancelled") return T("cancelled");
    return s.c_str();
}

void Board(const char* title, const sage::ai::Blackboard* b) {
    if (!b || !ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (b->Size() == 0) ImGui::TextDisabled("%s", T("empty"));
    for (const auto& [name, v] : b->All()) ImGui::Text("%s = %s", name.c_str(), v.AsString().c_str());
    ImGui::TreePop();
}

} // namespace

void AiDebugPanel::Draw(EditorHost& host, bool* open, AiGraphPanel& graph) {
    ImGui::SetNextWindowSize(ImVec2(820, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((std::string(T("AI Debugger")) + "###AiDebugger").c_str(), open)) {
        ImGui::End();
        return;
    }
    sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
    ScriptEngine* scripts = host.PlayScripts();
    sage::ai::World* world = scripts ? &scripts->AI() : nullptr;

    if (!host.InPlayMode() && dbg.Runs().empty()) {
        ImGui::TextWrapped("%s", T("Start Play: every AI task of the game (graph, Lua or C++) shows up here with "
                                   "its state, its variables and the reason it ended. Breakpoints are set in the "
                                   "graph editor (F9)."));
        ImGui::End();
        return;
    }
    if (EditorIcons::Button("play", T("Continue all"))) dbg.ContinueAll();
    ImGui::SameLine();
    if (EditorIcons::Button("trash", T("Clear breakpoints"))) dbg.ClearBreakpoints();
    ImGui::SameLine();
    ImGui::TextDisabled(T("Breakpoints: %d"), (int)dbg.Breakpoints().size());

    const float h = ImGui::GetContentRegionAvail().y;
    // --- Дерево выполнений -------------------------------------------------
    ImGui::BeginChild("##aid_runs", ImVec2(300, h * 0.6f), ImGuiChildFlags_Borders);
    std::map<int, std::vector<int>> children;
    std::vector<int> roots;
    for (const auto& [id, r] : dbg.Runs()) {
        // Только ИИ: графы Visual Scripting отлаживаются в своём редакторе.
        if (r.Script) continue;
        if (r.Parent && dbg.Runs().count(r.Parent)) children[r.Parent].push_back(id);
        else roots.push_back(id);
    }
    std::function<void(int)> row = [&](int id) {
        const sage::ai::RunDebug& r = dbg.Runs().at(id);
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(r.Status));
        const std::string label = "#" + std::to_string(id) + " " + r.Task + " (" + r.Origin + ")" +
                                  (r.Paused ? "  ||" : "");
        const bool hasKids = children.count(id) != 0;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_run == id) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked()) {
            m_run = id;
            if (!r.Graph.empty()) graph.SetDebugRun(id);
        }
        if (open) {
            if (hasKids)
                for (int c : children[id]) row(c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    for (int id : roots) row(id);
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Выбранное -----------------------------------------------------------
    ImGui::BeginChild("##aid_run", ImVec2(0, h * 0.6f), ImGuiChildFlags_Borders);
    if (const sage::ai::RunDebug* r = dbg.FindRun(m_run)) {
        ImGui::Text("#%d %s", r->Run, r->Task.c_str());
        ImGui::TextDisabled(T("Written in: %s, AI instance #%d"), r->Origin.c_str(), r->Agent);
        ImGui::TextColored(StatusColor(r->Status), "%s", StatusTitle(r->Status));
        if (r->Ended > 0.0) ImGui::TextDisabled(T("Ran %.2f s"), r->Ended - r->Started);
        if (!r->Error.empty()) {
            ImGui::TextUnformatted(T("Reason:"));
            ImGui::TextWrapped("%s", r->Error.c_str());
        } else if (r->Status == "cancelled") {
            ImGui::TextWrapped("%s", T("Cancelled: by the caller, by its parent ending, or because its object "
                                       "was removed."));
        }
        if (r->Paused) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), T("Paused at node #%d"), r->PausedNode);
            if (EditorIcons::Button("play", T("Continue"))) dbg.Continue(r->Run);
            ImGui::SameLine();
            if (EditorIcons::Button("step", T("Step"))) dbg.Step(r->Run);
        }
        if (!r->Graph.empty() && EditorIcons::Button("wire", T("Show in the graph editor"))) {
            graph.SetDebugRun(r->Run);
            if (graph.Doc().Name != r->Graph) graph.OpenTaskGraph(host, r->Graph);
            if (graph.Doc().Name == r->Graph) {
                host.Panels().SetVisible("AiGraph", true);
                graph.RequestFocus();
                if (r->Paused) graph.FocusNode(r->PausedNode);
            }
        }
        // Живые переменные — пока выполнение идёт.
        sage::ai::RunPtr live = world ? world->FindRun(r->Run) : nullptr;
        if (live) {
            ImGui::Separator();
            Board(T("Inputs"), live->Inputs.get());
            Board(T("Outputs"), live->Outputs.get());
            Board(T("Local variables"), live->Locals.get());
            if (sage::ai::Agent* a = world->FindAgent(live->Agent)) {
                Board(T("Instance memory"), a->Memory.get());
                const std::string shared = std::string(T("Shared")) + ": " + a->SharedSpace;
                Board(shared.c_str(), world->Shared(a->SharedSpace).get());
            }
        } else if (r->Status != "running") {
            ImGui::TextDisabled("%s", T("Finished — its variables are gone; the history below keeps what happened."));
        }
    } else {
        ImGui::TextDisabled("%s", T("Select a task on the left."));
    }
    ImGui::EndChild();

    // --- История ---------------------------------------------------------------
    static const char* kKinds[] = {"", "node", "task", "event", "error", "log", "pause"};
    const char* kindTitles[] = {T("Everything"), T("Nodes"), T("Tasks"), T("Events"), T("Errors"), T("Log"),
                                T("Pauses")};
    ImGui::Checkbox(T("Only the selected task"), &m_onlyRun);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("##aid_kind", kindTitles[m_kind])) {
        for (int i = 0; i < 7; ++i)
            if (ImGui::Selectable(kindTitles[i], m_kind == i)) m_kind = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox(T("Follow"), &m_follow);
    ImGui::BeginChild("##aid_history", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const sage::ai::DebugEntry& e : dbg.History()) {
        if (m_onlyRun && e.Run != m_run) continue;
        if (m_kind && e.Kind != kKinds[m_kind]) continue;
        // Шаги узлов без текста — «узел #N графа G»: самые частые записи, поэтому коротко.
        const std::string text = e.Text.empty() ? e.Graph + " #" + std::to_string(e.Node) : e.Text;
        const ImVec4 c = e.Kind == "error" ? ImVec4(1, 0.45f, 0.4f, 1)
                         : e.Kind == "event" ? ImVec4(1, 0.8f, 0.4f, 1)
                         : e.Kind == "task"  ? ImVec4(0.5f, 0.8f, 1, 1)
                                             : ImVec4(0.8f, 0.8f, 0.8f, 1);
        ImGui::TextColored(c, "%8.2f  #%-4d %-6s %s", e.Time, e.Run, e.Kind.c_str(), text.c_str());
        if (ImGui::IsItemClicked()) m_run = e.Run;
    }
    if (m_follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}
