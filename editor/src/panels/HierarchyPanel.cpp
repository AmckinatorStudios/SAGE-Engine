#include "HierarchyPanel.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// Рекурсивно рисует узел дерева: сам элемент (выбор/ПКМ/drag-drop) + детей.
void HierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    bool hasChildren = h && !h->Children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (host.SelectedId() == id) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::PushID(id);
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)id, flags, "%s", name.c_str());
    // Клик по строке (не по треугольнику раскрытия) — выбор.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) host.SetSelectedId(id);

    // Перетаскиваем эту сущность как источник.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("SAGE_ENTITY", &id, sizeof(int));
        ImGui::Text("%s", name.c_str());
        ImGui::EndDragDropSource();
    }
    // Бросили другую сущность на эту — делаем эту родителем.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            if (childId != id) {
                host.PushUndoSnapshot();
                scene.SetParentById(childId, id);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню сущности.
    if (ImGui::BeginPopupContextItem()) {
        host.SetSelectedId(id);
        if (ImGui::MenuItem("Create Child")) {
            host.PushUndoSnapshot();
            GameObject child = scene.CreateObject("Child");
            scene.SetParent(child.Entity(), e);
            host.SetSelectedId(child.Id());
        }
        if (ImGui::MenuItem("Duplicate")) host.DuplicateSelected();
        bool hasParent = h && h->Parent != entt::null;
        if (ImGui::MenuItem("Unparent", nullptr, false, hasParent)) {
            host.PushUndoSnapshot();
            scene.SetParent(e, entt::null);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) host.DeleteSelected();
        ImGui::EndPopup();
    }

    if (open && hasChildren) {
        // Копия детей: SetParent во время обхода мог бы менять список.
        std::vector<entt::entity> kids = h->Children;
        std::sort(kids.begin(), kids.end(), [&](entt::entity a, entt::entity b) {
            return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
        });
        for (auto k : kids)
            if (reg.valid(k)) DrawNode(host, scene, k);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void HierarchyPanel::Draw(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("Scene: %s  |  Entities: %zu", scene.Name().c_str(), scene.Count());
    ImGui::Separator();

    // Корни (без родителя) в стабильном порядке по id.
    std::vector<std::pair<int, entt::entity>> roots;
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        bool hasParent = h && h->Parent != entt::null && reg.valid(h->Parent);
        if (!hasParent) roots.push_back({view.get<IdComponent>(e).Id, e});
    }
    std::sort(roots.begin(), roots.end());
    for (auto& [id, e] : roots) DrawNode(host, scene, e);

    // Зона «в корень»: бросок сюда открепляет сущность от родителя.
    ImGui::Dummy(ImVec2(-1.0f, 24.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParentById(childId, -1); // -1 -> entt::null (в корень)
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню пустого места — создание корневых сущностей.
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
