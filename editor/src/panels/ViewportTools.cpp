#include "ViewportPanel.h"

#include <algorithm>
#include <string>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"
#include "sage/render/DebugView.h"

// ---------------------------------------------------------------------------
// ИНСТРУМЕНТЫ ВЬЮПОРТА — одна короткая строка поверх картинки.
//
// ЧТО БЫЛО. В строке стояло полтора десятка одинаковых квадратиков подряд:
// четыре режима гизмо, рамка, коллайдер, пространство осей, привязка, шаг,
// кадрировать, посадить, выровнять, сетка, габариты, широкий список режимов
// отрисовки и раскладка видов — плюс шестерёнка, которой всё это включалось и
// выключалось. Набор без подписей и без порядка: чтобы найти нужное, строку
// приходилось читать целиком, наводя мышь на каждый значок ради подсказки.
// Настройка состава беде не помогала, а добавляла свою: чтобы убрать лишнее,
// надо было сперва понять, что из этого лишнее, — то есть решить ту же задачу,
// от которой бежали.
//
// ЧТО СТАЛО. Строка отвечает на четыре вопроса, и ровно в таком порядке:
//   1. ЧЕМ Я СЕЙЧАС РАБОТАЮ — выбор, перенос, поворот, масштаб;
//   2. В КАКИХ ОСЯХ — «Глобально» или «Локально», подписью, а не значком:
//      это не переключатель «вкл/выкл», у него два РАВНОПРАВНЫХ состояния, и
//      значок в таком случае всегда врёт — показывает либо то, что выбрано,
//      либо то, что будет, и угадать, что именно, нельзя;
//   3. ЧТО ПОМОГАЕТ — привязка к шагу и сетка;
//   4. КАК ПОКАЗЫВАТЬ СЦЕНУ — освещённо, каркасом, отладочным видом.
// Всё остальное — редкое (рамка, универсальное гизмо, коллайдер, посадка,
// выравнивание, габариты, раскладка видов) — лежит под «…» справа. Редкое не
// должно занимать место постоянно, но и прятаться навсегда не имеет права:
// одна кнопка на видном месте — это и есть «спрятано, но найдётся».
//
// Хоткеи работают независимо от строки: Q/W/E/R — инструменты, T/Y/C —
// редкие гизмо, F/End — кадрировать и посадить.
//
// Пока курсор над виджетом, вьюпорт не отдаёт ему ни клик выбора, ни гизмо
// (см. ViewportPanel::Draw).
// ---------------------------------------------------------------------------

namespace {

// Кнопка, открывающая список: значок, подпись и треугольник справа.
//
// Треугольник рисуется руками, а не берётся символом: шрифт редактора несёт
// латиницу с кириллицей, и «▾» в нём попросту нет — на экране получился бы
// пустой квадрат. Хвост из пробелов в подписи — это место под него.
bool DropdownButton(const char* icon, const char* label, const char* tooltip) {
    const std::string padded = std::string(label) + "    ";
    const bool pressed = EditorIcons::Button(icon, padded.c_str(), tooltip);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const float cx = b.x - 11.0f;
    const float cy = (a.y + b.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(cx - 4.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy - 2.0f), ImVec2(cx, cy + 3.0f),
        ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

} // namespace

// --- Оси: мира или объекта --------------------------------------------------
//
// ОБА ВАРИАНТА ПОДПИСАНЫ И ВИДНЫ СРАЗУ. Разница между ними — не тонкость: в
// осях мира стрелка «вправо» всегда вправо по сцене, а в осях объекта —
// туда, куда повёрнут он сам. Человек, который тянет повёрнутый предмет и не
// знает, в каком он режиме, видит просто «гизмо едет не туда».
void ViewportPanel::DrawSpaceMenu(EditorHost& host) {
    if (!ImGui::BeginPopup("##space_menu")) return;
    ImGui::TextDisabled("%s", T("Gizmo axes"));
    const bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (ImGui::MenuItem(T("Global"), nullptr, world)) host.GizmoSpace() = EditorGizmoSpace::World;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("— axes of the scene"));
    if (ImGui::MenuItem(T("Local"), nullptr, !world)) host.GizmoSpace() = EditorGizmoSpace::Local;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("— axes of the object itself"));
    ImGui::Separator();
    ImGui::TextDisabled("%s", T("Scale always works in the object's own axes"));
    ImGui::EndPopup();
}

// --- Чем показывать сцену ---------------------------------------------------
void ViewportPanel::DrawShadingMenu(EditorHost& host) {
    if (!ImGui::BeginPopup("##shading_menu")) return;
    ImGui::TextDisabled("%s", T("How to show the scene"));
    for (int i = 0; i < (int)EditorRenderMode::Count; ++i) {
        // Имена отладочных видов берутся из самого движка (DebugViewName), а не
        // переписываются здесь: разойдясь однажды, список начнёт врать о том,
        // что показывает шейдер.
        const char* name = (i == 0)   ? T("Shaded")
                           : (i == 1) ? T("Wireframe")
                                      : sage::render::DebugViewName((sage::render::DebugView)(i - 1));
        if (ImGui::MenuItem(name, nullptr, (int)host.RenderMode() == i))
            host.RenderMode() = (EditorRenderMode)i;
    }
    ImGui::EndPopup();
}

