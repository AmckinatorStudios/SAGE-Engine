#include "ViewportPanel.h"

#include <algorithm>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../EditorPrefs.h"
#include "../Localization.h"
#include "sage/render/DebugView.h"

// ---------------------------------------------------------------------------
// ИНСТРУМЕНТЫ ВЬЮПОРТА — виджет ПОВЕРХ картинки, ОДНОЙ строкой.
//
// Раньше всё это жило в верхнем тулбаре редактора: гизмо, привязка,
// пространство, сетка, режим отрисовки, раскладка видов. Инструмент стоял в
// двадцати сантиметрах от объекта, над которым работают: чтобы включить сетку
// или сменить шаг привязки, мышь ехала от объекта к верхнему краю окна и
// обратно. Заодно строка переполнялась — блоки наезжали друг на друга, стоило
// добавить кнопку.
//
// Переехав во вьюпорт, инструменты сначала заняли две строки — и отрезали
// сверху полосу сцены в самом рабочем месте. Поэтому строка ОДНА, и держится
// она тремя приёмами:
//   • только иконки (подпись — в подсказке при наведении);
//   • поле шага показывается, лишь когда привязка включена;
//   • раскладка видов спрятана под одну кнопку с выпадающим списком.
// А что из этого показывать — решает человек: шестерёнка слева включает и
// выключает группы, и выбор запоминается между запусками. Нужное у всех
// разное, и «показывать всё» — это способ не выбирать за счёт того, кому нужна
// половина.
//
// Пока курсор над виджетом, вьюпорт не отдаёт ему ни клик выбора, ни гизмо
// (см. ViewportPanel::Draw).
// ---------------------------------------------------------------------------

const char* ViewportPanel::ToolGroupKey(ToolGroup g) {
    switch (g) {
        case ToolGroup::Gizmo:      return "viewport_tools.gizmo";
        case ToolGroup::Space:      return "viewport_tools.space";
        case ToolGroup::Snap:       return "viewport_tools.snap";
        case ToolGroup::Selection:  return "viewport_tools.selection";
        case ToolGroup::Display:    return "viewport_tools.display";
        case ToolGroup::RenderMode: return "viewport_tools.render_mode";
        case ToolGroup::Views:      return "viewport_tools.views";
        default:                    return "viewport_tools.unknown";
    }
}

const char* ViewportPanel::ToolGroupTitle(ToolGroup g) {
    // T() зовётся ЗДЕСЬ, а не в статической таблице: таблица собралась бы до
    // загрузки каталогов, и список навсегда остался бы английским.
    switch (g) {
        case ToolGroup::Gizmo:      return T("Gizmo: move, rotate, scale");
        case ToolGroup::Space:      return T("Gizmo axes: object or world");
        case ToolGroup::Snap:       return T("Snapping and its step");
        case ToolGroup::Selection:  return T("Frame, drop, align");
        case ToolGroup::Display:    return T("Grid and bounds of the selection");
        case ToolGroup::RenderMode: return T("Render mode");
        case ToolGroup::Views:      return T("View layout and projection");
        default:                    return "?";
    }
}

bool ViewportPanel::ToolGroupOn(ToolGroup g) {
    if (!m_toolGroupsLoaded) {
        m_toolGroupsLoaded = true;
        // По умолчанию включено ВСЁ: человек, который ещё ничего не настраивал,
        // должен увидеть весь набор и убрать лишнее, а не искать недостающее.
        for (int i = 0; i < (int)ToolGroup::Count; ++i)
            m_toolGroups[i] = sage::editor::prefs::GetBool(ToolGroupKey((ToolGroup)i), true);
    }
    return m_toolGroups[(int)g];
}

void ViewportPanel::SetToolGroupOn(ToolGroup g, bool on) {
    m_toolGroups[(int)g] = on;
    sage::editor::prefs::SetBool(ToolGroupKey(g), on);
}

void ViewportPanel::DrawToolsSettingsPopup() {
    if (!ImGui::BeginPopup("##viewtools_setup")) return;
    ImGui::TextDisabled("%s", T("What to show in the toolbar"));
    ImGui::Separator();
    for (int i = 0; i < (int)ToolGroup::Count; ++i) {
        const ToolGroup g = (ToolGroup)i;
        bool on = ToolGroupOn(g);
        if (ImGui::Checkbox(ToolGroupTitle(g), &on)) SetToolGroupOn(g, on);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Show everything"))) {
        for (int i = 0; i < (int)ToolGroup::Count; ++i) SetToolGroupOn((ToolGroup)i, true);
    }
    ImGui::TextDisabled("%s", T("Hotkeys keep working for hidden tools"));
    ImGui::EndPopup();
}

