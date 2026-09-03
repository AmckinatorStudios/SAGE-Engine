#include "UIEditorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"
#include "../UIElementProperties.h"
#include "../Project.h"
#include "../UILayoutOps.h"
#include "sage/core/Config.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"

namespace {

namespace ui = sage::ui;

// Приглушённое пояснение с переносом: колонки узкие, а обычный TextDisabled не
// переносит и обрезает строку посередине слова.
void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

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

// Сколько выделенных элементов интерфейса: от этого зависит, что имеет смысл.
int SelectedUICount(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    int count = 0;
    for (int id : host.Selection()) {
        GameObject obj = scene.Get(id);
        if (obj.Valid() && reg.all_of<ui::Transform>(obj.Entity())) ++count;
    }
    return count;
}

// Разрешение, в котором интерфейс увидит игрок.
//
// Берётся из настроек ИГРЫ, а не из размера панели: под него считаются якоря,
// растяжения и проценты, и верстать в размер окна редактора значило бы верстать
// под экран, которого у игрока нет.
void GameFrameSize(EditorHost& host, int& outW, int& outH) {
    const sage::EngineConfig& cfg = host.Settings();
    outW = std::max(64, cfg.Width);
    outH = std::max(64, cfg.Height);
}

} // namespace

// ============================================================================
//  Верхняя строка: что относится к самому холсту
// ============================================================================
void UIEditorPanel::DrawTopBar(EditorHost& host) {
    UIToolSettings& tools = host.UITools();

    DrawCreateMenu(host);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Масштаб показа. Проценты, а не доли: «120%» читается сразу, «1.2» надо
    // домножать в уме.
    ImGui::SetNextItemWidth(110.0f);
    float zoomPercent = m_zoom * 100.0f;
    if (ImGui::DragFloat("##zoom", &zoomPercent, 1.0f, 10.0f, 400.0f, "%.0f%%"))
        m_zoom = std::clamp(zoomPercent / 100.0f, 0.10f, 4.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Canvas zoom (wheel over the canvas)"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("1:1"))) { m_zoom = 1.0f; m_pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Fit"))) { m_fitOnce = true; m_pan = ImVec2(0, 0); }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Фон под интерфейсом: сцена или ровная заливка. Ползунком, а не галкой,
    // потому что худ правят ПОВЕРХ игры — там нужна полупрозрачная плёнка.
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat(T("Backdrop"), &tools.Backdrop, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("1 — a flat backdrop instead of the game frame;\n"
                                  "0 — design the HUD over the game."));
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (EditorIcons::IconOnlyButton("grid", T("Show the grid"), tools.ShowGrid))
        tools.ShowGrid = !tools.ShowGrid;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("magnet", T("Snap to the grid"), tools.Snap.ToGrid))
        tools.Snap.ToGrid = !tools.Snap.ToGrid;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(84.0f);
    ImGui::DragFloat("##cell", &tools.Snap.GridStep, 1.0f, 1.0f, 256.0f, "%.0f px");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Grid cell"));
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("align", T("Snap to edges and centers of neighbours"),
                                    tools.Snap.ToEdges))
        tools.Snap.ToEdges = !tools.Snap.ToEdges;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("wire", T("Outlines of all elements"), tools.ShowAllOutlines))
        tools.ShowAllOutlines = !tools.ShowAllOutlines;

    // Справа — разрешение, в котором всё это увидит игрок. Не украшение:
    // именно от него считается вся раскладка, и человек должен видеть, подо
    // что верстает.
    int gw = 0, gh = 0;
    GameFrameSize(host, gw, gh);
    char res[64];
    std::snprintf(res, sizeof(res), "%d x %d", gw, gh);
    const float w = ImGui::CalcTextSize(res).x + 90.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowWidth() - w));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s %s", T("Screen:"), res);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Game resolution from Game Settings.\n"
                                  "The layout is computed in it, so this is exactly\n"
                                  "what the player will see."));
    }
}

