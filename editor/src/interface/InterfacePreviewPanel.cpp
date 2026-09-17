#include "InterfacePreviewPanel.h"

#include <algorithm>
#include <cstdint>

#include "imgui.h"

#include "../EditorHost.h"
#include "../Localization.h"
#include "../PanelWindows.h"

#include "sage/core/Config.h"

void InterfacePreviewPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Preview" "###InterfacePreview"), &open,
                      panelwindows::WindowFlags("InterfacePreview"))) {
        ImGui::End();
        return;
    }

    // Масштаб: 0 — вписать. Ползунок рядом, а не в настройках: на него смотрят
    // ровно тогда, когда мелкий текст надо разглядеть.
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("##preview_zoom", &m_zoom, 0.0f, 3.0f,
                       m_zoom <= 0.001f ? T("Fit") : "%.2fx");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("The frame exactly as the player sees it."));
    ImGui::Separator();

    const sage::EngineConfig& cfg = host.Settings();
    const float gw = (float)std::max(64, cfg.Width);
    const float gh = (float)std::max(64, cfg.Height);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 32.0f || avail.y < 32.0f) {
        ImGui::End();
        return;
    }

    const float fit = std::min(avail.x / gw, avail.y / gh);
    const float k = m_zoom <= 0.001f ? fit : m_zoom;
    const ImVec2 size(gw * k, gh * k);

    // КАДР ЖИВЁТ В СВОЁМ ОКНЕ, А НЕ ПРЯМО В ПАНЕЛИ.
    //
    // Без него приближённый кадр (масштаб больше вписанного) не помещался в
    // панель, заводил ей полосы прокрутки и показывался ОТ ЛЕВОГО ВЕРХНЕГО
    // УГЛА: смотреть на середину экрана, ради которой приближали, было нельзя,
    // а сам кадр читался как уехавший в сторону. Полосы к тому же съедали
    // ширину, от которой считалось вписывание, — и «вписать» на кадр за кадром
    // давало разный масштаб.
    //
    // Своё окно без прокрутки решает оба: кадр всегда СТОИТ ПО ЦЕНТРУ, а
    // вылезшее за края обрезается одинаково со всех сторон.
    ImGui::BeginChild("##preview_frame", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 pos(origin.x + (avail.x - size.x) * 0.5f, origin.y + (avail.y - size.y) * 0.5f);

    const uint64_t tex = host.GameTexture();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex) {
        dl->AddImage((ImTextureID)(std::intptr_t)tex, pos,
                     ImVec2(pos.x + size.x, pos.y + size.y), ImVec2(0, 1), ImVec2(1, 0));
    } else {
        // Кадра ещё нет — так бывает до первой отрисовки. Пустота честнее
        // заглушки: заглушку принимают за интерфейс.
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(28, 30, 35, 255));
    }
    ImGui::EndChild();
    ImGui::End();
}
