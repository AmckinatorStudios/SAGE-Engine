#include "UIToolsPanel.h"

#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"
#include "../UILayoutOps.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"

namespace {

// Кнопка выравнивания: рисунок вместо подписи.
//
// Подписью тут не обойтись: шесть кнопок «Left/Center/Right/Top/Middle/Bottom»
// занимают три строки и всё равно читаются медленнее, чем полоска у края
// квадратика. Рисуется так же, как иконки редактора (EditorIcons.h) — своими
// примитивами, без шрифта со значками.
bool AlignButton(const char* id, sage::ui::AlignEdge edge, const char* tip, bool enabled) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##align", ImVec2(h, h));
    if (!enabled) ImGui::EndDisabled();
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImU32 body = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float pad = std::floor(h * 0.22f);
    const float x0 = p.x + pad, x1 = p.x + h - pad;
    const float y0 = p.y + pad, y1 = p.y + h - pad;

    // Две «плашки» разной длины и линия, к которой они прижаты: именно так это
    // выглядит в любом редакторе, и узнаётся без подписи.
    const float t = std::max(2.0f, std::floor(h * 0.14f));
    switch (edge) {
        case sage::ui::AlignEdge::Left:
            dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y1), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0 + 2, y0 + 1), ImVec2(x1, y0 + 1 + t), body);
            dl->AddRectFilled(ImVec2(x0 + 2, y1 - 1 - t), ImVec2(x1 - 4, y1 - 1), body);
            break;
        case sage::ui::AlignEdge::CenterX: {
            const float cx = (x0 + x1) * 0.5f;
            dl->AddLine(ImVec2(cx, y0), ImVec2(cx, y1), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0, y0 + 1), ImVec2(x1, y0 + 1 + t), body);
            dl->AddRectFilled(ImVec2(x0 + 3, y1 - 1 - t), ImVec2(x1 - 3, y1 - 1), body);
            break;
        }
        case sage::ui::AlignEdge::Right:
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y1), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0, y0 + 1), ImVec2(x1 - 2, y0 + 1 + t), body);
            dl->AddRectFilled(ImVec2(x0 + 4, y1 - 1 - t), ImVec2(x1 - 2, y1 - 1), body);
            break;
        case sage::ui::AlignEdge::Top:
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0 + 1, y0 + 2), ImVec2(x0 + 1 + t, y1), body);
            dl->AddRectFilled(ImVec2(x1 - 1 - t, y0 + 2), ImVec2(x1 - 1, y1 - 4), body);
            break;
        case sage::ui::AlignEdge::CenterY: {
            const float cy = (y0 + y1) * 0.5f;
            dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0 + 1, y0), ImVec2(x0 + 1 + t, y1), body);
            dl->AddRectFilled(ImVec2(x1 - 1 - t, y0 + 3), ImVec2(x1 - 1, y1 - 3), body);
            break;
        }
        case sage::ui::AlignEdge::Bottom:
            dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), line, 1.5f);
            dl->AddRectFilled(ImVec2(x0 + 1, y0), ImVec2(x0 + 1 + t, y1 - 2), body);
            dl->AddRectFilled(ImVec2(x1 - 1 - t, y0 + 4), ImVec2(x1 - 1, y1 - 2), body);
            break;
    }
    return pressed && enabled;
}

// Подписи девяти клеток якоря — по строкам сверху вниз. Держатся рядом с самой
// решёткой: порядок в ней и порядок здесь обязаны совпадать, и увидеть это
// проще, когда оба списка на виду.
const char* kAnchorTips[9] = {
    "Top left", "Top center", "Top right",
    "Center left", "Center", "Center right",
    "Bottom left", "Bottom center", "Bottom right",
};

// Приглушённая подпись С ПЕРЕНОСОМ. Обычный TextDisabled не переносится, и
// панель, пришвартованная слева (а это её место по умолчанию), обрезала
// пояснения на полуслове — то есть ровно те строки, ради которых они написаны.
void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Сколько выделенных элементов интерфейса. От этого зависит, что вообще имеет
// смысл: выравнивать можно и один (по родителю), распределять — от трёх.
int SelectedUICount(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    int count = 0;
    for (int id : host.Selection()) {
        GameObject obj = scene.Get(id);
        if (obj.Valid() && reg.all_of<sage::ui::Transform>(obj.Entity())) ++count;
    }
    return count;
}

} // namespace