void ViewportPanel::DrawToolsOverlay(EditorHost& host, ImVec2 origin) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 10.0f));

    // Полупрозрачная подложка: виджет лежит НА сцене, и непрозрачная плашка
    // отрезала бы кусок кадра насовсем.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.88f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3, 4));
    ImGui::BeginChild("##viewtools", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavFocus);

    // Разделитель между группами. Ставится ПЕРЕД группой и только если слева
    // что-то уже нарисовано: иначе выключенная первая группа оставляла бы
    // висящую в воздухе палочку.
    bool anyDrawn = false;
    auto sep = [&]() {
        if (!anyDrawn) return;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    };

    // --- Шестерёнка: состав строки -------------------------------------------
    //
    // Первой и всегда, на постоянном месте: это единственная дорога назад к
    // выключенной группе, и она не имеет права переезжать вслед за составом.
    if (EditorIcons::IconOnlyButton("gear", T("What to show in the toolbar")))
        ImGui::OpenPopup("##viewtools_setup");
    DrawToolsSettingsPopup();
    anyDrawn = true;

    // --- Гизмо ---------------------------------------------------------------
    if (ToolGroupOn(ToolGroup::Gizmo)) {
        sep();
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
        anyDrawn = true;
    }

    // --- Пространство осей ----------------------------------------------------
    if (ToolGroupOn(ToolGroup::Space)) {
        sep();
        const bool world = host.GizmoSpace() == EditorGizmoSpace::World;
        if (EditorIcons::IconOnlyButton(world ? "grid" : "cube",
                                        world ? T("Gizmo axes: world") : T("Gizmo axes: object"),
                                        true)) {
            host.GizmoSpace() = world ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
        }
        anyDrawn = true;
    }

    // --- Привязка -------------------------------------------------------------
    if (ToolGroupOn(ToolGroup::Snap)) {
        sep();
        if (EditorIcons::IconOnlyButton("magnet", T("Snap to step"), host.GizmoSnap()))
            host.GizmoSnap() = !host.GizmoSnap();
        // Поле шага — ТОЛЬКО при включённой привязке. Серое неактивное поле
        // занимает столько же места, сколько рабочее, а сказать ему нечего.
        if (host.GizmoSnap()) {
            ImGui::SameLine();
            const auto op = (ImGuizmo::OPERATION)host.GizmoOp();
            float* step = (op == ImGuizmo::ROTATE)  ? &host.SnapRotate()
                          : (op == ImGuizmo::SCALE) ? &host.SnapScale()
                                                    : &host.SnapMove();
            const char* fmt = (op == ImGuizmo::ROTATE) ? "%.0f°" : "%.2f";
            ImGui::SetNextItemWidth(58.0f);
            ImGui::DragFloat("##snapstep", step, 0.05f, 0.01f, 360.0f, fmt);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Snap step for the current mode.\n"
                  "When building from blocks, set it to the block size."));
            }
        }
        anyDrawn = true;
    }

    // --- Инструменты над выделением -------------------------------------------
    if (ToolGroupOn(ToolGroup::Selection)) {
        sep();
        // Отключены, когда выделения нет: серая кнопка честнее кнопки, которая
        // молча ничего не делает.
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
        anyDrawn = true;
    }

    // --- Показ ---------------------------------------------------------------
    if (ToolGroupOn(ToolGroup::Display)) {
        sep();
        if (EditorIcons::IconOnlyButton("grid", T("Viewport grid"), host.ShowGrid()))
            host.ShowGrid() = !host.ShowGrid();
        ImGui::SameLine();
        // Габариты выделенного — та самая коробка, по которой считается попадание
        // мышью. Включается тогда, когда непонятно, почему клик выбрал не то.
        if (EditorIcons::IconOnlyButton("wire", T("Bounds of the selection"), host.ShowBounds()))
            host.ShowBounds() = !host.ShowBounds();
        anyDrawn = true;
    }

    // --- Режим отрисовки ------------------------------------------------------
    if (ToolGroupOn(ToolGroup::RenderMode)) {
        sep();
        // Имена отладочных видов берутся из самого движка (DebugViewName), а не
        // переписываются здесь: разойдясь однажды, список начнёт врать о том,
        // что показывает шейдер.
        const char* modes[(int)EditorRenderMode::Count];
        modes[0] = T("Shaded");
        modes[1] = T("Wireframe");
        for (int i = 2; i < (int)EditorRenderMode::Count; ++i)
            modes[i] = sage::render::DebugViewName((sage::render::DebugView)(i - 1));
        ImGui::SetNextItemWidth(124.0f);
        int mode = (int)host.RenderMode();
        if (ImGui::Combo("##rendermode", &mode, modes, IM_ARRAYSIZE(modes)))
            host.RenderMode() = (EditorRenderMode)mode;
        anyDrawn = true;
    }

    // --- Раскладка видов ------------------------------------------------------
    //
    // Под ОДНОЙ кнопкой: два выпадающих списка и кнопка «Показать всё» занимали
    // треть строки, а трогают их редко — раскладку выбирают раз и работают.
    if (ToolGroupOn(ToolGroup::Views)) {
        sep();
        const bool multi = m_layout != Layout::Single;
        if (EditorIcons::IconOnlyButton("layout", T("View layout and projection"), multi))
            ImGui::OpenPopup("##views_setup");
        if (ImGui::BeginPopup("##views_setup")) {
            ImGui::TextDisabled("%s", T("Views"));
            const char* layouts[] = {T("Single view"), T("Two columns"), T("Four views")};
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(layouts[i], nullptr, (int)m_layout == i)) m_layout = (Layout)i;
            ImGui::TextDisabled("%s", T("Each view is a full scene pass"));
            ImGui::Separator();
            ImGui::TextDisabled("%s", T("Projection of the active view"));
            const char* kinds[] = {T("Perspective"), T("Top"), T("Front"), T("Side")};
            for (int i = 0; i < 4; ++i)
                if (ImGui::MenuItem(kinds[i], nullptr, (int)m_kinds[m_activeSlot] == i))
                    m_kinds[m_activeSlot] = (ViewKind)i;
            ImGui::Separator();
            if (ImGui::MenuItem(T("Show all"))) {
                // Вписываем сцену в ортогональные виды: без этого человек,
                // отъехавший колесом далеко, обратно уже не найдёт дорогу.
                for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
            }
            ImGui::EndPopup();
        }
        anyDrawn = true;
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
