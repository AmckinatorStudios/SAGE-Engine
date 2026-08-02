#include "HierarchyPanel.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "Project.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace {

// Какая иконка у сущности. Порядок проверок — от самого «говорящего»
// компонента к самому общему: у камеры со скриптом важнее, что это камера, а
// меш есть почти у всего и потому проверяется последним.
//
// По иконке иерархия читается одним взглядом: в списке из полусотни «Object»
// глазу не за что зацепиться, а «свет / камера / зонд / модель» видно сразу.
const char* EntityIcon(entt::registry& reg, entt::entity e) {
    if (reg.all_of<CameraComponent>(e)) return "camera";
    if (reg.all_of<LightComponent>(e)) return "light";
    if (reg.all_of<ReflectionProbeComponent>(e)) return "probe";
    if (reg.all_of<ParticleEmitterComponent>(e)) return "particles";
    if (reg.all_of<AnimatedModelComponent>(e)) return "anim";
    if (reg.all_of<UIElementComponent>(e)) return "file";
    if (reg.all_of<RigidBodyComponent>(e) || reg.all_of<ColliderComponent>(e)) return "physics";
    if (reg.all_of<ScriptComponent>(e)) return "script";
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        if (mr->Ref.type == MeshRef::Type::Model) return "model";
        if (mr->Ref.type == MeshRef::Type::Sphere) return "sphere";
        if (mr->Ref.type != MeshRef::Type::None) return "cube";
    }
    return "file";
}

} // namespace

// Рекурсивно рисует узел дерева: сам элемент (выбор/ПКМ/drag-drop) + детей.
void HierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    bool hasChildren = h && !h->Children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (host.IsSelected(id)) flags |= ImGuiTreeNodeFlags_Selected; // подсветка всех выбранных
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::PushID(id);
    // Иконка рисуется ПОВЕРХ строки узла, а не отдельным элементом: узел ImGui
    // занимает всю ширину (SpanAvailWidth), и вставить перед ним что-либо
    // обычным способом нельзя — клик перестал бы попадать в строку.
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)id, flags, "  %s", name.c_str());

    // Иконка — Overlay, а НЕ Inline. Inline резервирует место под рисунок через
    // Dummy, то есть подаёт свой элемент, и «последним элементом» для ImGui
    // становится иконка. Всё, что спрашивает про последний элемент —
    // IsItemClicked, BeginDragDropSource, BeginDragDropTarget,
    // BeginPopupContextItem, — после этого отвечает про квадратик размером с
    // букву вместо строки дерева. Из-за этого в иерархии НЕ ВЫБИРАЛИСЬ объекты:
    // клик проверялся у иконки, попасть в которую можно было лишь случайно, а
    // заодно молча не работали перетаскивание и контекстное меню.
    {
        const float s = ImGui::GetTextLineHeight() * 0.86f;
        EditorIcons::Overlay(rowPos.x + indent - s * 1.15f,
                             rowPos.y + (ImGui::GetTextLineHeight() - s) * 0.5f, s,
                             EntityIcon(reg, e), glm::vec3(0.62f, 0.72f, 0.85f));
    }

    // Клик по строке (не по треугольнику раскрытия) — выбор. Ctrl — добавить/
    // убрать из набора (множественный выбор), обычный клик — одиночный.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) host.ToggleSelection(id);
        else host.SetSelectedId(id);
    }

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
        // ПКМ по невыбранному — переключаемся на него; по выбранному в наборе —
        // сохраняем набор (Duplicate/Delete применятся ко всем выбранным).
        if (!host.IsSelected(id)) host.SetSelectedId(id);
        if (ImGui::MenuItem("Create Child")) {
            host.PushUndoSnapshot();
            GameObject child = scene.CreateObject("Child");
            scene.SetParent(child.Entity(), e);
            host.SetSelectedId(child.Id());
        }
        if (ImGui::MenuItem("Duplicate")) host.DuplicateSelected();
        // Сохранить выбранную сущность (с детьми) как переиспользуемый префаб в
        // assets/ проекта. Имя файла — по имени сущности.
        if (ImGui::MenuItem("Save as Prefab")) {
            std::error_code ec;
            std::filesystem::path dir = host.CurrentProject().Dir() / "assets";
            std::filesystem::create_directories(dir, ec);
            std::string safe = name;
            for (char& c : safe) if (c == '/' || c == '\\' || c == ':') c = '_';
            std::string perr;
            if (!host.SaveSelectedAsPrefab(dir / (safe + ".sageprefab"), perr))
                host.SetStatusMessage("Prefab save failed: " + perr);
        }
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
