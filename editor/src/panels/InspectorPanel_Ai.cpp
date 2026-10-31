#include "InspectorPanel.h"

#include <cstdio>

#include "imgui.h"

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../Project.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../VarsEditor.h"
#include "sage/core/Paths.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/VisualScriptComponent.h"

// ---------------------------------------------------------------------------
// Секция «AI»: какой граф ведёт объект и с какой памятью он начинает.
//
// Граф — СЛОТОМ (перетащить .aigraph из Assets), а не полем с путём: путь,
// набранный руками, расходится с файлом на первом же переименовании. Рядом —
// «Открыть»: граф правят в редакторе графов, а не здесь.
//
// Память (Instance) — те же поля, что у переменных объекта: у каждого стража
// свой пост при одном общем графе.
// ---------------------------------------------------------------------------
void InspectorPanel::DrawAiSection(EditorHost& host, GameObject obj, entt::registry& reg, EntityRemovals& rm) {
    if (!reg.all_of<AiAgentComponent>(obj.Entity())) return;
    if (!EditorTheme::SectionHeader("wire", T("AI" "###AI"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Ai)) return;
    AiAgentComponent& ai = reg.get<AiAgentComponent>(obj.Entity());

    ImGui::TextUnformatted(T("Behaviour graph"));
    const assetslot::Result r = assetslot::Draw(host, "aigraph", assetslot::Kind::AiGraph, ai.Graph, &m_preview,
                                                T("Drop an .aigraph here"));
    if (r.Changed) {
        host.PushUndoSnapshot();
        ai.Graph = r.Path;
    }
    if (!ai.Graph.empty() && EditorIcons::Button("wire", T("Open in the graph editor")))
        host.OpenAiGraph(sage::PathToUtf8(host.CurrentProject().Dir() / sage::PathFromUtf8(ai.Graph)));

    char shared[64];
    std::snprintf(shared, sizeof(shared), "%s", ai.Shared.c_str());
    ImGui::TextUnformatted(T("Shared area"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##aishared", T("world — data shared by every AI of this area"), shared,
                                 sizeof(shared))) {
        ai.Shared = shared;
    }
    host.TrackLastImGuiItem();
    bool autoStart = ai.AutoStart;
    if (ImGui::Checkbox(T("Start the graph on Play"), &autoStart)) {
        host.PushUndoSnapshot();
        ai.AutoStart = autoStart;
    }
    ImGui::TextUnformatted(T("Memory at start (Instance)"));
    varsui::DrawTable(host, obj, ai.Memory, &m_preview);
}

// Секция «Visual Script»: граф игровой логики объекта и его переменные.
void InspectorPanel::DrawVisualScriptSection(EditorHost& host, GameObject obj, entt::registry& reg,
                                             EntityRemovals& rm) {
    if (!reg.all_of<VisualScriptComponent>(obj.Entity())) return;
    if (!EditorTheme::SectionHeader("wire", T("Visual Script" "###VisualScriptSection"), ImGuiTreeNodeFlags_DefaultOpen,
                                    &rm.VisualScript))
        return;
    VisualScriptComponent& vs = reg.get<VisualScriptComponent>(obj.Entity());
    const assetslot::Result r = assetslot::Draw(host, "vgraph", assetslot::Kind::VisualScript, vs.Graph, &m_preview,
                                                T("Drop a .vgraph here"));
    if (r.Changed) {
        host.PushUndoSnapshot();
        vs.Graph = r.Path;
    }
    if (!vs.Graph.empty() && EditorIcons::Button("wire", T("Open in Visual Script")))
        host.OpenVisualScript(sage::PathToUtf8(host.CurrentProject().Dir() / sage::PathFromUtf8(vs.Graph)));
    ImGui::TextUnformatted(T("Variables of the object (Instance)"));
    varsui::DrawTable(host, obj, vs.Vars, &m_preview);
}
