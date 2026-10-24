#pragma once
// Общее для файлов ProjectLauncher*.cpp. Только для них.
#include "ProjectLauncher.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include "imgui_internal.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../Project.h"
#include "../ProjectTemplateCover.h"
#include "../ProjectTemplates.h"
#include "../ui/UI.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Version.h"

namespace fs = std::filesystem;

namespace Sage::Launcher {
namespace launcher_detail {

using EditorTheme::Role;

// Кегль из токенов — в единицах ДО масштаба интерфейса: ImGui умножает на
// style.FontScaleMain сам, а токены приходят уже умноженными (см. UIStyle.h).
// Без деления крупные надписи росли бы вдвое на экране со 150 %.
inline void PushFontSize(float scaledToken) {
    ImGui::PushFont(nullptr, scaledToken / std::max(0.01f, Sage::UI::Scale()));
}

// Пункт левой навигации: тонкий жёлтый указатель слева, подсвеченный фон,
// значок и светлый текст. Одинаковый для всех разделов — иначе «активный»
// начинает выглядеть по-разному в разных местах списка.
inline bool NavItem(const char* icon, const char* label, bool active) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float h = std::floor(ui.ControlHeight * 1.5f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    if (active) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Selection), ui.CornerRadius);
    else if (hovered) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Hover), ui.CornerRadius);
    if (active) {
        // Указатель — ТОНКИЙ. Толстая полоса и заливка сразу дают два акцента
        // на один пункт, и жёлтого на экране становится больше, чем смысла.
        dl->AddRectFilled(ImVec2(p0.x, p0.y + ui.SpacingXS),
                          ImVec2(p0.x + 2.0f * Sage::UI::Scale(), p1.y - ui.SpacingXS),
                          EditorTheme::Color32(Role::Accent), 1.0f);
    }

    const ImVec4 col = EditorTheme::Color(active ? Role::Text : Role::TextDim);
    EditorIcons::DrawLabeled(dl, ImVec2(p0.x + ui.SpacingMD, p0.y + (h - ui.IconSize) * 0.5f),
                             ui.IconSize, icon, ImGui::GetColorU32(col), label,
                             ImGui::GetColorU32(col));
    return pressed;
}

// Вкладка отбора («Все», «Игры», …). Не ImGui::TabBar: у вкладок ImGui свой
// вид, который не сходится с остальным окном, и своё состояние, которое живёт
// в imgui.ini, — а отбор списка это не «раскладка окон».
inline bool FilterChip(const char* label, bool active) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? EditorTheme::Color(Role::AccentMuted) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::Hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          EditorTheme::Color(active ? Role::Accent : Role::TextDim));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.SpacingMD, ui.PaddingControlY));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

// Строка «подпись — значение» в панели подробностей: значок, подпись слева,
// значение справа по краю. Таблицей, а не пробелами: значения обязаны
// начинаться на одной координате, иначе колонка «плывёт».
// width — ширина КОЛОНКИ подробностей. Передаётся, а не берётся из
// GetContentRegionAvail: доступное место тянется до края окна, и значение
// прижималось бы к самому краю панели, без поля.
inline void DetailRow(const char* icon, const char* label, const std::string& value, float width) {
    if (value.empty()) return;
    const Sage::UI::Style& ui = Sage::UI::Get();
    const ImVec4 dim = EditorTheme::Color(Role::TextDim);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(p0.x, p0.y + (ImGui::GetTextLineHeight() - ui.IconSize) * 0.5f,
                         ui.IconSize, icon, glm::vec3(dim.x, dim.y, dim.z));
    // Тот же зазор, что у всех пар «значок + подпись» в редакторе.
    const float labelX = ImGui::GetCursorPosX() + ui.IconSize + EditorIcons::TextGap();
    ImGui::SetCursorPosX(labelX);
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    // Значение — правым краем к правому краю колонки. Длинное укорачивается:
    // строка, вылезающая за панель, ломает колонку целиком.
    const std::string shown = Sage::UI::Truncate(value.c_str(), width * 0.6f);
    const float valueW = ImGui::CalcTextSize(shown.c_str()).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(labelX, ImGui::GetCursorPosX()) +
                         std::max(0.0f, (p0.x + width - valueW) - ImGui::GetCursorScreenPos().x));
    ImGui::TextUnformatted(shown.c_str());
}

} // namespace launcher_detail
using namespace launcher_detail;
} // namespace Sage::Launcher
