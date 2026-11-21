#include "AnimPanels.h"

#include <cstdint>
#include <vector>

#include "imgui.h"

#include "AnimStage.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "sage/anim/Skeleton.h"
#include "sage/scene/Components.h"

namespace {

// Дерево костей по родительским связям скелета — порядок детей тот, что в
// файле модели. Своих папок и групп нет: кость показывается там, где она
// висит на самом деле, и путь в дереве — путь по скелету.
void DrawBone(AnimStage& stage, const sage::anim::Skeleton& sk, const std::vector<std::vector<int>>& kids, int i,
              int forceOpenTo) {
    const bool leaf = kids[(size_t)i].empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (stage.SelectedBone() == i) flags |= ImGuiTreeNodeFlags_Selected;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    // Выбрали кость во вьюпорте — путь к ней раскрывается, иначе её не найти.
    if (forceOpenTo >= 0) {
        for (int j = forceOpenTo; j >= 0; j = sk.Joints[(size_t)j].Parent)
            if (j == i && !leaf) { ImGui::SetNextItemOpen(true); break; }
    }
    const bool open = ImGui::TreeNodeEx((void*)(intptr_t)(i + 1), flags, "%s", sk.Joints[(size_t)i].Name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) stage.SelectBone(i);
    if (stage.SelectedBone() == i && forceOpenTo == i) ImGui::SetScrollHereY(0.5f);
    if (open && !leaf) {
        for (int c : kids[(size_t)i]) DrawBone(stage, sk, kids, c, forceOpenTo);
        ImGui::TreePop();
    }
}

void DrawObject(AnimStage& stage, Scene& scene, entt::entity e) {
    const std::vector<entt::entity> kids = scene.ChildrenOf(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (stage.SelectedEntity() == e) flags |= ImGuiTreeNodeFlags_Selected;
    if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    GameObject o(&scene.Registry(), e);
    const bool open = ImGui::TreeNodeEx((void*)(intptr_t)(uint32_t)entt::to_integral(e), flags, "%s", o.Name().c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) stage.SelectEntity(e);
    if (open && !kids.empty()) {
        for (entt::entity c : kids) DrawObject(stage, scene, c);
        ImGui::TreePop();
    }
}

} // namespace

void AnimHierarchyPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!*open) return;
    const std::string title = std::string(T("Animation Hierarchy")) + "###" + windowId;
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return;
    }
    AnimStage* stage = host.AnimationStage();
    if (!stage || stage->TargetId() < 0 || !stage->StageRoot().Valid()) {
        ImGui::TextDisabled("%s", T("No animation target"));
        EditorTheme::Hint(T("Choose it in Animation Inspector > Target"));
        ImGui::End();
        return;
    }
    // Выбор из вьюпорта — раскрыть к нему путь один раз, а не держать открытым.
    static int s_lastBone = -1;
    const int forceOpen = stage->SelectedBone() != s_lastBone ? stage->SelectedBone() : -1;
    s_lastBone = stage->SelectedBone();

    if (const sage::anim::Skeleton* sk = stage->Skeleton()) {
        ImGui::TextDisabled("%s  %d", T("Bones:"), sk->Count());
        // Дети каждой кости — один раз на кадр: скелет до 128 костей.
        std::vector<std::vector<int>> kids((size_t)sk->Count());
        for (int i = 0; i < sk->Count(); ++i) {
            const int p = sk->Joints[(size_t)i].Parent;
            if (p >= 0 && p < sk->Count()) kids[(size_t)p].push_back(i);
        }
        for (int i = 0; i < sk->Count(); ++i)
            if (sk->Joints[(size_t)i].Parent < 0) DrawBone(*stage, *sk, kids, i, forceOpen);
    } else {
        // Цель без скелета: её объекты — то, что анимирует клип свойств.
        DrawObject(*stage, stage->StageScene(), stage->StageRoot().Entity());
    }
    ImGui::End();
}
