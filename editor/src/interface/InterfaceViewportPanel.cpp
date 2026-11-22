#include "EditorTheme.h"
#include "InterfaceViewportPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "CanvasToolbarRules.h"
#include "InterfaceWidgets.h"
#include "../Localization.h"
#include "../Project.h"
#include "../UIElementProperties.h"
#include "../UILayoutOps.h"
#include "../ui/UI.h"
#include "sage/core/Config.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "ui/ColorPicker.h"

namespace {

namespace ui = sage::ui;

using sage::editor::interfacewidgets::AlignButton;

// Приглушённое пояснение с переносом: колонки узкие, а обычный TextDisabled не
// переносит и обрезает строку посередине слова.
// Сколько выделенных элементов интерфейса: от этого зависит, что имеет смысл.
int SelectedUICount(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    int count = 0;
    for (int id : host.Selection().All()) {
        GameObject obj = scene.Get(id);
        if (obj.Valid() && reg.all_of<ui::Element>(obj.Entity())) ++count;
    }
    return count;
}

// Разрешение, в котором интерфейс увидит игрок.
//
// Берётся из настроек ИГРЫ, а не из размера панели: под него считаются якоря,
// растяжения и проценты, и верстать в размер окна редактора значило бы верстать
// под экран, которого у игрока нет.
void GameFrameSize(EditorHost& host, int& outW, int& outH) {
    // РАЗРЕШЕНИЕ ПРЕДПРОСМОТРА ПЕРЕБИВАЕТ НАСТРОЙКИ ИГРЫ, но их не меняет.
    // Посмотреть, как худ ляжет на 1080x1920, раньше можно было, только правя
    // настройки проекта туда и обратно — то есть трогая то, что уедет в игру,
    // ради одного взгляда.
    const glm::ivec2& p = host.Tools().UI.PreviewSize;
    if (p.x > 0 && p.y > 0) {
        outW = std::max(64, p.x);
        outH = std::max(64, p.y);
        return;
    }
    const sage::EngineConfig& cfg = host.Settings();
    outW = std::max(64, cfg.Width);
    outH = std::max(64, cfg.Height);
}

// РАЗДЕЛИТЕЛЬ ГРУПП: тонкая спокойная линия по центру строки с равными
// отступами слева и справа. Только между смысловыми группами, не между
// кнопками: панель — одна строка инструментов, а не набор карточек.
constexpr float kSepGap = 8.0f;   // отступ по обе стороны линии
constexpr float kStep = 3.0f;     // между кнопками одной группы

void GroupSeparator() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = std::floor(p.x) + 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, p.y + h * 0.8f),
                                        EditorTheme::Color32(EditorTheme::Role::Line), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
}

} // namespace

