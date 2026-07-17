#include "LightingPanel.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/scene/Components.h"

void LightingPanel::Draw(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    ImGui::Begin("Lighting");

    if (ImGui::CollapsingHeader("Ambient (hemisphere)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Sky", &env.SkyColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Ground", &env.GroundColor.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Strength", &env.AmbientStrength, 0.01f, 0.0f, 2.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Sky tints upward faces, Ground — downward");
    }

    if (ImGui::CollapsingHeader("Sun (directional)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Direction", &env.Sun.Direction.x, 0.02f, -1.0f, 1.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Color", &env.Sun.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Intensity", &env.Sun.Intensity, 0.02f, 0.0f, 5.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Direction is where light TRAVELS; casts shadows");
    }

    if (ImGui::CollapsingHeader("Point lights", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Точечные света — сущности сцены; здесь список для навигации.
        int count = 0;
        auto view = scene.Registry().view<LightComponent, IdComponent, NameComponent>();
        for (auto e : view) {
            ++count;
            int id = view.get<IdComponent>(e).Id;
            const std::string& name = view.get<NameComponent>(e).Name;
            std::string label = name + "##light" + std::to_string(id);
            if (ImGui::Selectable(label.c_str(), host.SelectedId() == id)) host.SetSelectedId(id);
        }
        if (count == 0) ImGui::TextDisabled("(no light entities)");
        ImGui::TextDisabled("Add via Entity > Create Light; edit in Inspector");
        ImGui::TextDisabled("Shader limit: %d point lights per frame", LightingEnvironment::MaxPointLights);
    }

    ImGui::End();
}
