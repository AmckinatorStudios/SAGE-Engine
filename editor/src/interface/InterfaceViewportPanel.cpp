#include "InterfaceViewportPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../Localization.h"
#include "../PanelWindows.h"
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
    const sage::EngineConfig& cfg = host.Settings();
    outW = std::max(64, cfg.Width);
    outH = std::max(64, cfg.Height);
}

} // namespace

void InterfaceViewportPanel::DrawToolbar(EditorHost& host) {
    UIToolSettings& tools = host.Tools().UI;

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


void InterfaceViewportPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Canvas" "###InterfaceViewport"), &open,
                      panelwindows::WindowFlags("InterfaceViewport"))) {
        ImGui::End();
        return;
    }
    DrawToolbar(host);
    ImGui::Separator();
    DrawCanvas(host);
    ImGui::End();
}
