#include "ViewportPanel.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "InspectorControls.h"
#include "ViewGizmo.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "panels/ViewportToolbarRules.h"
#include "sage/render/DebugView.h"

// ---------------------------------------------------------------------------
// ИНСТРУМЕНТЫ ВЬЮПОРТА — короткая строка поверх картинки.
//
// В СТРОКЕ — ТОЛЬКО ЧАСТОЕ, и каждая группа отвечает на свой вопрос:
//   1. ЧЕМ Я РАБОТАЮ — выбор, перенос, поворот, масштаб (Q/W/E/R);
//   2. В КАКИХ ОСЯХ — «Локально | Глобально» двумя подписанными сегментами:
//      оба состояния видны сразу, выбранное подсвечено (X переключает);
//   3. ПРИВЯЗКА — кнопка магнита (G; Ctrl — наоборот на время жеста), а поле
//      шагов «1 · 15° · 0.1» стоит рядом ТОЛЬКО пока привязка включена:
//      выключенной функции нечего показывать, и место отдаётся кадру. Шаги при
//      этом не теряются — они в настройках редактора (SaveSnap/LoadSnap);
//   4. ПОМОЩНИКИ — сетка и правка коллайдера (C). Правка коллайдера доступна,
//      только когда у выделенного есть форма с ручками (коробка, сфера,
//      капсула); иначе кнопка погашена и не обещает того, чего нет;
//   5. «…» — всё редкое: универсальное гизмо (T) и рамка (Y), показ
//      коллайдеров, вид отрисовки, операции над выделенным, раскладка видов,
//      проекция, информационный слой.
//
// Узкий вид — группы переносятся на следующую строку целиком (основные
// инструменты всегда первые), строка не вылезает за край и не заходит под
// гизмо ориентации. Пока курсор над строкой, вьюпорт не отдаёт клик ни выбору,
// ни гизмо (рект строки — m_toolsMin/m_toolsMax, см. ViewportPanel::Draw).
// ---------------------------------------------------------------------------

namespace ictl = sage::editor::ictl;
namespace vptools = sage::editor::vptools;

namespace {

// Ширина группы кнопок с подписями (как её посчитает ictl::ButtonGroup).
float TextGroupWidth(std::initializer_list<const char*> labels) {
    float seg = ImGui::GetFrameHeight();
    for (const char* l : labels)
        seg = std::max(seg, std::floor(ImGui::CalcTextSize(l).x + EditorTheme::KitMetrics().ButtonPadXCompact * 2.0f));
    return seg * (float)labels.size();
}

// Поле шагов привязки — та же ширина, что у кнопки kit с текстом и шевроном.
float SnapFieldWidth(const std::string& label) {
    const float h = ImGui::GetFrameHeight();
    return std::floor(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f +
                      h * EditorTheme::KitMetrics().ChevronIcon + EditorTheme::KitMetrics().TagGap);
}

} // namespace

// --- Привязка к шагу --------------------------------------------------------
//
// Шаг переноса — по каждой оси (плита 2×1×0.25 ставится своим шагом), поворот
// и масштаб — одним числом. Пределы и сохранение — прежние (SaveSnap).
void ViewportPanel::DrawSnapMenu(EditorHost& host) {
    // Окно — под полем шагов, а не там, где оказалась мышь.
    ImGui::SetNextWindowPos(m_snapAnchor, ImGuiCond_Appearing);
    const float popupW = 320.0f * Sage::UI::Scale();
    ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0.0f), ImVec2(popupW, FLT_MAX));
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("##snap_menu")) {
        DrawSnapSteps(host);
        ImGui::EndPopup();
    }
}

