#include "GamePanel.h"

#include <cstdint>

#include "imgui.h"

#include "EditorHost.h"
#include "../Localization.h"

void GamePanel::Draw(EditorHost& host) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(T("Game" "###Game"));

    // Фокус запоминаем для ввода Play-режима (см. GamePanel::Focused).
    // RootAndChildWindows — чтобы клик по изображению внутри панели считался
    // фокусом самой панели.
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!host.HasPrimaryCamera()) {
        ImGui::Dummy(ImVec2(0, avail.y * 0.42f));
        const char* hint = "No camera in scene";
        float w = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX((avail.x - w) * 0.5f);
        ImGui::TextDisabled("%s", hint);
        const char* hint2 = "Add a CameraComponent (Inspector > Camera > Add Camera)";
        w = ImGui::CalcTextSize(hint2).x;
        ImGui::SetCursorPosX((avail.x - w) * 0.5f);
        ImGui::TextDisabled("%s", hint2);
    } else if (avail.x >= 8 && avail.y >= 8) {
        host.SetGameViewportSize((int)avail.x, (int)avail.y);
        // Текстура OpenGL идёт снизу-вверх — переворот по V.
        ImTextureID tex = (ImTextureID)(std::intptr_t)host.GameTexture();
        ImGui::Image(tex, avail, ImVec2(0, 1), ImVec2(1, 0));
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
