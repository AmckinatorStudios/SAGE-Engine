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

ViewportPanel::ViewportPanel() : m_gizmoOp((int)ImGuizmo::TRANSLATE) {}

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
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOp = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOp = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOp = (int)ImGuizmo::SCALE;
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
        auto op = (ImGuizmo::OPERATION)m_gizmoOp;
        float snapUnit = (op == ImGuizmo::ROTATE) ? snapR : (op == ImGuizmo::SCALE ? snapS : snapT);
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;

        if (ImGuizmo::Manipulate(glm::value_ptr(host.ViewMatrix()), glm::value_ptr(host.ProjMatrix()),
                                 op, ImGuizmo::LOCAL, glm::value_ptr(model),
                                 nullptr, m_snap ? snapValues : nullptr)) {
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

    // --- Оверлей-тулбар в углу вьюпорта: режим гизмо + snap + Play ---
    ImGui::SetNextWindowPos(ImVec2(imgPos.x + 8.0f, imgPos.y + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##viewport_toolbar", nullptr, overlayFlags)) {
        auto opButton = [this](const char* label, ImGuizmo::OPERATION op) {
            bool active = m_gizmoOp == (int)op;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
            if (ImGui::SmallButton(label)) m_gizmoOp = (int)op;
            if (active) ImGui::PopStyleColor();
        };
        opButton("Move (W)", ImGuizmo::TRANSLATE);
        ImGui::SameLine();
        opButton("Rotate (E)", ImGuizmo::ROTATE);
        ImGui::SameLine();
        opButton("Scale (R)", ImGuizmo::SCALE);
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_snap);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        EditorPlayState state = host.GetPlayState();
        if (state == EditorPlayState::Editing) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
            if (ImGui::SmallButton("Play")) host.StartPlay();
            ImGui::PopStyleColor();
        } else {
            if (state == EditorPlayState::Playing) {
                if (ImGui::SmallButton("Pause")) host.PausePlay();
            } else {
                if (ImGui::SmallButton("Resume")) host.ResumePlay();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
            if (ImGui::SmallButton("Stop")) host.StopPlay();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(state == EditorPlayState::Playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                                                 : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                               state == EditorPlayState::Playing ? "PLAYING" : "PAUSED");
        }
    }
    ImGui::End();

    ImGui::End(); // Viewport
    ImGui::PopStyleVar();
}