// ============================================================================
//  Создание элементов
// ============================================================================
void UIEditorPanel::DrawCreateMenu(EditorHost& host) {
    // Кнопка с выпадающим списком заготовок. Заготовки — движковые
    // (sage::ui::PresetNames): что такое «кнопка», знает движок, а не редактор.
    if (EditorIcons::Button("plus", T("Element"), T("Add an interface element"))) {
        ImGui::OpenPopup("##ui_create");
    }
    if (ImGui::BeginPopup("##ui_create")) {
        ImGui::TextDisabled("%s", T("New element"));
        ImGui::Separator();
        for (const std::string& name : ui::PresetNames()) {
            if (ImGui::MenuItem(name.c_str())) {
                host.PushUndoSnapshot();
                GameObject created = host.CreateUIEntity(name);
                if (created.Valid()) host.SetSelectedId(created.Id());
            }
        }
        ImGui::Separator();
        Hint(T("It becomes a child of the selected element."));
        ImGui::EndPopup();
    }
}

// ============================================================================
//  Дерево элементов
// ============================================================================
void UIEditorPanel::DrawTreeNode(EditorHost& host, Scene& scene, int id, int depth) {
    GameObject obj = scene.Get(id);
    if (!obj.Valid()) return;
    entt::registry& reg = scene.Registry();
    const entt::entity e = obj.Entity();
    ui::Transform* xf = reg.try_get<ui::Transform>(e);
    if (!xf) return;

    ImGui::PushID(id);

    // Глазок — ПЕРВЫМ, а не в свойствах: спрятать мешающую панель, чтобы
    // добраться до того, что под ней, — самое частое действие в вёрстке.
    const bool wasVisible = xf->Visible;
    if (EditorIcons::IconOnlyButton("eye", wasVisible ? T("Hide") : T("Show"), wasVisible)) {
        host.PushUndoSnapshot();
        xf->Visible = !wasVisible;
    }
    ImGui::SameLine();

    // Дети берутся из HierarchyComponent: у элемента без детей его просто нет.
    std::vector<entt::entity> children;
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) children = h->Children;
    bool hasUiChildren = false;
    for (entt::entity c : children)
        if (reg.all_of<ui::Transform>(c)) { hasUiChildren = true; break; }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (!hasUiChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (host.IsSelected(id)) flags |= ImGuiTreeNodeFlags_Selected;

    // Выключенный элемент — серым: иначе «почему его не видно» решается
    // перебором свойств.
    if (!xf->Visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", obj.Name().c_str());
    if (!xf->Visible) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) host.ToggleSelection(id);
        else host.SetSelectedId(id);
    }

    // Порядок среди соседей — тут же, стрелками. Layer «больше — поверх», и
    // объяснять это в свойствах отдельным числом дольше, чем показать местом
    // в списке.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 44.0f);
    if (EditorIcons::IconOnlyButton("up", T("Bring forward"))) {
        host.PushUndoSnapshot();
        ++xf->Layer;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("drop", T("Send backward"))) {
        host.PushUndoSnapshot();
        --xf->Layer;
    }

    if (open && hasUiChildren) {
        for (entt::entity c : children) {
            if (!reg.all_of<ui::Transform>(c)) continue;
            const IdComponent* cid = reg.try_get<IdComponent>(c);
            if (cid) DrawTreeNode(host, scene, cid->Id, depth + 1);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UIEditorPanel::DrawTree(EditorHost& host, float width) {
    ImGui::BeginChild("##ui_tree", ImVec2(width, 0), ImGuiChildFlags_Borders);
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    ImGui::TextDisabled("%s", T("Elements"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Only interface entities are listed here.\n"
                                  "The rest of the scene is in the Hierarchy."));
    }
    ImGui::Separator();

    // Корни: элементы, у которых нет родителя-элемента. Именно они якорятся к
    // экрану, остальные — к своему родителю.
    int roots = 0;
    auto view = reg.view<ui::Transform>();
    for (entt::entity e : view) {
        const entt::entity parent = scene.ParentOf(e);
        if (parent != entt::null && reg.all_of<ui::Transform>(parent)) continue;
        const IdComponent* id = reg.try_get<IdComponent>(e);
        if (!id) continue;
        ++roots;
        DrawTreeNode(host, scene, id->Id, 0);
    }
    if (roots == 0) {
        ImGui::Spacing();
        Hint(T("There are no interface elements yet. Add one with the Element button above."));
    }

    // Удаление — внизу и только по выделенному: в дереве это же действие
    // рядом с каждой строкой превратило бы список в поле мин.
    ImGui::Separator();
    ImGui::BeginDisabled(SelectedUICount(host) == 0);
    if (EditorIcons::Button("trash", T("Delete"), T("Delete the selected elements")))
        host.DeleteSelected();
    ImGui::SameLine();
    if (EditorIcons::Button("copy", T("Duplicate"), T("Duplicate the selected elements")))
        host.DuplicateSelected();
    ImGui::EndDisabled();

    ImGui::EndChild();
}

// ============================================================================
//  Холст: игровой кадр в разрешении игры
// ============================================================================
void UIEditorPanel::DrawCanvas(EditorHost& host) {
    UIToolSettings& tools = host.UITools();

    int gw = 0, gh = 0;
    GameFrameSize(host, gw, gh);
    // Кадр рисуется В РАЗРЕШЕНИИ ИГРЫ. Это и есть главное отличие от прежнего
    // «режима вёрстки»: там холстом был вьюпорт редактора, то есть чужой
    // размер и чужое соотношение сторон.
    host.SetGameViewportSize(gw, gh);
    tools.FrameSize = glm::vec2((float)gw, (float)gh);

    ImGui::BeginChild("##ui_canvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (avail.x < 32.0f || avail.y < 32.0f) { ImGui::EndChild(); return; }

    // Вписывание: кадр целиком, с полем по краям — за границей экрана тоже
    // надо что-то видеть, иначе уехавший элемент не поймать мышью.
    const float fit = std::min(avail.x / (float)gw, avail.y / (float)gh) * 0.92f;
    if (m_fitOnce) {
        m_zoom = std::clamp(fit, 0.10f, 4.0f);
        m_fitOnce = false;
    }

    const ImVec2 imgSize((float)gw * m_zoom, (float)gh * m_zoom);
    const ImVec2 imgPos(origin.x + (avail.x - imgSize.x) * 0.5f + m_pan.x,
                        origin.y + (avail.y - imgSize.y) * 0.5f + m_pan.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Поле вокруг экрана — темнее самого экрана: границу игрового кадра видно
    // всегда, и элемент, уехавший за неё, отличим от стоящего у края.
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      IM_COL32(24, 26, 30, 255));

    // Сам кадр. Картинка игры под интерфейсом — ровно то, поверх чего худ и
    // рисуется; Backdrop гасит её, когда мешает.
    const uint64_t tex = host.GameTexture();
    ImGui::SetCursorScreenPos(imgPos);
    if (tex) {
        ImGui::Image((ImTextureID)(std::intptr_t)tex, imgSize, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::Dummy(imgSize);
        dl->AddRectFilled(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                          IM_COL32(30, 33, 38, 255));
    }
    if (tools.Backdrop > 0.001f) {
        dl->AddRectFilled(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                          IM_COL32(26, 28, 33, (int)(tools.Backdrop * 255.0f)));
    }
    // Граница экрана и осевые линии: по ним ставят то, что должно быть ровно
    // посередине, и промах в пиксель виден сразу.
    dl->AddRect(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                IM_COL32(110, 120, 140, 200));
    dl->AddLine(ImVec2(imgPos.x + imgSize.x * 0.5f, imgPos.y),
                ImVec2(imgPos.x + imgSize.x * 0.5f, imgPos.y + imgSize.y),
                IM_COL32(70, 78, 92, 160));
    dl->AddLine(ImVec2(imgPos.x, imgPos.y + imgSize.y * 0.5f),
                ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y * 0.5f),
                IM_COL32(70, 78, 92, 160));

    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    // Колесо — масштаб, средняя кнопка — панорама. Как в любом редакторе
    // холста; настраивать это отдельными полями незачем.
    if (hovered && !m_canvas.IsUsing()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            // Масштаб ОТ КУРСОРА: точка под мышью остаётся на месте. «От
            // центра» заставляет после каждого шага догонять панорамой то, на
            // что смотрел.
            //
            // Считается прямо: где сейчас под мышью точка кадра (f), где она
            // должна оказаться после смены масштаба, и какая панорама это даёт.
            const ImVec2 m = ImGui::GetMousePos();
            const float next = std::clamp(m_zoom * (io.MouseWheel > 0 ? 1.12f : 0.89f), 0.10f, 4.0f);
            const ImVec2 f((m.x - imgPos.x) / m_zoom, (m.y - imgPos.y) / m_zoom);
            const ImVec2 size2((float)gw * next, (float)gh * next);
            m_pan.x = m.x - f.x * next - origin.x - (avail.x - size2.x) * 0.5f;
            m_pan.y = m.y - f.y * next - origin.y - (avail.y - size2.y) * 0.5f;
            m_zoom = next;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            m_pan.x += io.MouseDelta.x;
            m_pan.y += io.MouseDelta.y;
        }
        // Home — вернуть один к одному; Shift+F — вписать ВСЁ, включая
        // уехавшее за экран. Это и есть ответ на «элемент пропал»: одна
        // клавиша показывает его вместе с экраном.
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) { m_zoom = 1.0f; m_pan = ImVec2(0, 0); }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && ImGui::GetIO().KeyShift) {
            const sage::ui::UIRect all = m_canvas.ContentBounds(gw, gh);
            const float k = std::min(avail.x / std::max(all.w, 1.0f),
                                     avail.y / std::max(all.h, 1.0f)) * 0.92f;
            m_zoom = std::clamp(k, 0.10f, 4.0f);
            // Центр содержимого — в центр панели.
            const ImVec2 size2((float)gw * m_zoom, (float)gh * m_zoom);
            const ImVec2 centre((all.x + all.w * 0.5f) * m_zoom, (all.y + all.h * 0.5f) * m_zoom);
            m_pan.x = avail.x * 0.5f - centre.x - (avail.x - size2.x) * 0.5f;
            m_pan.y = avail.y * 0.5f - centre.y - (avail.y - size2.y) * 0.5f;
        }
    }

    // И собственно вёрстка мышью: рамки, ручки, привязки, рамка выделения.
    // Вся математика — в UICanvas, он же считает попадание и пишет в компоненты.
    m_canvas.Draw(host, dl, imgPos, imgSize, gw, gh, hovered);

    ImGui::EndChild();
}

