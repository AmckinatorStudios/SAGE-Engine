#include "ToolbarPanel.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "EditorIcons.h"

void ToolbarPanel::Draw(EditorHost& host, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    ImGui::BeginChild("##toolbar", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    // --- Слева: гизмо (режим / snap / пространство) ---
    // Только иконки: три подписанные кнопки съедали треть тулбара, а форма
    // стрелки, дуги и рамки читается быстрее слова. Горячая клавиша — в
    // подсказке, чтобы её не нужно было искать в меню.
    if (EditorIcons::IconOnlyButton("move", "Перенос (W)",
                                    host.GizmoOp() == (int)ImGuizmo::TRANSLATE))
        host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("rotate", "Поворот (E)",
                                    host.GizmoOp() == (int)ImGuizmo::ROTATE))
        host.GizmoOp() = (int)ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("scale", "Масштаб (R)",
                                    host.GizmoOp() == (int)ImGuizmo::SCALE))
        host.GizmoOp() = (int)ImGuizmo::SCALE;
    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox("Snap", &host.GizmoSnap());
    ImGui::SameLine();
    bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (EditorIcons::Button(world ? "grid" : "cube", world ? "World" : "Local",
                            "Пространство гизмо: мировое или локальное", true)) {
        host.GizmoSpace() = world ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
    }

    // --- По центру: Play / Pause / Stop ---
    // Ширина блока плей-контролов оценивается, чтобы отцентрировать его.
    float playBlockW = 220.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f + ImGui::GetCursorPosX() - playBlockW * 0.5f);
    EditorPlayState state = host.GetPlayState();
    if (state == EditorPlayState::Editing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
        if (EditorIcons::Button("play", "Play", "Запустить сцену (сцена будет восстановлена по Stop)"))
            host.StartPlay();
        ImGui::PopStyleColor();
    } else {
        if (state == EditorPlayState::Playing) {
            if (EditorIcons::Button("pause", "Pause", "Приостановить")) host.PausePlay();
        } else {
            if (EditorIcons::Button("play", "Resume", "Продолжить")) host.ResumePlay();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (EditorIcons::Button("stop", "Stop", "Остановить и вернуть сцену как была"))
            host.StopPlay();
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
    if (EditorIcons::IconOnlyButton("grid", "Сетка вьюпорта", host.ShowGrid()))
        host.ShowGrid() = !host.ShowGrid();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int mode = (int)host.RenderMode();
    if (ImGui::Combo("##rendermode", &mode, modes, IM_ARRAYSIZE(modes))) {
        host.RenderMode() = (EditorRenderMode)mode;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}
