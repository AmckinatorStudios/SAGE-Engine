#include "GamePanel.h"

#include <algorithm>

#include <cstdint>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Config.h"
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
        // КАДР — В РАЗРЕШЕНИИ ИГРЫ, а не в размере панели.
        //
        // Раньше игра рисовалась ровно в панель, и от этого было две беды.
        // Первая видна сразу: панель почти никогда не совпадает по соотношению
        // сторон с окном игры, а интерфейс сверстан под ЕГО пропорции — то
        // есть «игровое окно» показывало не то, что увидит игрок. Вторая
        // всплыла с редактором интерфейса: он тоже просит кадр, но в
        // разрешении игры, и два размера дрались каждый кадр — буфер
        // пересоздавался по два раза за кадр, а картинка доставалась тому, кто
        // попросил последним.
        //
        // Теперь размер один, а панель ВПИСЫВАЕТ его с полями по краям.
        const sage::EngineConfig& cfg = host.Settings();
        const int gw = std::max(64, cfg.Width);
        const int gh = std::max(64, cfg.Height);
        host.SetGameViewportSize(gw, gh);

        const float k = std::min(avail.x / (float)gw, avail.y / (float)gh);
        const ImVec2 size((float)gw * k, (float)gh * k);
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const ImVec2 pos(base.x + (avail.x - size.x) * 0.5f,
                         base.y + (avail.y - size.y) * 0.5f);
        // Поля вокруг кадра — тёмные: граница игрового окна должна быть видна.
        ImGui::GetWindowDrawList()->AddRectFilled(
            base, ImVec2(base.x + avail.x, base.y + avail.y), IM_COL32(16, 17, 20, 255));
        ImGui::SetCursorScreenPos(pos);
        // Текстура OpenGL идёт снизу-вверх — переворот по V.
        ImTextureID tex = (ImTextureID)(std::intptr_t)host.GameTexture();
        ImGui::Image(tex, size, ImVec2(0, 1), ImVec2(1, 0));

        // Курсор в пикселях ИГРОВОГО КАДРА: интерфейс игры сверстан под них, а
        // мышь приходит в координатах окна редактора. Делим на масштаб показа —
        // раньше его не было, потому что кадр и панель совпадали.
        const ImVec2 mouse = ImGui::GetMousePos();
        const float scale = k > 0.0001f ? k : 1.0f;
        m_mouseX = (mouse.x - pos.x) / scale;
        m_mouseY = (mouse.y - pos.y) / scale;
        m_mouseInside = ImGui::IsItemHovered() && m_mouseX >= 0.0f && m_mouseY >= 0.0f &&
                        m_mouseX < (float)gw && m_mouseY < (float)gh;
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
