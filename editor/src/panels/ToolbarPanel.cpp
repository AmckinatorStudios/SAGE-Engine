#include "ToolbarPanel.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"

namespace {

// Кнопка-переключатель: подсвечена акцентом, когда active.
bool ToggleButton(const char* label, bool active, const ImVec2& size = ImVec2(0, 0)) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
    bool pressed = ImGui::Button(label, size);
    if (active) ImGui::PopStyleColor();
    return pressed;
}

} // namespace

void ToolbarPanel::Draw(EditorHost& host, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    ImGui::BeginChild("##toolbar", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    // --- Слева: гизмо (режим / snap / пространство) ---
    if (ToggleButton("Move", host.GizmoOp() == (int)ImGuizmo::TRANSLATE)) host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ToggleButton("Rotate", host.GizmoOp() == (int)ImGuizmo::ROTATE)) host.GizmoOp() = (int)ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ToggleButton("Scale", host.GizmoOp() == (int)ImGuizmo::SCALE)) host.GizmoOp() = (int)ImGuizmo::SCALE;
    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox("Snap", &host.GizmoSnap());
    ImGui::SameLine();
    bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (ToggleButton(world ? "World" : "Local", true)) {
        host.GizmoSpace() = world ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
    }

    // --- По центру: Play / Pause / Stop ---
    // Ширина блока плей-контролов оценивается, чтобы отцентрировать его.
    float playBlockW = 220.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f + ImGui::GetCursorPosX() - playBlockW * 0.5f);
    EditorPlayState state = host.GetPlayState();
    if (state == EditorPlayState::Editing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
        if (ImGui::Button("Play")) host.StartPlay();
        ImGui::PopStyleColor();
    } else {
        if (state == EditorPlayState::Playing) {
            if (ImGui::Button("Pause")) host.PausePlay();
        } else {
            if (ImGui::Button("Resume")) host.ResumePlay();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Stop")) host.StopPlay();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(state == EditorPlayState::Playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                                             : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           state == EditorPlayState::Playing ? "PLAYING" : "PAUSED");
    }

    // --- Справа: режим рендера + сетка ---
    const char* modes[] = {"Shaded", "Wireframe", "Unlit", "Normals"};
    float rightW = 180.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightW);
    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox("Grid", &host.ShowGrid());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int mode = (int)host.RenderMode();
    if (ImGui::Combo("##rendermode", &mode, modes, IM_ARRAYSIZE(modes))) {
        host.RenderMode() = (EditorRenderMode)mode;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}
