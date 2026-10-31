#include "AiGraphPanel.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../Localization.h"
#include "sage/ai/GraphDebug.h"
#include "sage/ai/GraphValidate.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// ПОД ХОЛСТОМ — две вещи, общие для SAGE AI Graph и Visual Scripting:
//
//   • ПРОБЛЕМЫ — проверка графа (sage/ai/GraphValidate.h) ещё до Play: узел
//     не того редактора, неизвестный узел, несовместимый провод, переменная,
//     которой нет, узел, до которого не дойдёт поток. Та же проверка не даёт
//     запустить граф с ошибкой — то, что здесь зелёное, не упадёт в игре на
//     том, что было видно заранее. Щелчок по строке — к узлу.
//   • ВЫВОД — что граф сделал в последнем Play: Log, ошибки узлов, конец
//     выполнения с причиной. Без него ошибка графа жила бы только в общем
//     журнале среди сотен строк движка.
// ---------------------------------------------------------------------------

void AiGraphPanel::Revalidate() {
    if (!m_issuesDirty) return;
    m_issuesDirty = false;
    m_issues.clear();
    sage::ai::World* lib = Library();
    if (!lib || !m_hasGraph) return;
    for (const sage::ai::GraphIssue& i : sage::ai::ValidateGraph(m_doc, *lib))
        m_issues.push_back({i.Node, {i.Error, i.Text}});
}

void AiGraphPanel::DrawBottom(EditorHost&) {
    Revalidate();
    int errors = 0, warnings = 0;
    for (const auto& [node, issue] : m_issues) (issue.first ? errors : warnings)++;
    const std::string shown = std::string(T("Problems")) + " (" + std::to_string(errors) + " / " +
                              std::to_string(warnings) + ")";
    const std::string problems = shown + "###aig_problems";
    // Ширина — по видимой подписи: суффикс «###» в неё не входит.
    if (ImGui::Selectable(problems.c_str(), m_bottomTab == 0, 0, ImVec2(ImGui::CalcTextSize(shown.c_str()).x, 0))) {
        m_bottomTab = 0;
        m_bottomOpen = true;
    }
    ImGui::SameLine();
    if (ImGui::Selectable(T("Output"), m_bottomTab == 1, 0, ImVec2(ImGui::CalcTextSize(T("Output")).x, 0))) {
        m_bottomTab = 1;
        m_bottomOpen = true;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(m_bottomOpen ? "eye-off" : "eye", m_bottomOpen ? T("Hide") : T("Show")))
        m_bottomOpen = !m_bottomOpen;
    if (!m_bottomOpen) return;
    ImGui::Separator();
    ImGui::BeginChild("##aig_bottom_list");
    if (m_bottomTab == 0) {
        if (m_issues.empty()) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s", T("No problems found"));
        for (size_t i = 0; i < m_issues.size(); ++i) {
            const auto& [node, issue] = m_issues[i];
            ImGui::PushID((int)i);
            const ImVec4 c = issue.first ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.8f, 0.4f, 1);
            std::string line = std::string(issue.first ? T("Error") : T("Warning")) + ": ";
            if (node) line += "#" + std::to_string(node) + " ";
            line += issue.second;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            if (ImGui::Selectable(line.c_str()) && node) FocusNode(node);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    } else {
        // Вывод — записи отладчика об этом графе: Log, ошибки, концы выполнений.
        int shown = 0;
        for (const sage::ai::DebugEntry& e : sage::ai::GraphDebugger::Get().History()) {
            if (e.Graph != m_doc.Name || e.Kind == "node" || e.Kind == "pause") continue;
            ++shown;
            const ImVec4 c = e.Kind == "error" ? ImVec4(1, 0.45f, 0.4f, 1)
                             : e.Kind == "log" ? ImVec4(0.85f, 0.85f, 0.85f, 1)
                                               : ImVec4(0.5f, 0.8f, 1, 1);
            ImGui::PushID(shown);
            const std::string line = std::to_string((int)(e.Time * 100) / 100.0).substr(0, 6) + "  " +
                                     (e.Node ? "#" + std::to_string(e.Node) + " " : std::string()) + e.Text;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            if (ImGui::Selectable(line.c_str()) && e.Node) FocusNode(e.Node);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (!shown) ImGui::TextDisabled("%s", T("Nothing yet — start Play: Log nodes, errors and the end of each run show up here."));
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}
