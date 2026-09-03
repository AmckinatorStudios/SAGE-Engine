#include "ViewportPanel.h"

#include <algorithm>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"
#include "sage/render/DebugView.h"

// ---------------------------------------------------------------------------
// ИНСТРУМЕНТЫ ВЬЮПОРТА — виджет ПОВЕРХ картинки, а не полоса под меню.
//
// Раньше всё это жило в верхнем тулбаре редактора: гизмо, привязка,
// пространство, сетка, режим отрисовки, раскладка видов. Инструмент там стоял
// в двадцати сантиметрах от объекта, над которым работают: чтобы включить
// сетку или сменить шаг привязки, мышь ехала от объекта к верхнему краю окна и
// обратно. Заодно строка переполнялась — блоки Play и режима отрисовки
// наезжали друг на друга, стоило добавить кнопку.
//
// Теперь разделено по назначению: наверху (TopBarPanel) — приложение (Play и
// окна), здесь — работа с объектом. Виджет прижат к левому верхнему углу вида,
// сворачивается в одну строку и не мешает: пока курсор над ним, вьюпорт не
// ловит ни клик выбора, ни гизмо (см. ViewportPanel::Draw).
// ---------------------------------------------------------------------------

void ViewportPanel::DrawToolsOverlay(EditorHost& host, ImVec2 origin) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 10.0f));

    // Полупрозрачная подложка: виджет лежит НА сцене, и непрозрачная плашка
    // отрезала бы кусок кадра насовсем.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.88f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::BeginChild("##viewtools", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavFocus);

    auto sep = []() {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    };

    // --- Строка 1: чем двигают -----------------------------------------------
    if (EditorIcons::IconOnlyButton("move", T("Move (W)"),
                                    host.GizmoOp() == (int)ImGuizmo::TRANSLATE))
        host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("rotate", T("Rotate (E)"),
                                    host.GizmoOp() == (int)ImGuizmo::ROTATE))
        host.GizmoOp() = (int)ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("scale", T("Scale (R)"),
                                    host.GizmoOp() == (int)ImGuizmo::SCALE))
        host.GizmoOp() = (int)ImGuizmo::SCALE;
    ImGui::SameLine();
    // Универсальное гизмо: перенос, поворот и масштаб одновременно. Экономит
    // самое частое действие в редакторе — переключение режима ради одной правки.
    if (EditorIcons::IconOnlyButton("universal", T("All at once (T): move + rotate + scale"),
                                    host.GizmoOp() == (int)ImGuizmo::UNIVERSAL))
        host.GizmoOp() = (int)ImGuizmo::UNIVERSAL;
    ImGui::SameLine();
    // Рамка (Y): тянет ОДНУ грань, оставляя противоположную на месте — в
    // отличие от масштаба, который тянет от центра сразу в обе стороны.
    if (EditorIcons::IconOnlyButton("rect", T("Rect (Y): drag the faces of the bounding box"),
                                    host.GizmoOp() == (int)ImGuizmo::BOUNDS))
        host.GizmoOp() = (int)ImGuizmo::BOUNDS;
    ImGui::SameLine();
    // Коллайдер (C): гизмо тянет ФОРМУ СТОЛКНОВЕНИЯ, а не объект.
    if (EditorIcons::IconOnlyButton("physics", T("Collider (C): drag the collision shape"),
                                    host.ColliderEditMode()))
        host.ColliderEditMode() = !host.ColliderEditMode();

    sep();
    bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (EditorIcons::IconOnlyButton(world ? "grid" : "cube",
                                    world ? T("Gizmo axes: world") : T("Gizmo axes: object"),
                                    true)) {
        host.GizmoSpace() = world ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox(T("Snap"), &host.GizmoSnap());
    ImGui::SameLine();
    // Шаг привязки — того режима, который сейчас включён: три поля разом заняли
    // бы весь виджет, а нужно всегда ровно одно.
    {
        const auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float* step = (op == ImGuizmo::ROTATE)  ? &host.SnapRotate()
                      : (op == ImGuizmo::SCALE) ? &host.SnapScale()
                                                : &host.SnapMove();
        const char* fmt = (op == ImGuizmo::ROTATE) ? "%.0f°" : "%.2f";
        ImGui::BeginDisabled(!host.GizmoSnap());
        ImGui::SetNextItemWidth(64.0f);
        ImGui::DragFloat("##snapstep", step, 0.05f, 0.01f, 360.0f, fmt);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Snap step for the current mode.\n"
              "When building from blocks, set it to the block size."));
        }
    }

    sep();
    // Инструменты над выделением. Отключены, когда выделения нет: серая кнопка
    // честнее кнопки, которая молча ничего не делает.
    ImGui::BeginDisabled(host.Selection().empty());
    if (EditorIcons::IconOnlyButton("eye", T("Frame the selection (F)"))) host.FocusSelected();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("drop", T("Drop onto the surface (End)")))
        host.DropSelectedToSurface();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(host.Selection().size() < 2);
    if (EditorIcons::IconOnlyButton("align", T("Align the selection to the primary object")))
        ImGui::OpenPopup("##align_axis");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##align_axis")) {
        ImGui::TextDisabled("%s", T("Align to axis"));
        if (ImGui::MenuItem("X")) host.AlignSelection(0);
        if (ImGui::MenuItem("Y")) host.AlignSelection(1);
        if (ImGui::MenuItem("Z")) host.AlignSelection(2);
        ImGui::EndPopup();
    }

    // Сворачивание — последней кнопкой строки: вторая строка нужна реже первой,
    // а место во вьюпорте дороже места в тулбаре.
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(m_toolsExpanded ? "up" : "layout",
                                    m_toolsExpanded ? T("Collapse the tools")
                                                    : T("Expand the tools")))
        m_toolsExpanded = !m_toolsExpanded;

    // --- Строка 2: как показывают --------------------------------------------
    if (m_toolsExpanded) {
        if (EditorIcons::IconOnlyButton("grid", T("Viewport grid"), host.ShowGrid()))
            host.ShowGrid() = !host.ShowGrid();
        ImGui::SameLine();
        // Габариты выделенного — та самая коробка, по которой считается попадание
        // мышью. Включается тогда, когда непонятно, почему клик выбрал не то.
        if (EditorIcons::IconOnlyButton("wire", T("Bounds of the selection"), host.ShowBounds()))
            host.ShowBounds() = !host.ShowBounds();
        ImGui::SameLine();
        // Режим вёрстки интерфейса: UI показывается прямо во вьюпорте и правится
        // мышью. Раньше его было видно только в панели Game, где ничего не выделить.
        if (EditorIcons::IconOnlyButton("layout", T("UI layout mode (U)"), host.UIEditMode()))
            host.UIEditMode() = !host.UIEditMode();

        ImGui::SameLine();
        // Имена отладочных видов берутся из самого движка (DebugViewName), а не
        // переписываются здесь: разойдясь однажды, список начнёт врать о том,
        // что показывает шейдер.
        const char* modes[(int)EditorRenderMode::Count];
        modes[0] = T("Shaded");
        modes[1] = T("Wireframe");
        for (int i = 2; i < (int)EditorRenderMode::Count; ++i)
            modes[i] = sage::render::DebugViewName((sage::render::DebugView)(i - 1));
        ImGui::SetNextItemWidth(140.0f);
        int mode = (int)host.RenderMode();
        if (ImGui::Combo("##rendermode", &mode, modes, IM_ARRAYSIZE(modes)))
            host.RenderMode() = (EditorRenderMode)mode;

        sep();
        const char* layouts[] = {T("Single view"), T("Two columns"), T("Four views")};
        int layout = (int)m_layout;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("##layout", &layout, layouts, IM_ARRAYSIZE(layouts)))
            m_layout = (Layout)layout;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Viewport layout.\n"
              "Each view is a full scene pass,\n"
              "so extra views cost frames."));
        }

        // Вид АКТИВНОГО слота: перспектива или одна из ортогональных проекций.
        ImGui::SameLine();
        const char* kinds[] = {T("Perspective"), T("Top"), T("Front"), T("Side")};
        int kind = (int)m_kinds[m_activeSlot];
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("##kind", &kind, kinds, IM_ARRAYSIZE(kinds)))
            m_kinds[m_activeSlot] = (ViewKind)kind;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Projection of the active view"));

        ImGui::SameLine();
        if (ImGui::SmallButton(T("Show all"))) {
            // Вписываем сцену в ортогональные виды: без этого человек, отъехавший
            // колесом далеко, обратно уже не найдёт дорогу.
            for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
        }
    }

    // Рект виджета — для СЛЕДУЮЩЕГО кадра: по нему вьюпорт понимает, что мышь
    // на инструментах, и не отдаёт клик ни выбору, ни гизмо (ImGuizmo считает
    // попадание сам, по координатам мыши, и об окнах ImGui ничего не знает).
    m_toolsMin = ImGui::GetWindowPos();
    m_toolsMax = ImVec2(m_toolsMin.x + ImGui::GetWindowSize().x,
                        m_toolsMin.y + ImGui::GetWindowSize().y);

    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
}