void UIToolsPanel::Draw(EditorHost& host, bool* open, bool focus) {
    if (focus) ImGui::SetNextWindowFocus();
    // Ключ «UI Layout», а не «Layout»: последний в каталоге уже занят
    // компонентом ui::Layout («Положение»), и панель инструментов получала его
    // перевод. Скрытый идентификатор окна остаётся прежним — от него зависит
    // швартовка (DockBuilderDockWindow) и запись в imgui.ini.
    if (!ImGui::Begin(T("UI Layout" "###Layout"), open)) {
        ImGui::End();
        return;
    }

    UIToolSettings& tools = host.UITools();

    // Режим вёрстки — первое, о чём надо сказать: без него ни сетки, ни рамок
    // во вьюпорте нет, и панель выглядит сломанной.
    if (!host.UIEditMode()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s",
                           T("Layout mode is off — the viewport shows no UI frames."));
        if (ImGui::Button(T("Turn on layout mode (U)"), ImVec2(-1.0f, 0.0f)))
            host.UIEditMode() = true;
        ImGui::Spacing();
    }

    // --- Вид холста ----------------------------------------------------------
    //
    // Первым разделом, а не последним: это ответ на два самых частых вопроса
    // новичка — «почему сквозь меню видно траву» и «куда делся элемент».
    ImGui::SeparatorText(T("Canvas view"));
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::SliderFloat(T("Backdrop"), &tools.Backdrop, 0.0f, 1.0f, "%.2f");
    Hint(T("1 — a flat backdrop instead of the 3D scene; 0 — design the HUD over the game."));

    ImGui::SetNextItemWidth(-90.0f);
    float zoomPercent = tools.Zoom * 100.0f;
    if (ImGui::SliderFloat(T("Zoom"), &zoomPercent, 15.0f, 300.0f, "%.0f%%"))
        tools.Zoom = zoomPercent / 100.0f;
    if (ImGui::Button(T("1:1 (Home)"))) { tools.Zoom = 1.0f; tools.Pan = glm::vec2(0.0f); }
    ImGui::SameLine();
    if (ImGui::Button(T("Bring back on screen"))) uiops::BringIntoView(host);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Pushes the selected elements back inside the screen."));
    Hint(T("Wheel zooms, middle button pans, Shift+F fits everything including what ran off."));

    // --- Сетка и привязки ----------------------------------------------------
    ImGui::SeparatorText(T("Grid and snapping"));
    ImGui::Checkbox(T("Show the grid"), &tools.ShowGrid);
    ImGui::Checkbox(T("Snap to it"), &tools.Snap.ToGrid);

    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat(T("Cell"), &tools.Snap.GridStep, 1.0f, 1.0f, 256.0f, "%.0f px");
    // Ходовые шаги рядом кнопками: 8 — самый частый шаг интерфейса, 16 и 32 —
    // под крупные отступы. Набирать их числом каждый раз незачем.
    // Отдельной строкой, а не рядом с полем: панель по умолчанию пришвартована
    // слева и узкая, и последняя кнопка обрезалась её краем.
    bool first = true;
    for (float step : {4.0f, 8.0f, 16.0f, 32.0f}) {
        char label[8];
        std::snprintf(label, sizeof(label), "%.0f px", step);
        ImGui::PushID((int)step);
        if (!first) ImGui::SameLine();
        first = false;
        if (ImGui::SmallButton(label)) tools.Snap.GridStep = step;
        ImGui::PopID();
    }

    ImGui::Checkbox(T("Snap to edges and centers"), &tools.Snap.ToEdges);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat(T("Snap radius"), &tools.Snap.Threshold, 0.5f, 1.0f, 40.0f, "%.0f px");
    Hint(T("Alt while dragging turns snapping off"));

    ImGui::Checkbox(T("Outlines of all elements"), &tools.ShowAllOutlines);
    ImGui::Checkbox(T("Distances to the parent"), &tools.ShowDistances);

    // --- Выравнивание --------------------------------------------------------
    const int count = SelectedUICount(host);
    ImGui::SeparatorText(T("Align"));
    if (count == 0) {
        Hint(T("Select an interface element."));
    } else {
        Hint(count == 1 ? T("One element — aligned to its parent")
                        : T("Aligned to the last clicked element"));
    }
    const bool canAlign = count >= 1;
    struct AlignDef { const char* Id; sage::ui::AlignEdge Edge; const char* Tip; };
    const AlignDef aligns[6] = {
        {"al", sage::ui::AlignEdge::Left, T("Left edges")},
        {"ac", sage::ui::AlignEdge::CenterX, T("Centers horizontally")},
        {"ar", sage::ui::AlignEdge::Right, T("Right edges")},
        {"at", sage::ui::AlignEdge::Top, T("Top edges")},
        {"am", sage::ui::AlignEdge::CenterY, T("Centers vertically")},
        {"ab", sage::ui::AlignEdge::Bottom, T("Bottom edges")},
    };
    for (int i = 0; i < 6; ++i) {
        if (i == 3) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        else if (i > 0) ImGui::SameLine();
        if (AlignButton(aligns[i].Id, aligns[i].Edge, aligns[i].Tip, canAlign))
            uiops::Align(host, aligns[i].Edge);
    }

    // --- Распределение -------------------------------------------------------
    //
    // Две операции на две оси — четыре кнопки. Подписи короткие, а объяснение
    // в подсказке: «равный шаг между центрами, по горизонтали» на кнопке
    // растягивает панель шире вьюпорта, ради которого она открыта.
    ImGui::SeparatorText(T("Distribute"));
    const bool canDistribute = count >= 3;
    if (!canDistribute) ImGui::BeginDisabled();
    struct Row { const char* Caption; bool Horizontal; };
    const Row rows[2] = {{T("Across"), true}, {T("Down"), false}};
    const float captionW = std::max(ImGui::CalcTextSize(rows[0].Caption).x,
                                    ImGui::CalcTextSize(rows[1].Caption).x) +
                           ImGui::GetStyle().ItemSpacing.x * 2.0f;
    for (const Row& row : rows) {
        ImGui::PushID(row.Horizontal ? "dh" : "dv");
        ImGui::TextDisabled("%s", row.Caption);
        ImGui::SameLine(captionW);
        if (ImGui::Button(T("Gaps"))) uiops::Distribute(host, row.Horizontal, false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Equal gaps between neighbours; the outer ones stay put"));
        ImGui::SameLine();
        if (ImGui::Button(T("Centers"))) uiops::Distribute(host, row.Horizontal, true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Equal step between centers — for rows of unequal widths"));
        ImGui::PopID();
    }
    if (!canDistribute) ImGui::EndDisabled();
    if (!canDistribute) Hint(T("Needs three elements or more."));

    // --- Якорь ---------------------------------------------------------------
    ImGui::SeparatorText(T("Anchor point"));
    Hint(T("Where the element holds on when the screen changes size."));
    if (count == 0) ImGui::BeginDisabled();
    {
        // Девять клеток вместо выпадающего списка из девяти слов: якорь — это
        // и есть решётка три на три, и выбирается он взглядом.
        UIAnchor current = UIAnchor::TopLeft;
        Scene& scene = host.CurrentScene();
        GameObject primary = scene.Get(host.SelectedId());
        if (primary.Valid()) {
            if (const sage::ui::Transform* u =
                    scene.Registry().try_get<sage::ui::Transform>(primary.Entity()))
                current = u->Anchor;
        }
        const UIAnchor grid[9] = {
            UIAnchor::TopLeft,    UIAnchor::TopCenter,    UIAnchor::TopRight,
            UIAnchor::CenterLeft, UIAnchor::Center,       UIAnchor::CenterRight,
            UIAnchor::BottomLeft, UIAnchor::BottomCenter, UIAnchor::BottomRight,
        };
        const float cell = ImGui::GetFrameHeight();
        for (int i = 0; i < 9; ++i) {
            if (i % 3 != 0) ImGui::SameLine();
            ImGui::PushID(1000 + i);
            const bool active = grid[i] == current;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##anchor", ImVec2(cell, cell)))
                uiops::SetAnchorKeepingPlace(host, grid[i]);
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(kAnchorTips[i]));
            ImGui::PopID();

            // Точка НА МЕСТЕ ЯКОРЯ внутри клетки: без неё девять одинаковых
            // квадратов не говорят вообще ничего, и решётка читается как
            // девять пустых кнопок.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float inset = std::floor(cell * 0.24f);
            const float cx = p.x + inset + (cell - inset * 2.0f) * (float)(i % 3) * 0.5f;
            const float cy = p.y + inset + (cell - inset * 2.0f) * (float)(i / 3) * 0.5f;
            dl->AddRect(ImVec2(p.x + inset, p.y + inset), ImVec2(p.x + cell - inset,
                                                                p.y + cell - inset),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.0f, 0, 1.0f);
            dl->AddCircleFilled(ImVec2(cx, cy), std::max(2.0f, cell * 0.10f),
                                ImGui::GetColorU32(ImGuiCol_Text), 8);
        }
        Hint(T("The element stays where it is — only Offset is recomputed."));
    }
    if (count == 0) ImGui::EndDisabled();

    // --- Быстрые действия ----------------------------------------------------
    ImGui::SeparatorText(T("Quick actions"));
    if (count == 0) ImGui::BeginDisabled();
    if (ImGui::Button(T("Fill the parent"), ImVec2(-1.0f, 0.0f)))
        uiops::StretchToParent(host, 0.0f);
    if (ImGui::Button(T("Fill with a margin"), ImVec2(-1.0f, 0.0f)))
        uiops::StretchToParent(host, 16.0f);
    if (ImGui::Button(T("Round to the grid"), ImVec2(-1.0f, 0.0f)))
        uiops::SnapSelectionToGrid(host);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Tidies up a screen that was built before snapping was on."));
    if (count == 0) ImGui::EndDisabled();

    ImGui::End();
}
