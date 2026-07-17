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

    if (ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Skybox", &env.Skybox.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3("Sky Top", &env.Skybox.TopColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Sky Horizon", &env.Skybox.HorizonColor.x); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Procedural gradient (top -> horizon), no textures");
    }

    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Fog", &env.Fog.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3("Fog Color", &env.Fog.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Fog Start", &env.Fog.Start, 0.2f, 0.0f, 500.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat("Fog End", &env.Fog.End, 0.2f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
        if (env.Fog.End < env.Fog.Start) env.Fog.End = env.Fog.Start;
        ImGui::TextDisabled("Linear distance fog (applied in Shaded mode)");
    }

    if (ImGui::CollapsingHeader("Scene lights", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Света — сущности сцены (точечные/прожекторы); здесь список для
        // навигации с пометкой типа.
        int count = 0;
        auto view = scene.Registry().view<LightComponent, IdComponent, NameComponent>();
        for (auto e : view) {
            ++count;
            int id = view.get<IdComponent>(e).Id;
            const LightComponent& lc = view.get<LightComponent>(e);
            const char* tag = lc.Kind == LightComponent::Type::Spot ? "[spot] " : "[point] ";
            std::string label = tag + view.get<NameComponent>(e).Name + "##light" + std::to_string(id);
            if (ImGui::Selectable(label.c_str(), host.SelectedId() == id)) host.SetSelectedId(id);
        }
        if (count == 0) ImGui::TextDisabled("(no light entities)");
        ImGui::TextDisabled("Add via Entity > Create Light; type/params in Inspector");
        ImGui::TextDisabled("Shader limit: %d point + %d spot lights per frame",
                            LightingEnvironment::MaxPointLights, LightingEnvironment::MaxSpotLights);
    }

    ImGui::End();
}