// ============================================================================
//  Правая колонка: свойства и инструменты вёрстки
// ============================================================================
void UIEditorPanel::DrawAlignTools(EditorHost& host) {
    const int count = SelectedUICount(host);

    ImGui::SeparatorText(T("Align"));
    if (count == 0) {
        Hint(T("Select an interface element."));
    } else {
        Hint(count == 1 ? T("One element — aligned to its parent")
                        : T("Aligned to the last clicked element"));
    }
    // Шесть кнопок с РИСУНКОМ, а не с подписью: полоска у края квадратика
    // читается быстрее слова «Left», а шесть слов заняли бы три строки.
    struct AlignDef { const char* Id; ui::AlignEdge Edge; const char* Tip; };
    const AlignDef aligns[6] = {
        {"al", ui::AlignEdge::Left, T("Left edges")},
        {"ac", ui::AlignEdge::CenterX, T("Centers horizontally")},
        {"ar", ui::AlignEdge::Right, T("Right edges")},
        {"at", ui::AlignEdge::Top, T("Top edges")},
        {"am", ui::AlignEdge::CenterY, T("Centers vertically")},
        {"ab", ui::AlignEdge::Bottom, T("Bottom edges")},
    };
    for (int i = 0; i < 6; ++i) {
        if (i == 3) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        else if (i > 0) ImGui::SameLine();
        if (AlignButton(aligns[i].Id, aligns[i].Edge, aligns[i].Tip, count >= 1))
            uiops::Align(host, aligns[i].Edge);
    }

    ImGui::SeparatorText(T("Distribute"));
    ImGui::BeginDisabled(count < 3);
    if (ImGui::Button(T("Across"))) uiops::Distribute(host, true, false);
    ImGui::SameLine();
    if (ImGui::Button(T("Down"))) uiops::Distribute(host, false, false);
    ImGui::EndDisabled();
    if (count < 3) Hint(T("Needs three elements or more."));

    ImGui::SeparatorText(T("Quick actions"));
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button(T("Fill the parent"), ImVec2(-1.0f, 0.0f))) uiops::StretchToParent(host, 0.0f);
    if (ImGui::Button(T("Fill with a margin"), ImVec2(-1.0f, 0.0f)))
        uiops::StretchToParent(host, 16.0f);
    if (ImGui::Button(T("Round to the grid"), ImVec2(-1.0f, 0.0f)))
        uiops::SnapSelectionToGrid(host);
    if (ImGui::Button(T("Bring back on screen"), ImVec2(-1.0f, 0.0f))) uiops::BringIntoView(host);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Pushes the selected elements back inside the screen."));
    ImGui::EndDisabled();
}

