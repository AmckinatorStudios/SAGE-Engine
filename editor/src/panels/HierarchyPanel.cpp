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
#include "AssetSlot.h"
#include "EditorIcons.h"
#include "Project.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/scene/Scene.h"
#include "../Localization.h"
#include "EditorTheme.h"
#include "ui/UI.h"

namespace {

// Какая иконка у сущности. Порядок проверок — от самого «говорящего»
// компонента к самому общему: у камеры со скриптом важнее, что это камера, а
// меш есть почти у всего и потому проверяется последним.
//
// По иконке иерархия читается одним взглядом: в списке из полусотни «Object»
// глазу не за что зацепиться, а «свет / камера / зонд / модель» видно сразу.
const char* EntityIcon(entt::registry& reg, entt::entity e) {
    if (reg.all_of<CameraComponent>(e)) return "camera";
    // Солнце — не лампа. Направленный свет один на сцену и задаёт всё её
    // настроение, поэтому в списке он обязан отличаться с первого взгляда.
    if (const LightComponent* lc = reg.try_get<LightComponent>(e)) {
        return lc->Kind == LightComponent::Type::Directional ? "sun" : "light";
    }
    if (reg.all_of<ReflectionProbeComponent>(e)) return "probe";
    if (reg.all_of<ParticleEmitterComponent>(e)) return "particles";
    if (reg.all_of<AnimatedModelComponent>(e)) return "anim";
    if (reg.all_of<sage::ui::Transform>(e)) return "file";
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
void HierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e, bool leaf) {
    entt::registry& reg = scene.Registry();
    int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    bool hasChildren = !leaf && h && !h->Children.empty();

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
                             EntityIcon(reg, e), EditorIcons::kThemeColor);
    }

    // Полоса акцента у левого края ВЫБРАННОЙ строки.
    //
    // Одной заливки мало: подложка выделения намеренно слабая (18% акцента),
    // иначе десяток выбранных строк превращает список в жёлтое поле. Но слабую
    // заливку не видно на светлой теме и не видно боковым зрением — а «что
    // сейчас выбрано» глаз должен ловить, не читая. Полоса решает это, не
    // усиливая заливку.
    if (host.IsSelected(id)) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float w = std::max(2.0f, 2.0f * EditorTheme::UiScale());
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(ImGui::GetWindowPos().x, a.y), ImVec2(ImGui::GetWindowPos().x + w, b.y),
            EditorTheme::Color32(EditorTheme::Role::Accent));
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
        // Ассет, брошенный НА СУЩНОСТЬ, относится к ней: материал красит её,
        // скрипт вешается на неё, модель заменяет её меш. Бросок в пустое место
        // списка (ниже) означает другое — «добавить в сцену», — и различает их
        // именно то, на что попали.
        // Ответ даётся ДО отпускания кнопки: подсказка говорит, что именно
        // произойдёт с этим файлом на этой сущности. Раньше бросок был
        // «наугад»: материал красил, модель заменяла меш, а .txt не делал
        // ничего — и все три случая выглядели одинаково, пока не отпустишь.
        const ImGuiDragDropFlags peek =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH", peek)) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            const char* what = nullptr;
            switch (assetslot::KindOf(dropped)) {
                case assetslot::Kind::Material: what = T("Assign the material to this object"); break;
                case assetslot::Kind::Model:    what = T("Replace this object's mesh"); break;
                case assetslot::Kind::Script:   what = T("Attach the script to this object"); break;
                case assetslot::Kind::Prefab:   what = T("Add the prefab as a child"); break;
                default: break;
            }
            ImGui::BeginTooltip();
            if (what) ImGui::TextUnformatted(what);
            else ImGui::TextDisabled("%s", T("This file cannot be applied to an object"));
            ImGui::EndTooltip();
            if (what) {
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(r0, r1, IM_COL32(120, 210, 130, 255), 3.0f, 0,
                                                    1.5f);
            }
            if (p->IsDelivery() && what) host.ApplyAssetToEntity(id, dropped);
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню сущности.
    if (ImGui::BeginPopupContextItem()) {
        // ПКМ по невыбранному — переключаемся на него; по выбранному в наборе —
        // сохраняем набор (Duplicate/Delete применятся ко всем выбранным).
        if (!host.IsSelected(id)) host.SetSelectedId(id);
        if (ImGui::MenuItem(T("Create Child"))) {
            host.PushUndoSnapshot();
            GameObject child = scene.CreateObject("Child");
            scene.SetParent(child.Entity(), e);
            host.SetSelectedId(child.Id());
        }
        if (ImGui::MenuItem(T("Duplicate"))) host.DuplicateSelected();
        // Сохранить выбранную сущность (с детьми) как переиспользуемый префаб в
        // assets/ проекта. Имя файла — по имени сущности.
        if (ImGui::MenuItem(T("Save as Prefab"))) {
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
        if (ImGui::MenuItem(T("Unparent"), nullptr, false, hasParent)) {
            host.PushUndoSnapshot();
            scene.SetParent(e, entt::null);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Delete"))) host.DeleteSelected();
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

// Плоский список совпавших. Узлы рисуются тем же DrawNode, поэтому у найденной
// строки работает всё то же самое: выбор, ПКМ, перетаскивание, бросок ассета.
// Разница одна — детей не разворачиваем: они либо совпали сами и стоят в
// списке отдельной строкой, либо к поиску отношения не имеют.
void HierarchyPanel::DrawFiltered(EditorHost& host, Scene& scene) {
    entt::registry& reg = scene.Registry();
    std::vector<std::pair<int, entt::entity>> hits;
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        if (Sage::UI::Matches(view.get<NameComponent>(e).Name, m_filter))
            hits.push_back({view.get<IdComponent>(e).Id, e});
    }
    std::sort(hits.begin(), hits.end());

    if (hits.empty()) {
        Sage::UI::EmptyState(T("Nothing found"), T("Try a different name"));
        return;
    }
    for (auto& [id, e] : hits) DrawNode(host, scene, e, /*leaf=*/true);
}

void HierarchyPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    ImGui::Begin(T("Hierarchy" "###Hierarchy"), open);

    // ПОИСК ПО ИМЕНИ — первым делом, ещё до списка.
    //
    // Дерево из полусотни объектов пролистывается, а из пятисот — уже нет, и
    // «найти в сцене нужный объект» превращалось в единственную операцию, у
    // которой в редакторе не было ни одного инструмента: ни поиска, ни
    // сортировки. Причём чаще всего человек ЗНАЕТ имя — он сам его и задал.
    Sage::UI::SearchField("##hierarchy_filter", m_filter, sizeof(m_filter), T("Search..."));
    Sage::UI::TextSecondary(T("Scene: %s  |  Entities: %zu"), scene.Name().c_str(), scene.Count());
    Sage::UI::Separator();

    if (m_filter[0]) {
        DrawFiltered(host, scene);
    } else {
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
    }

    // Зона «в корень»: бросок сюда открепляет сущность от родителя, а
    // брошенный ассет добавляется в сцену как новый объект.
    ImGui::Dummy(ImVec2(-1.0f, 24.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParentById(childId, -1); // -1 -> entt::null (в корень)
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            // Точки под курсором тут нет — список это не трёхмерный вид,
            // — поэтому объект встаёт в начало координат, как при создании
            // через меню Entity.
            if (!host.AddAssetToScene(dropped))
                host.SetStatusMessage(T("Only a model or a prefab can be added to the scene"));
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню пустого места — создание корневых сущностей.
    if (ImGui::BeginPopupContextWindow("##hierarchy_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(T("Create Empty"))) {
            host.PushUndoSnapshot();
            host.SetSelectedId(scene.CreateObject("Empty").Id());
        }
        if (ImGui::MenuItem(T("Create Cube"))) {
            host.PushUndoSnapshot();
            host.SetSelectedId(host.CreateCubeEntity("Cube").Id());
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}
