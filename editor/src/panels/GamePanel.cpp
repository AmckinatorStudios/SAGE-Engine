#include "GamePanel.h"

#include <cstdint>

#include "imgui.h"

#include "EditorHost.h"
#include "../Localization.h"
#include "sage/ui/UIInteraction.h"

void GamePanel::Draw(EditorHost& host, bool* open) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(T("Game" "###Game"), open);

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

        // Курсор в координатах ИГРОВОГО КАДРА: интерфейс игры сверстан под
        // размер картинки, а мышь приходит в координатах окна редактора.
        // Перевод возможен только здесь — где картинка нарисована.
        const ImVec2 origin = ImGui::GetItemRectMin();
        const ImVec2 mouse = ImGui::GetMousePos();
        m_mouseX = mouse.x - origin.x;
        m_mouseY = mouse.y - origin.y;
        m_mouseInside = ImGui::IsItemHovered() && m_mouseX >= 0.0f && m_mouseY >= 0.0f &&
                        m_mouseX < avail.x && m_mouseY < avail.y;
        m_mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    } else {
        m_mouseInside = false;
    }

    // Символы — очередью ImGui, а не опросом клавиш: буква зависит от раскладки
    // и композиции, и собрать её из кодов клавиш нельзя (ровно та же причина,
    // по которой плеер слушает char-колбэк окна, а не GLFW_KEY_*).
    m_typed.clear();
    if (m_focused) {
        const ImGuiIO& io = ImGui::GetIO();
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            sage::ui::AppendUtf8(m_typed, (unsigned int)io.InputQueueCharacters[i]);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
