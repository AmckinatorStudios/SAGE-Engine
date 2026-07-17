#include "HierarchyPanel.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/scene/Components.h"

void HierarchyPanel::Draw(EditorHost& host) {
    Scene& scene = host.CurrentScene();

    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Scene: %s  |  Entities: %zu", scene.Name().c_str(), scene.Count());
    ImGui::Separator();

    // Стабильный порядок — сортировка по id (порядок обхода entt не гарантирован).
    std::vector<std::pair<int, std::string>> items;
    auto view = scene.Registry().view<IdComponent, NameComponent>();
    for (auto e : view) {
        items.push_back({view.get<IdComponent>(e).Id, view.get<NameComponent>(e).Name});
    }
    std::sort(items.begin(), items.end());

    for (auto& [id, name] : items) {
        std::string label = name + "##" + std::to_string(id);
        if (ImGui::Selectable(label.c_str(), host.SelectedId() == id)) host.SetSelectedId(id);
        if (ImGui::BeginPopupContextItem()) {
            host.SetSelectedId(id);
            if (ImGui::MenuItem("Duplicate")) host.DuplicateSelected();
            if (ImGui::MenuItem("Delete")) host.DeleteSelected();
            ImGui::EndPopup();
        }
    }

    // Контекстное меню пустого места — создание сущностей.
    if (ImGui::BeginPopupContextWindow("##hierarchy_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Empty")) {
            host.PushUndoSnapshot();
            host.SetSelectedId(scene.CreateObject("Empty").Id());
        }
        if (ImGui::MenuItem("Create Cube")) {
            host.PushUndoSnapshot();
            host.SetSelectedId(host.CreateCubeEntity("Cube").Id());
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}
