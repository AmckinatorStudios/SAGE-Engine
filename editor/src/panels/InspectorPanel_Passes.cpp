// ---------------------------------------------------------------------------
// Инспектор: слой отрисовки объекта и проходы камеры (sage/ecs/RenderLayers.h,
// CameraComponent::Passes). Отдельным файлом: секции сущности и так большие.
// ---------------------------------------------------------------------------
#include "InspectorPassesUI.h"

#include <cstdio>
#include <sstream>
#include <string>

#include <imgui.h>

#include "EditorHost.h"
#include "EditorTheme.h"
#include "../Localization.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"

namespace sage::editor {

namespace {
std::string JoinLayers(const std::vector<std::string>& layers) {
    std::string out;
    for (const std::string& l : layers) {
        if (!out.empty()) out += ", ";
        out += l.empty() ? "default" : l;
    }
    return out;
}

std::vector<std::string> SplitLayers(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}
} // namespace

void DrawRenderLayerField(EditorHost& host, MeshRendererComponent& mr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", mr.Layer.c_str());
    if (ImGui::InputTextWithHint(T("Layer"), "default", buf, sizeof(buf))) {
        mr.Layer = buf;
        if (mr.Layer == "default") mr.Layer.clear();
    }
    host.TrackLastImGuiItem();
    EditorTheme::Hint(T("Render layer: which camera pass draws this object (empty — the main pass)"));
}

void DrawCameraPasses(EditorHost& host, CameraComponent& cam) {
    if (!ImGui::TreeNode(T("Render passes" "###Render passes"))) return;
    EditorTheme::Hint(T("Passes run in order. 'default' is the main pass; layers named in other passes are "
                        "drawn only there. A pass that clears depth is drawn over everything before it."));
    int removeAt = -1;
    for (size_t i = 0; i < cam.Passes.size(); ++i) {
        CameraPass& p = cam.Passes[i];
        ImGui::PushID((int)i);
        ImGui::SeparatorText((std::string(T("Pass")) + " " + std::to_string(i + 1)).c_str());
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", JoinLayers(p.Layers).c_str());
        if (ImGui::InputText(T("Layers"), buf, sizeof(buf))) p.Layers = SplitLayers(buf);
        host.TrackLastImGuiItem();
        if (ImGui::Checkbox(T("Clear depth"), &p.ClearDepth)) host.PushUndoSnapshot();
        ImGui::DragFloat(T("Pass FOV"), &p.Fov, 0.5f, 0.0f, 140.0f, "%.1f");
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Pass near"), &p.Near, 0.005f, 0.0f, 10.0f, "%.3f");
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Pass far"), &p.Far, 1.0f, 0.0f, 10000.0f, "%.0f");
        host.TrackLastImGuiItem();
        EditorTheme::Hint(T("0 — as the camera"));
        if (ImGui::Button(T("Remove pass"))) removeAt = (int)i;
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        host.PushUndoSnapshot();
        cam.Passes.erase(cam.Passes.begin() + removeAt);
    }
    if (ImGui::Button(T("Add pass"))) {
        host.PushUndoSnapshot();
        CameraPass p;
        p.Layers = {cam.Passes.empty() ? std::string("default") : std::string("layer")};
        cam.Passes.push_back(p);
    }
    ImGui::TreePop();
}

} // namespace sage::editor