void UIEditorPanel::DrawSide(EditorHost& host, float width) {
    ImGui::BeginChild("##ui_side", ImVec2(width, 0), ImGuiChildFlags_Borders);

    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();
    const bool isElement = obj.Valid() && reg.all_of<ui::Transform>(obj.Entity());

    if (!isElement) {
        ImGui::TextDisabled("%s", T("Element"));
        ImGui::Separator();
        Hint(T("Select an element on the canvas or in the list on the left."));
    } else {
        ImGui::TextDisabled("%s", T("Element"));
        ImGui::SameLine();
        ImGui::TextUnformatted(obj.Name().c_str());
        ImGui::Separator();

        // Имя правится здесь же: в дереве слева его читают, а переименовывают
        // там, где смотрят на свойства.
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
        if (ImGui::InputText(T("Name"), buf, sizeof(buf))) obj.SetName(buf);
        host.TrackLastImGuiItem();

        // Инструменты вёрстки — ВЫШЕ свойств: они занимают три строки и нужны
        // постоянно, а свойств три десятка, и уехав под них, выравнивание
        // оказалось бы за пределами экрана.
        DrawAlignTools(host);

        ImGui::SeparatorText(T("Properties"));
        // Подписи у ImGui стоят СПРАВА от поля, и в узкой колонке они
        // обрезались посередине слова. Отдаём им фиксированную долю ширины.
        ImGui::PushItemWidth(-118.0f);
        // ТЕ ЖЕ свойства, что в инспекторе — общий модуль, а не вторая копия.
        sage::editor::UIPropsContext ctx;
        ctx.Preview = &m_preview;
        ctx.Browser = &m_browser;
        ctx.BrowseTarget = &m_browseTarget;
        sage::editor::DrawUIElementProperties(host, obj, ctx);
        ImGui::PopItemWidth();
    }

    ImGui::EndChild();
}