void ViewportPanel::DrawSnapSteps(EditorHost& host) {
    EditorTools& t = host.Tools();
    // G работает и при открытом окне: окно перекрывает вид, и хоткей вьюпорта
    // (он ждёт курсор над видом) сюда бы не дошёл.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G)) {
        t.Snap = !t.Snap;
        t.SaveSnap();
    }
    // Привязку выключили — шаги прячутся сразу.
    if (!t.Snap) {
        ImGui::CloseCurrentPopup();
        return;
    }
    // Поля шагов — настройки редактора, а не правка сцены: в историю отмены
    // они не пишутся.
    ictl::UndoScope noHistory([] {});
    bool changed = false;
    Sage::UI::MenuSection(T("Snap steps"), true);
    changed |= ictl::DragVec(nullptr, T("Move"), &t.SnapMove.x, 3, 0.05f, 0.001f, 1000.0f, "%.3g");
    if (ImGui::Button(T("Same as X"))) {
        t.SnapMove.y = t.SnapMove.z = t.SnapMove.x;
        changed = true;
    }
    ictl::NumOpts rot;
    rot.Step = 1.0f;
    rot.Min = 0.1f;
    rot.Max = 180.0f;
    rot.Format = "%.3g°";
    changed |= ictl::Number(host, T("Rotate"), &t.SnapRotate, rot);
    ictl::NumOpts sc;
    sc.Step = 0.01f;
    sc.Min = 0.001f;
    sc.Max = 10.0f;
    sc.Format = "%.3g";
    changed |= ictl::Number(host, T("Scale"), &t.SnapScale, sc);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    Sage::UI::TextSecondary("%s", T("G — on/off, Ctrl — invert while dragging. Collider handles use the move step"));
    ImGui::PopTextWrapPos();
    if (changed) {
        t.SnapMove = glm::clamp(t.SnapMove, glm::vec3(0.001f), glm::vec3(1000.0f));
        t.SnapRotate = std::clamp(t.SnapRotate, 0.1f, 180.0f);
        t.SnapScale = std::clamp(t.SnapScale, 0.001f, 10.0f);
        t.SaveSnap();
    }
}

