#include "AnimPanels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "ImGuizmo.h"

#include "AnimStage.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/scene/Components.h"

namespace ictl = sage::editor::ictl;

namespace {

// Точка мира -> точка экрана. false — за камерой.
bool ProjectPoint(const glm::mat4& viewProj, const glm::vec3& p, const glm::vec2& origin, const glm::vec2& size,
             glm::vec2& out) {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    out = origin + glm::vec2((ndc.x * 0.5f + 0.5f) * size.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
    return true;
}

float SegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-6f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

} // namespace

void AnimViewportPanel::DrawToolbar(EditorHost& host, AnimStage& stage) {
    (void)host;
    // Инструменты — как у вьюпорта сцены: те же значки и клавиши W / E / R.
    ictl::GroupItem ops[3];
    ops[0].Icon = "move";
    ops[0].Tooltip = T("Move (W)");
    ops[1].Icon = "rotate";
    ops[1].Tooltip = T("Rotate (E)");
    ops[2].Icon = "scale";
    ops[2].Tooltip = T("Scale (R)");
    for (int i = 0; i < 3; ++i) ops[i].Active = m_op == i;
    if (const int hit = ictl::ButtonGroup("##anim_ops", ops, 3); hit >= 0) m_op = hit;
    ImGui::SameLine();
    ictl::GroupItem space[2];
    space[0].Text = T("Local");
    space[0].Tooltip = T("Gizmo axes of the bone or object itself");
    space[0].Active = m_local;
    space[1].Text = T("World");
    space[1].Tooltip = T("Gizmo axes of the world");
    space[1].Active = !m_local;
    // Масштаб — всегда в своих осях: в мировых он перекашивал бы кость.
    if (const int hit = ictl::ButtonGroup("##anim_space", space, 2, ictl::ButtonLook::Standard); hit >= 0)
        m_local = hit == 0;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("fit", T("Frame the model (F)"))) stage.FrameTarget();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("refresh", T("Reset the camera"))) stage.ResetCamera();
    if (stage.HasPoseEdits()) {
        ImGui::SameLine();
        if (ictl::Button("undo", T("Reset pose"), T("Drop the pose edits that are not keyed yet")))
            stage.ClearPose();
    }
}

void AnimViewportPanel::DrawBones(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin,
                                  const glm::vec2& size) {
    const sage::anim::Skeleton* sk = stage.Skeleton();
    if (!sk) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bone = IM_COL32(40, 120, 220, 230);
    const ImU32 picked = EditorTheme::Color32(EditorTheme::Role::Accent);
    const int sel = stage.SelectedBone();
    // Кость — отрезок от родителя к суставу, сустав — кружок. Выбранная — цветом
    // акцента вместе с отрезком к ней: видно, ЧТО повернётся.
    for (int i = 0; i < sk->Count(); ++i) {
        glm::vec2 a, b;
        if (!ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(i)[3]), origin, size, b)) continue;
        const int parent = sk->Joints[(size_t)i].Parent;
        const ImU32 col = i == sel ? picked : bone;
        if (parent >= 0 && ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(parent)[3]), origin, size, a))
            dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, i == sel ? 3.0f : 2.0f);
        dl->AddCircleFilled(ImVec2(b.x, b.y), i == sel ? 5.0f : 3.5f, col);
    }
}

int AnimViewportPanel::PickBone(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin,
                                const glm::vec2& size, const glm::vec2& mouse) const {
    const sage::anim::Skeleton* sk = stage.Skeleton();
    if (!sk) return -1;
    // Сначала сустав (точнее), затем отрезок кости (длинную кость берут за
    // середину). Отрезок отдаёт ДОЧЕРНИЙ сустав: кость поворачивается в нём.
    int best = -1;
    float bestDist = 9.0f;
    for (int i = 0; i < sk->Count(); ++i) {
        glm::vec2 p;
        if (!ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(i)[3]), origin, size, p)) continue;
        const float d = glm::length(p - mouse);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best >= 0) return best;
    bestDist = 6.0f;
    for (int i = 0; i < sk->Count(); ++i) {
        const int parent = sk->Joints[(size_t)i].Parent;
        glm::vec2 a, b;
        if (parent < 0 || !ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(parent)[3]), origin, size, a) ||
            !ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(i)[3]), origin, size, b))
            continue;
        const float d = SegmentDistance(mouse, a, b);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void AnimViewportPanel::Manipulate(EditorHost& host, AnimStage& stage, const glm::vec2& origin, const glm::vec2& size) {
    (void)host;
    const int bone = stage.SelectedBone();
    const entt::entity ent = stage.SelectedEntity();
    Scene& scene = stage.StageScene();
    const bool hasEntity = ent != entt::null && scene.Registry().valid(ent);
    if (bone < 0 && !hasEntity) {
        m_gizmoWasUsing = false;
        return;
    }
    glm::mat4 model = bone >= 0 ? stage.BoneWorld(bone) : scene.WorldMatrix(ent);
    const glm::mat4 view = stage.View();
    const glm::mat4 proj = stage.Proj(size.x / std::max(1.0f, size.y));
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    const ImGuizmo::OPERATION op = m_op == 0 ? ImGuizmo::TRANSLATE : m_op == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = (m_local || m_op == 2) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(model))) {
        if (bone >= 0) {
            stage.SetBoneWorld(bone, model, m_op == 0, m_op == 1, m_op == 2);
        } else if (Transform* tr = scene.Registry().try_get<Transform>(ent)) {
            // Объект копии: локальный трансформ относительно родителя. Уровень
            // не трогается — ключ на это значение ставит таймлайн.
            const entt::entity p = scene.ParentOf(ent);
            tr->SetFromMatrix(p != entt::null ? glm::inverse(scene.WorldMatrix(p)) * model : model);
            stage.NotePoseEdit();
        }
    }
    m_gizmoWasUsing = ImGuizmo::IsUsing();
}

void AnimViewportPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!*open) return;
    const std::string title = std::string(T("Viewport")) + "###" + windowId;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    const bool visible = ImGui::Begin(title.c_str(), open);
    ImGui::PopStyleVar();
    AnimStage* stagePtr = host.AnimationStage();
    if (!visible || !stagePtr) {
        ImGui::End();
        return;
    }
    AnimStage& stage = *stagePtr;
    DrawToolbar(host, stage);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const glm::vec2 size(std::max(32.0f, avail.x), std::max(32.0f, avail.y));
    const glm::vec2 origin(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y);
    const float scale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    const uint64_t tex = stage.Render((int)(size.x * scale), (int)(size.y * scale));
    if (tex) ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(size.x, size.y), ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(ImVec2(size.x, size.y));

    const glm::mat4 viewProj = stage.Proj(size.x / size.y) * stage.View();
    DrawBones(stage, viewProj, origin, size);

    // Кнопка поверх картинки — после неё ImGuizmo: его ручки рисуются сверху
    // и выигрывают у выбора щелчком, пока курсор над ними.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##anim_view", ImVec2(size.x, size.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    Manipulate(host, stage, origin, size);
    const bool gizmoHot = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    ImGuiIO& io = ImGui::GetIO();
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (io.MouseWheel != 0.0f) stage.Zoom(io.MouseWheel);
    }
    // Камера: правая или средняя кнопка — орбита, с Shift — сдвиг (как в
    // сцене: средняя кнопка — орбита, Shift — сдвиг).
    const bool camButton = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
        m_dragging = true;
    if (!camButton) m_dragging = false;
    if (m_dragging) {
        const ImVec2 d = io.MouseDelta;
        if (io.KeyShift) stage.Pan(d.x, d.y);
        else stage.Orbit(-d.x * 0.4f, d.y * 0.4f);
    }
    // Выбор — щелчок левой без протяжки и не по ручке гизмо. Мимо костей — снять.
    if (hovered && !gizmoHot && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold * io.MouseDragThreshold && !m_gizmoWasUsing) {
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        if (stage.Skeletal()) {
            stage.SelectBone(PickBone(stage, viewProj, origin, size, mouse));
        } else {
            // Объект копии — ближайшее начало координат под курсором.
            Scene& scene = stage.StageScene();
            entt::entity best = entt::null;
            float bestDist = 14.0f;
            for (auto e : scene.Registry().view<Transform>()) {
                if (GameObject(&scene.Registry(), e).Name().rfind("##", 0) == 0) continue;   // пол
                glm::vec2 p;
                if (!ProjectPoint(viewProj, glm::vec3(scene.WorldMatrix(e)[3]), origin, size, p)) continue;
                const float d = glm::length(p - mouse);
                if (d < bestDist) { bestDist = d; best = e; }
            }
            stage.SelectEntity(best);
        }
    }
    // Клавиши — пока курсор над видом и не набирают текст.
    if ((hovered || ImGui::IsWindowFocused()) && !io.WantTextInput && !m_dragging) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_op = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_op = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_op = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) stage.FrameTarget();
    }

    if (stage.TargetId() < 0) {
        const char* hint = T("Choose a model or object in Animation Inspector > Target");
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f),
                                            IM_COL32(40, 40, 46, 255), hint);
    }
    ImGui::End();
}