void InterfaceViewportPanel::DrawToolbar(EditorHost& host) {
    UIToolSettings& tools = host.Tools().UI;
    namespace bar = sage::editor::canvasbar;

    namespace prefs = sage::editor::prefs;
    if (!m_backdropLoaded) {
        m_backdropLoaded = true;
        tools.Backdrop = std::clamp(prefs::GetFloat("ui.backdrop", tools.Backdrop), 0.0f, 1.0f);
        tools.BackdropColor.r = std::clamp(prefs::GetFloat("ui.backdrop_r", tools.BackdropColor.r), 0.0f, 1.0f);
        tools.BackdropColor.g = std::clamp(prefs::GetFloat("ui.backdrop_g", tools.BackdropColor.g), 0.0f, 1.0f);
        tools.BackdropColor.b = std::clamp(prefs::GetFloat("ui.backdrop_b", tools.BackdropColor.b), 0.0f, 1.0f);
    }
    // Запись — ПО ОКОНЧАНИИ правки, а не на каждый кадр перетаскивания: файл
    // настроек переписывается целиком.
    auto save = [&tools]() {
        prefs::SetFloat("ui.backdrop", tools.Backdrop);
        prefs::SetFloat("ui.backdrop_r", tools.BackdropColor.r);
        prefs::SetFloat("ui.backdrop_g", tools.BackdropColor.g);
        prefs::SetFloat("ui.backdrop_b", tools.BackdropColor.b);
    };

    const float h = ImGui::GetFrameHeight();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    // --- Разрешение — компактно справа, первой строкой ----------------------
    //
    // Рисуется ПЕРВЫМ: группы инструментов знают, сколько места справа занято,
    // и уходят на следующую строку, а не наезжают на него.
    int gw = 0, gh = 0;
    GameFrameSize(host, gw, gh);
    const bar::PreviewMode mode = bar::ModeOf(tools);
    char res[64];
    std::snprintf(res, sizeof(res), "%d x %d", gw, gh);
    const float resW = ImGui::CalcTextSize(res).x + ImGui::GetStyle().FramePadding.x * 2.0f + h + 8.0f;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(std::max(rowStart.x, right - resW), rowStart.y));
    ImGui::SetNextItemWidth(resW);
    if (ImGui::BeginCombo("##preview_res", res)) {
        if (ImGui::Selectable(T("As in the game"), mode == bar::PreviewMode::Game)) {
            tools.PreviewSize = glm::ivec2(0);
            tools.PreviewCustom = false;
            RequestFit();
        }
        ImGui::Separator();
        for (const bar::PreviewPreset& p : bar::kPreviewPresets) {
            const bool on = mode == bar::PreviewMode::Preset && tools.PreviewSize.x == p.W && tools.PreviewSize.y == p.H;
            if (ImGui::Selectable(p.Label, on)) {
                tools.PreviewSize = glm::ivec2(p.W, p.H);
                tools.PreviewCustom = false;
                RequestFit();
            }
        }
        ImGui::Separator();
        // СВОЁ — явным выбором: поля и «Повернуть» появляются только у него.
        if (ImGui::Selectable(T("Custom"), mode == bar::PreviewMode::Custom, ImGuiSelectableFlags_NoAutoClosePopups)) {
            if (mode != bar::PreviewMode::Custom) tools.PreviewSize = glm::ivec2(gw, gh);
            tools.PreviewCustom = true;
        }
        if (bar::ShowCustomResolution(tools)) {
            int wh[2] = {tools.PreviewSize.x, tools.PreviewSize.y};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragInt2("##custom_res", wh, 1.0f, 64, 8192)) {
                tools.PreviewSize = glm::ivec2(std::max(64, wh[0]), std::max(64, wh[1]));
                RequestFit();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Width and height of the preview frame"));
            // ПОВЕРНУТЬ — ОДНОЙ КНОПКОЙ: «как это на телефоне боком» руками —
            // два поля, которые надо поменять местами, не перепутав.
            if (EditorIcons::Button("refresh", T("Rotate the screen"))) {
                tools.PreviewSize = glm::ivec2(tools.PreviewSize.y, tools.PreviewSize.x);
                RequestFit();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", mode != bar::PreviewMode::Game
            ? T("Preview resolution. The layout is computed in it, and the game\n"
                "settings are not touched — this is a look, not a change.")
            : T("Game resolution from Game Settings.\n"
                "The layout is computed in it, so this is exactly\n"
                "what the player will see."));
    }
    const float rowLimit = right - resW - kSepGap;   // первая строка — до разрешения
    ImGui::SetCursorScreenPos(rowStart);

    // --- Группы: строка с разделителями, перенос по ширине -------------------
    int row = 0;
    auto group = [&](int i, const auto& body) {
        if (i > 0) {
            const float prevEnd = ImGui::GetItemRectMax().x;
            const float limit = row == 0 ? rowLimit : right;
            // Встаёт в строку — с разделителем; нет — на следующую строку, без
            // него (линия в начале строки ничего не разделяет).
            if (prevEnd + 2.0f * kSepGap + 1.0f + m_groupW[i] <= limit) {
                ImGui::SameLine(0.0f, kSepGap);
                GroupSeparator();
                ImGui::SameLine(0.0f, kSepGap);
            } else {
                ++row;
            }
        }
        ImGui::BeginGroup();
        body();
        ImGui::EndGroup();
        m_groupW[i] = ImGui::GetItemRectSize().x;
    };

    // 1. ПРОСМОТР: масштаб, 1:1, вписать.
    group(0, [&] {
        // Проценты, а не доли: «120%» читается сразу.
        ImGui::SetNextItemWidth(72.0f);
        float zoomPercent = m_zoom * 100.0f;
        if (ImGui::DragFloat("##zoom", &zoomPercent, 1.0f, 10.0f, 400.0f, "%.0f%%")) {
            m_zoom = std::clamp(zoomPercent / 100.0f, 0.10f, 4.0f);
            m_autoFit = false;   // попросили масштаб числом — вписывать больше нечего
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Canvas zoom (wheel over the canvas)"));
        ImGui::SameLine(0.0f, kStep);
        if (ImGui::Button("1:1", ImVec2(0.0f, h))) { m_zoom = 1.0f; m_pan = ImVec2(0, 0); m_autoFit = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Actual size: one frame pixel is one screen pixel"));
        ImGui::SameLine(0.0f, kStep);
        // Кнопка нажата — режим ВКЛЮЧЁН: она и показывает, вписан ли кадр.
        if (EditorIcons::IconOnlyButton("fit", T("Fit the frame into the panel"), m_autoFit)) RequestFit();
    });

    // 2. СЕТКА И ПРИВЯЗКА. Показ сетки и притяжка к ней — независимы; шаг
    // общий, порог — общий у сетки и краёв (алгоритм ui::Snap не меняется).
    group(1, [&] {
        if (EditorIcons::IconOnlyButton("grid", T("Show the grid"), tools.ShowGrid)) tools.ShowGrid = !tools.ShowGrid;
        ImGui::SameLine(0.0f, kStep);
        if (EditorIcons::IconOnlyButton("magnet", T("Snap to the grid"), tools.Snap.ToGrid))
            tools.Snap.ToGrid = !tools.Snap.ToGrid;
        if (bar::ShowGridStep(tools)) {
            ImGui::SameLine(0.0f, kStep);
            ImGui::SetNextItemWidth(64.0f);
            ImGui::DragFloat("##cell", &tools.Snap.GridStep, 1.0f, 1.0f, 256.0f, "%.0f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Grid step: of the grid you see and of snapping to it"));
        }
        ImGui::SameLine(0.0f, kStep);
        if (EditorIcons::IconOnlyButton("align", T("Snap to edges and centers of the parent and neighbours"),
                                        tools.Snap.ToEdges))
            tools.Snap.ToEdges = !tools.Snap.ToEdges;
        if (bar::ShowSnapThreshold(tools)) {
            ImGui::SameLine(0.0f, kStep);
            ImGui::SetNextItemWidth(56.0f);
            ImGui::DragFloat("##snap_threshold", &tools.Snap.Threshold, 0.25f, 1.0f, 32.0f, "%.0f px");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Snap distance: how close, in frame pixels, an edge has to come to stick"));
        }
    });

    // 3. РЕДАКТИРОВАНИЕ: выравнивание и якоря.
    group(2, [&] {
        // ВЫРАВНИВАНИЕ — НА ХОЛСТЕ, а не только в инспекторе: действие над тем,
        // что видно. Меню, а не восемь кнопок в строке.
        if (EditorIcons::IconOnlyButton("align-left", T("Align and distribute")))
            ImGui::OpenPopup("AlignTools###AlignTools");
        if (Sage::UI::MenuScope alignMenu; ImGui::BeginPopup("AlignTools###AlignTools")) {
            const int count = SelectedUICount(host);
            ImGui::TextDisabled("%s", count == 0   ? T("Select elements to align them")
                                      : count == 1 ? T("One element — aligned to its parent")
                                                   : T("Aligned to the last clicked element"));
            ImGui::Separator();
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
                if (i == 3) ImGui::SameLine(0.0f, 10.0f);
                else if (i > 0) ImGui::SameLine(0.0f, kStep);
                if (AlignButton(aligns[i].Id, aligns[i].Edge, aligns[i].Tip, count >= 1))
                    uiops::Align(host, aligns[i].Edge);
            }
            // Распределение — только у трёх и больше: с двумя распределять
            // нечего, и пункты без смысла не показываются вовсе.
            if (count >= 3) {
                ImGui::Separator();
                ImGui::TextDisabled("%s", T("Distribute"));
                if (EditorIcons::IconOnlyButton("distribute-x", T("Distribute across"))) uiops::Distribute(host, true, false);
                ImGui::SameLine(0.0f, kStep);
                if (EditorIcons::IconOnlyButton("distribute-y", T("Distribute down"))) uiops::Distribute(host, false, false);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, kStep);
        // Ручки якоря на холсте: собранный экран удобнее смотреть без них.
        if (EditorIcons::IconOnlyButton("anchor-tl", T("Edit the anchor on the canvas"), tools.EditAnchors))
            tools.EditAnchors = !tools.EditAnchors;
    });

    // 4. ОТОБРАЖЕНИЕ: подложка, её цвет, рамки всех элементов.
    group(3, [&] {
        // Ползунком, а не галкой: худ правят ПОВЕРХ игры — там нужна
        // полупрозрачная плёнка.
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("##backdrop", &tools.Backdrop, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Backdrop under the interface.\n"
                                      "1 — a flat fill instead of the game frame;\n"
                                      "0 — design the HUD over the game."));
        }
        // Цвет — образцом, и только когда подложка есть: при нуле он ни на
        // что не влияет.
        if (bar::ShowBackdropColor(tools)) {
            ImGui::SameLine(0.0f, kStep);
            const ImVec4 c(tools.BackdropColor.r, tools.BackdropColor.g, tools.BackdropColor.b, 1.0f);
            if (ImGui::ColorButton("##backdrop_swatch", c, ImGuiColorEditFlags_NoTooltip, ImVec2(h, h)))
                ImGui::OpenPopup("BackdropColor###BackdropColor");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Backdrop color"));
        }
        if (Sage::UI::MenuScope backdropMenu; ImGui::BeginPopup("BackdropColor###BackdropColor")) {
            ImGui::TextDisabled("%s", T("Backdrop color"));
            ImGui::Separator();
            Sage::UI::ColorPickerInline("##backdrop_color", &tools.BackdropColor.x);
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
            // Готовые крайности: тёмный интерфейс смотрят на светлом, светлый —
            // на тёмном.
            bool preset = false;
            if (ImGui::Button(T("Dark"))) { tools.BackdropColor = glm::vec3(0.10f, 0.11f, 0.13f); preset = true; }
            ImGui::SameLine();
            if (ImGui::Button(T("Grey"))) { tools.BackdropColor = glm::vec3(0.50f, 0.50f, 0.52f); preset = true; }
            ImGui::SameLine();
            if (ImGui::Button(T("Light"))) { tools.BackdropColor = glm::vec3(0.92f, 0.93f, 0.95f); preset = true; }
            if (preset) save();
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, kStep);
        if (EditorIcons::IconOnlyButton("wire", T("Outlines of all elements (the selected one is always outlined)"),
                                        tools.ShowAllOutlines))
            tools.ShowAllOutlines = !tools.ShowAllOutlines;
    });

    // 5. НАПРАВЛЯЮЩИЕ: расстояния при перемещении, безопасная зона.
    group(4, [&] {
        if (EditorIcons::IconOnlyButton("ruler", T("Distances to the parent's edges while moving"), tools.ShowDistances))
            tools.ShowDistances = !tools.ShowDistances;
        ImGui::SameLine(0.0f, kStep);
        if (EditorIcons::IconOnlyButton("focus", T("Safe area: everything outside it can be cut off (camera notch, "
                                                   "rounded corners, TV overscan)"),
                                        tools.ShowSafeArea))
            tools.ShowSafeArea = !tools.ShowSafeArea;
        if (bar::ShowSafeAreaPercent(tools)) {
            ImGui::SameLine(0.0f, kStep);
            ImGui::SetNextItemWidth(60.0f);
            ImGui::DragFloat("##safe", &tools.SafeAreaPercent, 0.1f, 0.0f, 20.0f, "%.1f%%");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Safe area margin, percent of the frame"));
        }
    });
}