// --- Редкое: «…» ------------------------------------------------------------
void ViewportPanel::DrawMoreMenu(EditorHost& host) {
    if (!ImGui::BeginPopup("##more_menu")) return;

    ImGui::TextDisabled("%s", T("Rarer gizmos"));
    if (ImGui::MenuItem(T("All at once (T): move + rotate + scale"), nullptr,
                        host.GizmoOp() == (int)ImGuizmo::UNIVERSAL))
        host.GizmoOp() = (int)ImGuizmo::UNIVERSAL;
    // Рамка (Y): тянет ОДНУ грань, оставляя противоположную на месте — в
    // отличие от масштаба, который тянет от центра сразу в обе стороны.
    if (ImGui::MenuItem(T("Rect (Y): drag the faces of the bounding box"), nullptr,
                        host.GizmoOp() == (int)ImGuizmo::BOUNDS))
        host.GizmoOp() = (int)ImGuizmo::BOUNDS;
    // Коллайдер (C): гизмо тянет ФОРМУ СТОЛКНОВЕНИЯ, а не объект.
    if (ImGui::MenuItem(T("Collider (C): drag the collision shape"), nullptr,
                        host.ColliderEditMode()))
        host.ColliderEditMode() = !host.ColliderEditMode();

    ImGui::Separator();
    ImGui::TextDisabled("%s", T("Over the selection"));
    // Отключены, когда выделения нет: серый пункт честнее пункта, который молча
    // ничего не делает.
    ImGui::BeginDisabled(host.Selection().empty());
    if (ImGui::MenuItem(T("Frame the selection (F)"))) host.FocusSelected();
    if (ImGui::MenuItem(T("Drop onto the surface (End)"))) host.DropSelectedToSurface();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(host.Selection().size() < 2);
    if (ImGui::BeginMenu(T("Align to axis"))) {
        if (ImGui::MenuItem("X")) host.AlignSelection(0);
        if (ImGui::MenuItem("Y")) host.AlignSelection(1);
        if (ImGui::MenuItem("Z")) host.AlignSelection(2);
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    // Габариты выделенного — та самая коробка, по которой считается попадание
    // мышью. Включается тогда, когда непонятно, почему клик выбрал не то.
    if (ImGui::MenuItem(T("Bounds of the selection"), nullptr, host.ShowBounds()))
        host.ShowBounds() = !host.ShowBounds();

    ImGui::Separator();
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
    if (ImGui::MenuItem(T("Show all"))) {
        // Вписываем сцену в ортогональные виды: без этого человек, отъехавший
        // колесом далеко, обратно уже не найдёт дорогу.
        for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
    }
    ImGui::EndPopup();
}

void ViewportPanel::DrawToolsOverlay(EditorHost& host, ImVec2 origin) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 10.0f));

    // БЕЗ ПЛАШКИ: только кнопки.
    //
    // Подложка с рамкой рисовала поверх сцены серый прямоугольник — вторую
    // панель внутри панели, со своей границей и своим фоном. У каждой кнопки и
    // так есть собственная подложка (она и показывает, что это кнопка), а
    // общая поверх них ничего не добавляла, кроме отрезанного куска кадра в
    // самом рабочем углу. Само окно остаётся: им ловится мышь (кнопки — это
    // элементы ImGui, им нужен хозяин) и по нему считается «курсор на
    // инструментах».
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
    ImGui::BeginChild("##viewtools", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavFocus);

    // Разделитель между смысловыми группами — воздухом, а не палочкой: палочек
    // в строке из четырёх групп набирается столько же, сколько кнопок, и они
    // начинают читаться как кнопки.
    auto gap = [&]() { ImGui::SameLine(0.0f, 14.0f); };

    // --- 1. Чем работаем ----------------------------------------------------
    if (EditorIcons::IconOnlyButton("select", T("Select (Q): no gizmo, clicks pick objects"),
                                    host.GizmoOp() == EditorHost::kGizmoSelectOnly))
        host.GizmoOp() = EditorHost::kGizmoSelectOnly;
    ImGui::SameLine();
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

    // --- 2. В каких осях ----------------------------------------------------
    gap();
    const bool world = host.GizmoSpace() == EditorGizmoSpace::World;
    if (DropdownButton(world ? "world" : "cube", world ? T("Global") : T("Local"),
                       T("Which axes the gizmo works in")))
        ImGui::OpenPopup("##space_menu");
    DrawSpaceMenu(host);

    // --- 3. Что помогает ----------------------------------------------------
    gap();
    if (EditorIcons::IconOnlyButton("magnet", T("Snap to step"), host.GizmoSnap()))
        host.GizmoSnap() = !host.GizmoSnap();
    // Поле шага — ТОЛЬКО при включённой привязке. Серое неактивное поле занимает
    // столько же места, сколько рабочее, а сказать ему нечего.
    if (host.GizmoSnap()) {
        ImGui::SameLine();
        const auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float* step = (op == ImGuizmo::ROTATE)  ? &host.SnapRotate()
                      : (op == ImGuizmo::SCALE) ? &host.SnapScale()
                                                : &host.SnapMove();
        const char* fmt = (op == ImGuizmo::ROTATE) ? "%.0f°" : "%.2f";
        ImGui::SetNextItemWidth(56.0f);
        ImGui::DragFloat("##snapstep", step, 0.05f, 0.01f, 360.0f, fmt);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Snap step for the current mode.\n"
              "When building from blocks, set it to the block size."));
        }
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("grid", T("Viewport grid"), host.ShowGrid()))
        host.ShowGrid() = !host.ShowGrid();

    // --- 4. Как показывать сцену --------------------------------------------
    //
    // Список вместо широкого поля выбора: поле занимало треть строки ради
    // строки «Освещённо», которая девять раз из десяти и так не меняется.
    gap();
    if (EditorIcons::IconOnlyButton("sun", T("How to show the scene"),
                                    host.RenderMode() != EditorRenderMode::Shaded))
        ImGui::OpenPopup("##shading_menu");
    DrawShadingMenu(host);

    // --- Редкое -------------------------------------------------------------
    gap();
    if (EditorIcons::IconOnlyButton("dots", T("More tools")))
        ImGui::OpenPopup("##more_menu");
    DrawMoreMenu(host);

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
