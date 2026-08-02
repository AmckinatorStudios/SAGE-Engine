#include "ToolbarPanel.h"

#include <algorithm>

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
    // Универсальное гизмо: перенос, поворот и масштаб одновременно. Экономит
    // самое частое действие в редакторе — переключение режима ради одной правки.
    if (EditorIcons::IconOnlyButton("universal", "Всё сразу (T): перенос + поворот + масштаб",
                                    host.GizmoOp() == (int)ImGuizmo::UNIVERSAL))
        host.GizmoOp() = (int)ImGuizmo::UNIVERSAL;
    ImGui::SameLine();
    // Рамка (Y): тянет ОДНУ грань, оставляя противоположную на месте — в
    // отличие от масштаба, который тянет от центра сразу в обе стороны.
    if (EditorIcons::IconOnlyButton("rect", "Рамка (Y): тянуть за грани габаритной коробки",
                                    host.GizmoOp() == (int)ImGuizmo::BOUNDS))
        host.GizmoOp() = (int)ImGuizmo::BOUNDS;
    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox("Snap", &host.GizmoSnap());
    ImGui::SameLine();
    // Шаг привязки — того режима, который сейчас включён: три поля разом
    // занимали бы полтулбара, а нужно всегда ровно одно.
    {
        const auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float* step = (op == ImGuizmo::ROTATE)  ? &host.SnapRotate()
                      : (op == ImGuizmo::SCALE) ? &host.SnapScale()
                                                : &host.SnapMove();
        const char* fmt = (op == ImGuizmo::ROTATE) ? "%.0f°" : "%.2f";
        ImGui::BeginDisabled(!host.GizmoSnap());
        ImGui::SetNextItemWidth(70.0f);
        ImGui::DragFloat("##snapstep", step, 0.05f, 0.01f, 360.0f, fmt);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Шаг привязки для текущего режима.\n"
                              "Для постройки из блоков ставьте его равным размеру блока.");
        }
    }
    ImGui::SameLine();
    bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (EditorIcons::Button(world ? "grid" : "cube", world ? "World" : "Local",
                            "Пространство гизмо: мировое или локальное", true)) {
        host.GizmoSpace() = world ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
    }
    ImGui::SameLine();
    // Инструменты над выделением. Отключены, когда выделения нет: серая кнопка
    // честнее кнопки, которая молча ничего не делает.
    ImGui::BeginDisabled(host.Selection().empty());
    if (EditorIcons::IconOnlyButton("eye", "Показать в кадре (F)")) host.FocusSelected();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("drop", "Опустить на поверхность (End)"))
        host.DropSelectedToSurface();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(host.Selection().size() < 2);
    if (EditorIcons::IconOnlyButton("align", "Выровнять выделенные по первичному"))
        ImGui::OpenPopup("##align_axis");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##align_axis")) {
        ImGui::TextDisabled("Выровнять по оси");
        if (ImGui::MenuItem("X")) host.AlignSelection(0);
        if (ImGui::MenuItem("Y")) host.AlignSelection(1);
        if (ImGui::MenuItem("Z")) host.AlignSelection(2);
        ImGui::EndPopup();
    }

    // --- По центру: Play / Pause / Stop ---
    //
    // Центрирование считается ОТ ОКНА и зажимается между левым и правым блоками.
    //
    // Раньше здесь было `GetContentRegionAvail().x * 0.5f + GetCursorPosX()` —
    // то есть середина ОСТАВШЕГОСЯ места. Пока слева было три кнопки, это
    // выглядело как центр; стоило добавить инструменты, и требуемая позиция
    // уехала левее текущего курсора. ImGui::SameLine(x) в таком случае честно
    // ставит курсор назад, и блок Play рисуется ПОВЕРХ кнопок гизмо — они
    // просто исчезали с экрана.
    const float leftEnd = ImGui::GetCursorPosX();
    const float playBlockW = 220.0f;
    const float rightBlockW = 250.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float windowW = ImGui::GetWindowWidth();
    float playX = windowW * 0.5f - playBlockW * 0.5f;
    // Зажим: центр уступает и левому блоку, и правому. Если места нет вовсе,
    // блок просто идёт следом за левым — лучше несимметрично, чем внахлёст.
    const float playMax = windowW - rightBlockW - playBlockW - spacing;
    if (playX > playMax) playX = playMax;
    if (playX < leftEnd + spacing) playX = leftEnd + spacing;
    ImGui::SameLine(playX);
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
    // Та же защита: правый блок не заезжает на то, что уже нарисовано.
    const float rightX = std::max(ImGui::GetCursorPosX() + spacing, windowW - rightBlockW);
    ImGui::SameLine(rightX);
    if (EditorIcons::IconOnlyButton("grid", "Сетка вьюпорта", host.ShowGrid()))
        host.ShowGrid() = !host.ShowGrid();
    ImGui::SameLine();
    // Габариты выделенного — та самая коробка, по которой считается попадание
    // мышью. Включается тогда, когда непонятно, почему клик выбрал не то.
    if (EditorIcons::IconOnlyButton("wire", "Габариты выделенного", host.ShowBounds()))
        host.ShowBounds() = !host.ShowBounds();
    ImGui::SameLine();
    // Режим вёрстки интерфейса: UI показывается прямо во вьюпорте и правится
    // мышью. Раньше его было видно только в панели Game, где ничего не выделить.
    if (EditorIcons::IconOnlyButton("layout", "Режим вёрстки интерфейса (U)", host.UIEditMode()))
        host.UIEditMode() = !host.UIEditMode();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int mode = (int)host.RenderMode();
    if (ImGui::Combo("##rendermode", &mode, modes, IM_ARRAYSIZE(modes))) {
        host.RenderMode() = (EditorRenderMode)mode;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}