// ============================================================================
//  Создание элементов
// ============================================================================

void InterfaceViewportPanel::DrawCanvas(EditorHost& host) {
    UIToolSettings& tools = host.Tools().UI;

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
    if (m_autoFit) {
        m_zoom = std::clamp(fit, 0.10f, 4.0f);
        m_pan = ImVec2(0.0f, 0.0f);
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
    // рисуется; подложка гасит её, когда мешает, — но гасит ВНУТРИ кадра, под
    // интерфейсом (см. EditorSceneRenderer::DrawGameUI). Здесь её не рисуем:
    // прямоугольник поверх готового кадра ложился и на интерфейс тоже.
    const uint64_t tex = host.GameTexture();
    ImGui::SetCursorScreenPos(imgPos);
    if (tex) {
        ImGui::Image((ImTextureID)(std::intptr_t)tex, imgSize, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::Dummy(imgSize);
        // Кадра нет (ни камеры, ни подложки) — заглушка ЦВЕТОМ ПОДЛОЖКИ: чужой
        // прямоугольник посреди выбранного фона читается как поломка.
        dl->AddRectFilled(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                          ImGui::GetColorU32(ImVec4(tools.BackdropColor.x, tools.BackdropColor.y,
                                                    tools.BackdropColor.z, 1.0f)));
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
            m_autoFit = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            m_pan.x += io.MouseDelta.x;
            m_pan.y += io.MouseDelta.y;
            m_autoFit = false;   // кадр сдвинули руками — держать его вписанным нечестно
        }
        // Home — вернуть один к одному; Shift+F — вписать ВСЁ, включая
        // уехавшее за экран. Это и есть ответ на «элемент пропал»: одна
        // клавиша показывает его вместе с экраном.
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            m_zoom = 1.0f;
            m_pan = ImVec2(0, 0);
            m_autoFit = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && ImGui::GetIO().KeyShift) {
            m_autoFit = false;   // «показать всё» — это свой масштаб, не вписывание кадра
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


void InterfaceViewportPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(EditorIcons::WindowTitle("layout", T("Canvas"), "InterfaceViewport").c_str(), &open)) {
        ImGui::End();
        return;
    }
    DrawToolbar(host);
    ImGui::Separator();
    DrawCanvas(host);
    ImGui::End();
}