// --- Редкое: «…» ------------------------------------------------------------
void ViewportPanel::DrawMoreMenu(EditorHost& host) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##more_menu")) return;
    EditorTools& tools = host.Tools();

    Sage::UI::MenuSection(T("Gizmos"), true);
    if (EditorIcons::MenuItemSelected("universal", T("Universal: move + rotate + scale"),
                                      tools.GizmoOp == (int)ImGuizmo::UNIVERSAL, "T"))
        tools.GizmoOp = (int)ImGuizmo::UNIVERSAL;
    if (EditorIcons::MenuItemSelected("rect", T("Bounds: drag the faces of the box"),
                                      tools.GizmoOp == (int)ImGuizmo::BOUNDS, "Y"))
        tools.GizmoOp = (int)ImGuizmo::BOUNDS;

    // ПОКАЗ коллайдеров — не их ПРАВКА: правка — кнопкой в строке (C), здесь —
    // как формы нарисованы. Непрозрачность заливки есть только у режимов с
    // заливкой: у «Линий» её нет вовсе, а не серым полем.
    if (EditorIcons::BeginMenu("physics", T("Collider display"))) {
        ColliderGizmoStyle& cg = tools.ColliderGizmo;
        bool changed = false;
        const char* modes[] = {T("Lines"), T("Solid (shaded)"), T("Solid + lines")};
        for (int i = 0; i < 3; ++i) {
            if (EditorIcons::MenuItemSelected("cube", modes[i], (int)cg.Draw == i)) {
                cg.Draw = (ColliderGizmoStyle::Mode)i;
                changed = true;
            }
        }
        if (cg.DrawFill()) {
            ImGui::SetNextItemWidth(160.0f * Sage::UI::Scale());
            ImGui::SliderFloat(T("Fill opacity"), &cg.FillOpacity, 0.05f, 0.9f, "%.2f");
            // Запись — по отпусканию: файл настроек не переписывается каждый кадр.
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        }
        if (EditorIcons::MenuItemToggle("eye", T("Only the selection"), &cg.OnlySelected)) changed = true;
        if (changed) cg.Save();
        ImGui::EndMenu();
    }

    // Вид отрисовки: освещённо, каркасом и отладочные виды движка (имена — из
    // самого движка, DebugViewName, чтобы список не врал о шейдере).
    if (EditorIcons::BeginMenu("sun", T("Shading"))) {
        for (int i = 0; i < (int)EditorRenderMode::Count; ++i) {
            const char* name = (i == 0)   ? T("Shaded")
                               : (i == 1) ? T("Wireframe")
                                          : sage::render::DebugViewName((sage::render::DebugView)(i - 1));
            if (i == 2) ImGui::Separator();
            if (EditorIcons::MenuItemSelected("cube", name, (int)tools.RenderMode == i))
                tools.RenderMode = (EditorRenderMode)i;
        }
        ImGui::EndMenu();
    }

    Sage::UI::MenuSection(T("Over the selection"));
    // Погашены, когда выделения нет: серый пункт честнее пункта, который молча
    // ничего не делает.
    ImGui::BeginDisabled(host.Selection().Empty());
    if (EditorIcons::MenuItem("focus", T("Frame the selection"), "F")) host.FocusSelected();
    if (EditorIcons::MenuItem("to-floor", T("Drop onto the surface"), "End")) host.DropSelectedToSurface();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(host.Selection().All().size() < 2);
    if (EditorIcons::BeginMenu("align", T("Align to axis"))) {
        if (EditorIcons::MenuItem("align-left", "X")) host.AlignSelection(0);
        if (EditorIcons::MenuItem("align-top", "Y")) host.AlignSelection(1);
        if (EditorIcons::MenuItem("align-center-x", "Z")) host.AlignSelection(2);
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    // Габариты выделенного — та коробка, по которой считается попадание мышью.
    if (EditorIcons::MenuItemToggle("rect", T("Bounds of the selection"), &tools.ShowBounds)) {}
    // Кости моделей со скелетом — видно, куда смотрит и как гнётся поза.
    if (EditorIcons::MenuItemToggle("ik", T("Skeleton of animated models"), &tools.ShowSkeleton)) {}

    Sage::UI::MenuSection(T("Views"));
    if (EditorIcons::BeginMenu("layout", T("View layout"))) {
        const char* layouts[] = {T("Single view"), T("Two columns"), T("Four views")};
        for (int i = 0; i < 3; ++i)
            if (EditorIcons::MenuItemSelected("layout", layouts[i], (int)m_layout == i)) m_layout = (Layout)i;
        ImGui::EndMenu();
    }
    if (EditorIcons::BeginMenu("camera", T("Projection of the active view"))) {
        const char* kinds[] = {T("Perspective"), T("Top"), T("Front"), T("Side")};
        for (int i = 0; i < 4; ++i)
            if (EditorIcons::MenuItemSelected("camera", kinds[i], (int)m_kinds[m_activeSlot] == i))
                m_kinds[m_activeSlot] = (ViewKind)i;
        ImGui::Separator();
        // Вписать сцену в ортогональные виды: отъехавший колесом далеко иначе
        // обратно дорогу не найдёт.
        if (EditorIcons::MenuItem("fit", T("Show all")))
            for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
        ImGui::EndMenu();
    }
    // Вернуть камеру к виду по умолчанию: улетевший далеко иначе ищет сцену.
    if (EditorIcons::MenuItem("camera", T("Reset camera"))) {
        ResetCamera(host.EditorCamera());
        m_kinds[m_activeSlot] = ViewKind::Perspective;
        for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
    }
    // Информация поверх вида — привычка человека: в настройках редактора.
    if (EditorIcons::BeginMenu("info", T("Viewport Overlays"))) {
        vptools::OverlayPrefs& o = OverlayPrefsRef();
        bool changed = EditorIcons::MenuItemToggle("info", T("Show Information Overlay"), &o.Enabled);
        ImGui::Separator();
        // Части слоя — только пока слой включён: при выключенном их галки
        // ничего бы не меняли.
        if (o.Enabled) {
            changed |= EditorIcons::MenuItemToggle("info", T("Show View Information"), &o.View);
            changed |= EditorIcons::MenuItemToggle("info", T("Show Selection Information"), &o.Selection);
            changed |= EditorIcons::MenuItemToggle("info", T("Show Camera Information"), &o.Camera);
            changed |= EditorIcons::MenuItemToggle("info", T("Show Performance Statistics"), &o.Stats);
        }
        if (changed) o.Save();
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void ViewportPanel::DrawToolsOverlay(EditorHost& host, ImVec2 origin, float cellWidth) {
    const float pad = Sage::UI::Get().SpacingSM;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + pad));
    // Окно без подложки — только кнопки: им ловится мышь, и по нему считается
    // «курсор на инструментах». Своя подложка у каждой группы — общая поверх
    // сцены только отрезала бы кусок кадра.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::KitMetrics().TagGap, Sage::UI::Get().SpacingXS));
    ImGui::BeginChild("##viewtools", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);

    EditorTools& t = host.Tools();
    const float h = ImGui::GetFrameHeight();
    const float inner = EditorTheme::KitMetrics().TagGap;          // внутри группы
    const float groupGap = Sage::UI::Get().SpacingMD;               // между группами
    // Свободная ширина: вид минус отступы и угол гизмо ориентации справа.
    const float maxWidth = std::max(h * 4.0f, cellWidth - pad * 2.0f - sage::editor::viewgizmo::Extent());

    // Правка коллайдера доступна, если у выделенного форма с ручками.
    const GameObject sel = host.SelectedObject();
    const bool colliderOk = sel.Valid() && vptools::ColliderEditAvailable(host.CurrentScene().Registry(), sel.Entity());
    m_colliderButtonEnabled = colliderOk;
    const std::string snapLabel = vptools::SnapLabel(t.SnapMove, t.SnapRotate, t.SnapScale);

    // Ширины групп — заранее: по ним решается перенос (vptools::FlowRows).
    const float spaceW = TextGroupWidth({T("Local"), T("Global")});
    const float snapW = h + (t.Snap ? inner + SnapFieldWidth(snapLabel) : 0.0f);
    const std::vector<float> widths = {h * 4.0f, spaceW, snapW, h * 3.0f + inner * 2.0f, h};
    const std::vector<int> rows = vptools::FlowRows(widths, groupGap, maxWidth);
    m_snapFieldShown = t.Snap;
    auto placeGroup = [&](size_t i) {
        if (i == 0) return;
        if (rows[i] == rows[i - 1]) ImGui::SameLine(0.0f, groupGap);
    };

    // --- 1. Чем работаем ----------------------------------------------------
    {
        ictl::GroupItem items[4];
        const char* icons[4] = {"select", "move", "rotate", "scale"};
        const int ops[4] = {EditorHost::kGizmoSelectOnly, (int)ImGuizmo::TRANSLATE, (int)ImGuizmo::ROTATE,
                            (int)ImGuizmo::SCALE};
        const char* tips[4] = {T("Select (Q): no gizmo, clicks pick objects"), T("Move (W): drag along the axes"),
                               T("Rotate (E): turn around the axes"), T("Scale (R): resize along the axes")};
        for (int i = 0; i < 4; ++i) {
            items[i].Icon = icons[i];
            items[i].Tooltip = tips[i];
            items[i].Active = t.GizmoOp == ops[i];
        }
        const int hit = ictl::ButtonGroup("##ops", items, 4, ictl::ButtonLook::IconOnly);
        if (hit >= 0) t.GizmoOp = ops[hit];
    }

    // --- 2. В каких осях: два подписанных сегмента ---------------------------
    placeGroup(1);
    {
        ictl::GroupItem items[2];
        items[0].Text = T("Local");
        items[0].Tooltip = T("Local (X): the gizmo follows the object's own axes");
        items[0].Active = t.GizmoSpace == EditorGizmoSpace::Local;
        items[1].Text = T("Global");
        items[1].Tooltip = T("Global (X): the gizmo follows the axes of the scene. Scale always uses the object's axes");
        items[1].Active = t.GizmoSpace == EditorGizmoSpace::World;
        const int hit = ictl::ButtonGroup("##space", items, 2, ictl::ButtonLook::Compact);
        if (hit >= 0) t.GizmoSpace = hit == 0 ? EditorGizmoSpace::Local : EditorGizmoSpace::World;
    }

    // --- 3. Привязка: кнопка и (только при включённой) поле шагов -----------
    placeGroup(2);
    if (ictl::Button("magnet", nullptr, T("Snap to step (G): moves, turns and scales jump by the step. "
                                          "Hold Ctrl to invert while dragging"),
                     ictl::ButtonLook::IconOnly, 0.0f, t.Snap)) {
        t.Snap = !t.Snap;
        t.SaveSnap();
    }
    if (t.Snap) {
        ImGui::SameLine(0.0f, inner);
        ImGui::SetNextItemWidth(SnapFieldWidth(snapLabel));
        m_snapAnchor = ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + h + inner);
        if (ictl::DropButton("##snap_steps", snapLabel.c_str(), ImGui::IsPopupOpen("##snap_menu"),
                             T("Snap steps: move per axis, rotation, scale")))
            ImGui::OpenPopup("##snap_menu");
    }
    DrawSnapMenu(host);

    // --- 4. Помощники: сетка и правка коллайдера ------------------------------
    placeGroup(3);
    if (ictl::Button("grid", nullptr, T("Grid: the floor grid of the viewport"), ictl::ButtonLook::IconOnly, 0.0f,
                     t.ShowGrid))
        t.ShowGrid = !t.ShowGrid;
    ImGui::SameLine(0.0f, inner);
    {
        // Погашена, пока у выделенного нет формы с ручками: тогда и правка,
        // и подсветка «включено» обещали бы то, чего нет.
        ictl::GroupItem it;
        it.Icon = "physics";
        it.Active = host.ColliderEditMode() && colliderOk;
        it.Enabled = colliderOk;
        it.Tooltip = colliderOk ? T("Collider Edit (C): drag the handles of the collision shape — the object itself "
                                    "does not move or scale")
                                : T("Collider Edit (C): select an object with a box, sphere or capsule collider");
        if (ictl::ButtonGroup("##collider_edit", &it, 1, ictl::ButtonLook::IconOnly) == 0)
            host.ColliderEditMode() = !host.ColliderEditMode();
    }
    ImGui::SameLine(0.0f, inner);
    {
        // Правка опоры: гизмо встаёт в опору и тянет только её. Повторное
        // нажатие возвращает обычное гизмо объекта.
        ictl::GroupItem it;
        it.Icon = "anchor-cc";
        it.Active = t.PivotEdit;
        it.Enabled = sel.Valid();
        it.Tooltip = T("Edit Pivot: drag the point the object rotates and scales around — the object itself "
                       "does not move");
        if (ictl::ButtonGroup("##pivot_edit", &it, 1, ictl::ButtonLook::IconOnly) == 0) t.PivotEdit = !t.PivotEdit;
    }

    // --- 5. Редкое ----------------------------------------------------------
    placeGroup(4);
    if (ictl::Button("dots", nullptr, T("More: rare gizmos, collider display, shading, views, overlays"),
                     ictl::ButtonLook::IconOnly))
        ImGui::OpenPopup("##more_menu");
    DrawMoreMenu(host);

    // Рект виджета — для СЛЕДУЮЩЕГО кадра: по нему вьюпорт понимает, что мышь
    // на инструментах, и не отдаёт клик ни выбору, ни гизмо (ImGuizmo считает
    // попадание сам, по координатам мыши, и об окнах ImGui ничего не знает).
    m_toolsMin = ImGui::GetWindowPos();
    m_toolsMax = ImVec2(m_toolsMin.x + ImGui::GetWindowSize().x, m_toolsMin.y + ImGui::GetWindowSize().y);

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}
