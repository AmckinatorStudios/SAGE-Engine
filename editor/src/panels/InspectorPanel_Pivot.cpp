// Раздел Pivot внутри Transform: точка опоры, вокруг которой объект
// поворачивается и масштабируется (Transform::Pivot). Числа, готовые положения
// по габаритам модели, сброс и переход к правке опоры гизмо во вьюпорте.
#include "InspectorPanelDetail.h"

#include <glm/glm.hpp>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTools.h"
#include "InspectorControls.h"
#include "imgui.h"
#include "../Localization.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::editor::inspectordetail {

namespace {
// Готовое положение — доли габаритной коробки по осям: −1 — минимум, 0 —
// середина, +1 — максимум. Перед (Front) — +Z: туда смотрит камера сцены по
// умолчанию, и «передняя грань» — та, что видна.
struct Preset {
    const char* Name;
    glm::vec3 At;
};
const Preset kSides[] = {
    {"Center", {0, 0, 0}},       {"Bottom Center", {0, -1, 0}}, {"Top Center", {0, 1, 0}},
    {"Left", {-1, 0, 0}},        {"Right", {1, 0, 0}},          {"Front", {0, 0, 1}},
    {"Back", {0, 0, -1}},
};
const Preset kCorners[] = {
    {"Bottom Left Front", {-1, -1, 1}}, {"Bottom Right Front", {1, -1, 1}},
    {"Bottom Left Back", {-1, -1, -1}}, {"Bottom Right Back", {1, -1, -1}},
    {"Top Left Front", {-1, 1, 1}},     {"Top Right Front", {1, 1, 1}},
    {"Top Left Back", {-1, 1, -1}},     {"Top Right Back", {1, 1, -1}},
};

// Габариты модели в координатах объекта. Нет меша — нет и габаритов.
bool LocalBounds(GameObject obj, glm::vec3& lo, glm::vec3& hi) {
    const auto* mr = obj.Registry()->try_get<MeshRendererComponent>(obj.Entity());
    if (!mr || !mr->MeshPtr) return false;
    lo = mr->MeshPtr->BoundsMin();
    hi = mr->MeshPtr->BoundsMax();
    return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z;
}
} // namespace

glm::vec3 PivotPreset(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& at) {
    const glm::vec3 c = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    return c + half * at;
}

void DrawPivotSection(EditorHost& host, GameObject obj) {
    namespace ictl = sage::editor::ictl;
    Transform& tr = obj.GetTransform();
    // Заданная опора — раздел открыт с первого взгляда: она меняет поворот, и
    // спрятанной её не найти.
    if (!ictl::SubSection("transform_pivot", T("Pivot"), tr.Pivot != glm::vec3(0.0f))) return;

    // Числа — в координатах объекта. Опора меняется, а объект стоит:
    // SetPivotKeepPlace держит его мировое положение.
    glm::vec3 p = tr.Pivot;
    ictl::NumOpts o;
    o.Step = 0.05f;
    o.Format = "%.3f";
    o.HasReset = true;
    o.Reset = 0.0f;
    if (ictl::Vector3(host, T("Position"), &p, o)) tr.SetPivotKeepPlace(p);

    // Готовые положения — по габаритам модели. Без габаритов (пустой объект,
    // свет, камера) список не предлагает того, чего не может посчитать.
    glm::vec3 lo, hi;
    const bool hasBounds = LocalBounds(obj, lo, hi);
    ictl::Label(T("Preset"));
    ImGui::BeginDisabled(!hasBounds);
    if (ictl::BeginDropdown("##pivot_preset", T("Choose a point..."))) {
        auto item = [&](const Preset& pr) {
            if (ImGui::Selectable(T(pr.Name))) {
                host.PushUndoSnapshot();
                tr.SetPivotKeepPlace(PivotPreset(lo, hi, pr.At));
            }
        };
        for (const Preset& pr : kSides) item(pr);
        ictl::SeparatorText(T("Corners"));
        for (const Preset& pr : kCorners) item(pr);
        ictl::EndDropdown();
    }
    ImGui::EndDisabled();
    if (!hasBounds && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", T("The object has no model bounds: the pivot stays where it is"));

    ImGui::BeginDisabled(tr.Pivot == glm::vec3(0.0f));
    if (EditorIcons::Button("refresh", T("Reset"), T("Back to the object origin (0, 0, 0)"))) {
        host.PushUndoSnapshot();
        tr.SetPivotKeepPlace(glm::vec3(0.0f));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool& edit = host.Tools().PivotEdit;
    if (EditorIcons::Button("anchor-cc", T("Edit in Viewport"),
                            T("The gizmo stands at the pivot and drags only it; press again to return"), edit))
        edit = !edit;
    ictl::EndSubSection();
}

} // namespace sage::editor::inspectordetail
