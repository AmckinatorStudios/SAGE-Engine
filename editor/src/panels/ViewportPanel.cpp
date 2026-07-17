#include "ViewportPanel.h"

#include <cstdint>
#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "sage/core/Application.h"
#include "sage/scene/Components.h"

namespace {

// Раскладывает мировую матрицу обратно в Transform (Position/Rotation/Scale).
// ВАЖНО: порядок углов должен совпадать с Transform::GetMatrix (T*Rx*Ry*Rz*S),
// поэтому используется glm::extractEulerAngleXYZ, а не декомпозиция ImGuizmo
// (у неё другой порядок осей — гизмо «прыгал» бы на повёрнутых объектах).
void DecomposeToTransform(const glm::mat4& m, Transform& out) {
    out.Position = glm::vec3(m[3]);

    glm::vec3 scale(glm::length(glm::vec3(m[0])),
                    glm::length(glm::vec3(m[1])),
                    glm::length(glm::vec3(m[2])));
    scale = glm::max(scale, glm::vec3(1e-6f)); // защита от вырожденного масштаба
    out.Scale = scale;

    glm::mat4 rot(1.0f);
    rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
    rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
    rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);

    float rx, ry, rz;
    glm::extractEulerAngleXYZ(rot, rx, ry, rz);
    out.Rotation = glm::degrees(glm::vec3(rx, ry, rz));
}

} // namespace

void ViewportPanel::Draw(EditorHost& host) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    bool hovered = ImGui::IsWindowHovered();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x >= 8 && avail.y >= 8) host.SetViewportSize((int)avail.x, (int)avail.y);
    ImVec2 imgPos = ImGui::GetCursorScreenPos();

    // Текстура OpenGL идёт снизу-вверх — переворот по V.
    ImTextureID tex = (ImTextureID)(std::intptr_t)host.SceneTexture();
    ImGui::Image(tex, avail, ImVec2(0, 1), ImVec2(1, 0));

    ImGuiIO& io = ImGui::GetIO();
    float dt = sage::Application::Get().DeltaTime();
    Camera& camera = host.EditorCamera();

    // --- Камера: ПКМ — осмотр, ПКМ+WASDQE — полёт, Shift — ускорение ---
    bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if ((hovered || m_cameraDriving) && rmb) {
        m_cameraDriving = true;
        camera.ProcessMouse(io.MouseDelta.x, -io.MouseDelta.y);
        float speed = camera.MovementSpeed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) camera.Position += camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) camera.Position -= camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) camera.Position -= camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) camera.Position += camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) camera.Position += camera.WorldUp * speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) camera.Position -= camera.WorldUp * speed;
    } else {
        m_cameraDriving = false;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        camera.Position += camera.Front * io.MouseWheel * 0.8f;
    }

    // --- Хоткеи гизмо (не во время полёта камеры и не в полях ввода) ---
    if (hovered && !m_cameraDriving && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) host.GizmoOp() = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) host.GizmoOp() = (int)ImGuizmo::SCALE;
    }

    // --- ImGuizmo: манипулятор выбранной сущности (сетка — DebugDraw в FBO) ---
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgPos.x, imgPos.y, avail.x, avail.y);

    GameObject selected = host.SelectedObject();
    if (selected.Valid()) {
        Transform& tr = selected.GetTransform();
        glm::mat4 model = tr.GetMatrix();

        // Пока гизмо не тащат, но курсор над ним — запоминаем состояние «до»:
        // первый же кадр перетаскивания уже мутирует Transform, поэтому снапшот
        // должен быть взят раньше него.
        if (!ImGuizmo::IsUsing() && ImGuizmo::IsOver() && !host.InPlayMode()) {
            host.CapturePendingSnapshot();
        }

        float snapT = 0.5f, snapR = 15.0f, snapS = 0.1f;
        float snapValues[3];
        auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float snapUnit = (op == ImGuizmo::ROTATE) ? snapR : (op == ImGuizmo::SCALE ? snapS : snapT);
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;

        // Scale всегда в локальном пространстве (ImGuizmo игнорит WORLD для scale);
        // Move/Rotate — по выбору пользователя (тулбар: Local/World).
        auto mode = (host.GizmoSpace() == EditorGizmoSpace::World && op != ImGuizmo::SCALE)
                        ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        if (ImGuizmo::Manipulate(glm::value_ptr(host.ViewMatrix()), glm::value_ptr(host.ProjMatrix()),
                                 op, mode, glm::value_ptr(model),
                                 nullptr, host.GizmoSnap() ? snapValues : nullptr)) {
            DecomposeToTransform(model, tr);
        }

        // Фронт «начали таскать»: одна запись undo на всё перетаскивание.
        bool usingNow = ImGuizmo::IsUsing();
        if (usingNow && !m_gizmoWasUsing && !host.InPlayMode()) {
            host.CommitPendingSnapshot();
        }
        m_gizmoWasUsing = usingNow;
    } else {
        m_gizmoWasUsing = false;
    }

    // --- Пикинг ЛКМ (не по гизмо и не во время манипуляции) ---
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        ImVec2 mp = ImGui::GetMousePos();
        float u = (mp.x - imgPos.x) / avail.x;
        float v = (mp.y - imgPos.y) / avail.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) host.PickAtViewport(u, v);
    }

    // Инструменты (режим гизмо, snap, пространство, Play, режим рендера) —
    // в верхнем тулбаре редактора (ToolbarPanel), не оверлеем во вьюпорте.

    ImGui::End(); // Viewport
    ImGui::PopStyleVar();
}
