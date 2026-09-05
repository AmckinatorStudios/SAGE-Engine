#include "ui/CommandPalette.h"

#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorIcons.h"
#include "Localization.h"
#include "ui/UI.h"

namespace Sage::UI {

using EditorTheme::Role;
using sage::editor::T;

void CommandPalette::Open(const std::string& query) {
    m_open = true;
    m_justOpened = true;
    std::snprintf(m_query, sizeof(m_query), "%s", query.c_str());
    m_selected = 0;
}

void CommandPalette::Close() { m_open = false; }

void CommandPalette::Draw(CommandRegistry& registry) {
    if (!m_open) return;
    const Style& ui = Get();

    // Окно ставится вверху по центру экрана, а не там, где был курсор: палитру
    // ищут глазами в одном и том же месте, как в любом другом инструменте.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = ImMin(560.0f * Scale(), vp->WorkSize.x - ui.SpacingXL * 2.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.18f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui.SpacingSM, ui.SpacingSM));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui.CornerRadiusLarge);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Color(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::Color(Role::LineStrong));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##commandpalette", nullptr, flags)) {
        // Фокус в поле — сразу: палитру открывают, чтобы печатать, и лишний
        // щелчок мышью здесь сводит на нет весь смысл горячей клавиши.
        if (m_justOpened) {
            ImGui::SetKeyboardFocusHere();
            m_justOpened = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        SearchField("palette", m_query, sizeof(m_query), T("Search commands..."), -1.0f);

        const std::vector<int> found = registry.Search(m_query);
        if (m_selected >= (int)found.size()) m_selected = (int)found.size() - 1;
        if (m_selected < 0) m_selected = 0;

        // Стрелки и Enter работают, НЕ уводя фокус из поля: иначе после каждой
        // стрелки пришлось бы возвращаться к набору текста мышью.
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++m_selected;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --m_selected;
        if (!found.empty()) m_selected = (m_selected + (int)found.size()) % (int)found.size();
        const bool accept = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) Close();

        Separator();

        if (found.empty()) {
            EmptyState(T("Nothing found"), T("Try a different word."));
        } else {
            const float rowH = ui.RowHeight;
            const float listH = ImMin((float)found.size() * (rowH + ui.SpacingXS),
                                      vp->WorkSize.y * 0.45f);
            ImGui::BeginChild("##list", ImVec2(0.0f, listH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            for (int i = 0; i < (int)found.size(); ++i) {
                const Command& cmd = registry.All()[found[i]];
                const bool enabled = !cmd.Enabled || cmd.Enabled();
                const bool picked = (i == m_selected);
                ImGui::PushID(cmd.Id.c_str());

                ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::Color(Role::Selection));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(Role::Hover));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorTheme::Color(Role::Selection));
                if (!enabled) ImGui::BeginDisabled();
                const bool clicked = ImGui::Selectable("##row", picked,
                                                       ImGuiSelectableFlags_SpanAllColumns,
                                                       ImVec2(0.0f, rowH));
                if (!enabled) ImGui::EndDisabled();
                ImGui::PopStyleColor(3);

                // Содержимое строки рисуется ПОВЕРХ Selectable, чтобы сама
                // строка оставалась единым кликабельным элементом.
                const ImVec2 p0 = ImGui::GetItemRectMin();
                const ImVec2 p1 = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec4 textCol = EditorTheme::Color(enabled ? Role::Text : Role::TextFaint);
                float x = p0.x + ui.SpacingSM;
                if (!cmd.Icon.empty() && EditorIcons::Has(cmd.Icon.c_str())) {
                    EditorIcons::Overlay(x, p0.y + (rowH - ui.IconSize) * 0.5f, ui.IconSize,
                                         cmd.Icon.c_str(),
                                         glm::vec3(textCol.x, textCol.y, textCol.z));
                    x += ui.IconSize + ui.SpacingSM;
                }
                const float textY = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(textCol), cmd.Title.c_str());

                // Раздел и горячая клавиша — справа, приглушённо: они помогают
                // выбрать, но читается в первую очередь название.
                float right = p1.x - ui.SpacingSM;
                if (!cmd.Shortcut.empty()) {
                    const float w = ImGui::CalcTextSize(cmd.Shortcut.c_str()).x;
                    right -= w;
                    dl->AddText(ImVec2(right, textY),
                                ImGui::GetColorU32(EditorTheme::Color(Role::TextFaint)),
                                cmd.Shortcut.c_str());
                    right -= ui.SpacingMD;
                }
                if (!cmd.Category.empty()) {
                    const float w = ImGui::CalcTextSize(cmd.Category.c_str()).x;
                    if (right - w > x + ui.SpacingLG) {
                        dl->AddText(ImVec2(right - w, textY),
                                    ImGui::GetColorU32(EditorTheme::Color(Role::TextFaint)),
                                    cmd.Category.c_str());
                    }
                }

                if (picked) ImGui::SetScrollHereY(0.5f);
                if ((clicked || (picked && accept)) && enabled) {
                    registry.Run(cmd.Id);
                    Close();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        // Щелчок мимо палитры закрывает её — как и Escape.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
            Close();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace Sage::UI
