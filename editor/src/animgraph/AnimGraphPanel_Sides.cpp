// Animation Graph вне холста: свойства выбранного узла (их рисует Inspector
// пространства — узлы на холсте компактные) и параметры графа (колонка окна).
#include "AnimGraphPanel.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../Project.h"
#include "../aigraph/AiGraphDetail.h"

using namespace sage::anim;
namespace ictl = sage::editor::ictl;

bool AnimGraphPanel::DrawInspector(EditorHost& host) {
    // Выбрали объект после узла — инспектор снова его.
    if (m_inspectNode && host.Selection().Changes() != m_inspectSelection) m_inspectNode = false;
    if (!m_inspectNode || !m_hasGraph || !m_graph.Find(m_selected)) return false;
    // Правки полей — в СВОЮ историю графа, а не в историю сцены. Одно
    // перетаскивание поля — один шаг отмены.
    ictl::UndoScope undo([this] {
        if (m_editUndo) return;
        PushUndo();
        m_editUndo = true;
    });
    const std::string header = std::string(T("Animation Graph Node")) + "###ag_node";
    if (EditorTheme::SectionHeader("wire", header.c_str())) DrawNodeInspector(host);
    return true;
}

void AnimGraphPanel::DrawNodeInspector(EditorHost& host) {
    AnimGraphNode* n = m_graph.Find(m_selected);
    if (!n) return;
    const AnimGraphInstance* live = LiveInstance(host);
    ImGui::TextUnformatted(NodeTitle(*n).c_str());
    ImGui::Spacing();
    switch (n->Kind) {
        case AnimNodeKind::Clip: {
            // Клип — файлом (.sageanim, кости по именам) или клипом самой модели
            // по имени: модель, клипы которой не вынуты, тоже годится.
            ImGui::TextUnformatted(T("Clip file"));
            const assetslot::Result r = assetslot::Draw(host, "ag_clip", assetslot::Kind::Animation, n->ClipPath, nullptr,
                                                        T("Or pick a clip of the preview model below"));
            if (r.Changed) {
                PushUndo();
                n->ClipPath = r.Path;
                if (!n->ClipPath.empty()) n->ClipName.clear();
                Changed();
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose an animation clip");
                c.Filters = {".sageanim"};
                c.FilterLabel = T("Animation");
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browse = Browse::ClipFile;
                m_browseNode = n->Id;
            }
            if (n->ClipPath.empty()) {
                const std::vector<std::string> names = ModelClipNames(host);
                if (names.empty()) {
                    ImGui::TextDisabled("%s", T("Select an object with a skeleton to pick one of its clips."));
                } else {
                    ictl::Label(T("Model clip"));
                    const char* preview = n->ClipName.empty() ? T("choose...") : n->ClipName.c_str();
                    if (ictl::BeginDropdown("##ag_modelclip", preview)) {
                        for (const std::string& name : names) {
                            if (ImGui::Selectable(name.c_str(), name == n->ClipName) && name != n->ClipName) {
                                PushUndo();
                                n->ClipName = name;
                                Changed();
                            }
                        }
                        ictl::EndDropdown();
                    }
                }
            }
            if (ictl::Switch(host, T("Loop"), &n->Loop)) Changed();
            ictl::NumOpts o;
            o.Step = 0.05f;
            o.Min = -8.0f;
            o.Max = 8.0f;
            o.Format = "%.2f";
            o.HasReset = true;
            o.Reset = 1.0f;
            if (ictl::Number(host, T("Speed"), &n->Speed, o)) Changed();
            if (live) ImGui::TextDisabled(T("Time: %.2f s"), live->ClipTime(n->Id));
            break;
        }
        case AnimNodeKind::Blend: {
            const bool wired = m_graph.LinkInto(n->Id, kBlendPinWeight) != nullptr;
            ImGui::BeginDisabled(wired);
            if (ictl::SliderNumber(host, T("Weight"), &n->Weight, 0.0f, 1.0f)) Changed();
            ImGui::EndDisabled();
            if (wired) ImGui::TextDisabled("%s", T("The weight comes from the wire."));
            if (live) ImGui::TextDisabled(T("Current weight: %.2f  (0 = A, 1 = B)"), live->BlendWeight(n->Id));
            break;
        }
        case AnimNodeKind::Float: {
            if (m_graph.Params.empty()) {
                ImGui::TextDisabled("%s", T("Add a parameter below first."));
                break;
            }
            ictl::Label(T("Parameter"));
            const char* preview = n->Param.empty() ? T("choose...") : n->Param.c_str();
            if (ictl::BeginDropdown("##ag_param", preview)) {
                for (const AnimGraphParam& p : m_graph.Params) {
                    if (ImGui::Selectable(p.Name.c_str(), p.Name == n->Param) && p.Name != n->Param) {
                        PushUndo();
                        n->Param = p.Name;
                        Changed();
                    }
                }
                ictl::EndDropdown();
            }
            if (!n->Param.empty() && !m_graph.FindParam(n->Param))
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", T("No such parameter in the graph"));
            break;
        }
        case AnimNodeKind::Output:
            ImGui::TextWrapped("%s", T("The pose that reaches this node is what the model plays."));
            if (!m_graph.LinkInto(n->Id, 0))
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", T("Nothing is connected: rest pose"));
            break;
    }
    if (n->Kind != AnimNodeKind::Output) {
        ImGui::Spacing();
        if (EditorIcons::Button("trash", T("Delete node"))) DeleteSelected();
    }
}

void AnimGraphPanel::DrawParams(EditorHost& host) {
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextWrapped("%s", T("Game code sets them: animator:SetFloat(name, value)"));
    ImGui::PopStyleColor();
    int removeAt = -1;
    for (size_t i = 0; i < m_graph.Params.size(); ++i) {
        AnimGraphParam& p = m_graph.Params[i];
        ImGui::PushID((int)i);
        // Имя: переименование тянет за собой узлы, которые на него смотрят.
        std::string name = p.Name;
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 6.0f));
        if (sage::editor::aigraph::InputStr("##name", name) && !name.empty() && !m_graph.FindParam(name)) {
            if (!m_editUndo) {
                PushUndo();
                m_editUndo = true;
            }
            for (AnimGraphNode& n : m_graph.Nodes)
                if (n.Kind == AnimNodeKind::Float && n.Param == p.Name) n.Param = name;
            p.Name = name;
            Changed();
        }
        ImGui::SameLine();
        if (EditorIcons::IconOnlyButton("trash", T("Remove parameter"))) removeAt = (int)i;

        if (ictl::SubSection("ag_param_range", T("Default and range"), false)) {
            ictl::NumOpts o;
            o.Step = 0.05f;
            o.Format = "%.2f";
            if (ictl::Number(host, T("Default"), &p.Default, o)) Changed();
            if (ictl::Number(host, T("Min"), &p.Min, o)) Changed();
            if (ictl::Number(host, T("Max"), &p.Max, o)) Changed();
            ictl::EndSubSection();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        PushUndo();
        m_graph.Params.erase(m_graph.Params.begin() + removeAt);
        Changed();
    }
    if (EditorIcons::Button("plus", T("Add parameter"))) {
        PushUndo();
        std::string name = "Speed";
        for (int k = 2; m_graph.FindParam(name); ++k) name = "Param" + std::to_string(k);
        m_graph.Params.push_back({name, 0.0f, 0.0f, 1.0f});
        Changed();
    }
}