// ============================================================================
void UIEditorPanel::Draw(EditorHost& host, bool* open) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    // Файловый диалог качается ДО окна: он живёт дольше одного кадра, а его
    // результат надо положить в поле, о котором знает только эта панель.
    if (m_browser.Draw() && m_browseTarget) {
        // Ссылка ОТНОСИТЕЛЬНО ПРОЕКТА: абсолютный путь уехал бы в .sage и не
        // открылся бы ни на другой машине, ни в собранной игре.
        *m_browseTarget = host.CurrentProject().AssetRef(m_browser.Result());
        m_browseTarget = nullptr;
    }

    ImGui::SetNextWindowSize(ImVec2(1100, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Interface" "###UIEditor"), open)) {
        ImGui::End();
        return;
    }

    DrawTopBar(host);
    ImGui::Separator();

    DrawTree(host, m_treeWidth);
    ImGui::SameLine();
    // Холст занимает всё, что осталось между колонками: он здесь главный, и
    // отдавать ему остаток — единственная раскладка, которая не требует
    // подгонки при каждом изменении размера окна.
    ImGui::BeginChild("##ui_center", ImVec2(-m_sideWidth - ImGui::GetStyle().ItemSpacing.x, 0),
                      ImGuiChildFlags_None);
    DrawCanvas(host);
    ImGui::EndChild();
    ImGui::SameLine();
    DrawSide(host, 0.0f);

    ImGui::End();
}
