#include "panels/settings/SettingsUi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../../EditorIcons.h"
#include "../../EditorTheme.h"
#include "../../Localization.h"
#include "../../ui/UI.h"

namespace sage::editor::settingsui {

std::string Lower(const char* s) {
    std::string out;
    if (!s) return out;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80) {
            out += (char)std::tolower(*p);
            ++p;
        } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
            // Кириллица: А-Я (U+0410..042F) -> а-я, Ё (U+0401) -> ё.
            unsigned cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;
            else if (cp == 0x401) cp = 0x451;
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
            p += 2;
        } else {
            out += (char)*p;
            ++p;
        }
    }
    return out;
}

bool Matches(const Ctx& c, const char* label, const char* keywords) {
    if (c.Filter.empty()) return true;
    if (Lower(label).find(c.Filter) != std::string::npos) return true;
    return keywords && Lower(keywords).find(c.Filter) != std::string::npos;
}

namespace {
void OpenPending(Ctx& c) {
    if (c.GroupOpen || !c.PendingTitle) return;
    c.GroupOpen = true;
    const Sage::UI::Style& ui = Sage::UI::Get();
    // Карточка: фон на ступень светлее окна, скругление, воздух внутри и
    // между карточками. Рамки нет — группу отделяет фон, а не линия. Поля
    // ввода внутри — темнее карточки (утоплены): цвет Input почти совпадает
    // с SurfaceAlt, и без этого поле сливается с фоном.
    ImGui::PushID(c.GroupIndex++);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(EditorTheme::Role::SurfaceAlt));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Color(EditorTheme::Role::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui.SpacingLG, ui.SpacingMD));
    ImGui::BeginChild("##group", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.08f);
    ImGui::TextUnformatted(c.PendingTitle);
    ImGui::PopFont();
    if (c.PendingNote) {
        ImGui::PushTextWrapPos(0.0f);
        Sage::UI::TextSecondary("%s", c.PendingNote);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingXS));
}

// Две колонки строк. Широкая строка (Wide) таблицу закрывает: ячейка
// TableNextRow не охватывает обе колонки, и ряд кнопок профилей ужимался
// в половину карточки. Следующая обычная строка открывает таблицу заново.
void EnsureTable(Ctx& c, bool wide) {
    if (wide) {
        if (c.InTable) ImGui::EndTable();
        c.InTable = false;
        return;
    }
    if (c.InTable) return;
    // Имя — с номером, а не PushID: BeginTable кладёт свой ID на тот же стек,
    // и PopID сразу после снял бы его, а не наш.
    char id[24];
    std::snprintf(id, sizeof(id), "##rows%d", c.TableIndex++);
    c.InTable = ImGui::BeginTable(id, c.Narrow ? 1 : 2, ImGuiTableFlags_SizingStretchSame);
}
} // namespace

void BeginGroup(Ctx& c, const char* title, const char* note) {
    c.PendingTitle = title;
    c.PendingNote = note;
    c.GroupOpen = false;
    c.InTable = false;
    c.TableIndex = 0;
}

void FullWidth(Ctx& c) { EnsureTable(c, true); }

bool FreeBlock(Ctx& c, const char* label, const char* keywords) {
    if (!Matches(c, label, keywords) && !(c.PendingTitle && Matches(c, c.PendingTitle, nullptr))) return false;
    OpenPending(c);
    EnsureTable(c, true);
    ++c.Shown;
    return true;
}

void EndGroup(Ctx& c) {
    if (c.GroupOpen) {
        if (c.InTable) ImGui::EndTable();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, Sage::UI::Get().SpacingMD));
    }
    c.PendingTitle = nullptr;
    c.GroupOpen = false;
    c.InTable = false;
}

namespace {
// Метка возле подписи: целиком, если влезает в строку, иначе — значок.
void Mark(const char* text, EditorTheme::Role role, const char* icon, const char* tip, float reserve) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::SameLine(0.0f, 8.0f);
    const float w = ImGui::CalcTextSize(text).x + ui.SpacingSM * 2.0f;
    const float room = ImGui::GetContentRegionMax().x - reserve - ImGui::GetCursorPosX();
    if (w <= room) {
        Sage::UI::Badge(text, role);
    } else {
        const float s = ImGui::GetFontSize();
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec4 col = EditorTheme::Color(role);
        EditorIcons::Overlay(ImGui::GetWindowDrawList(), p.x, p.y + (h - s) * 0.5f, s, icon,
                             glm::vec3(col.x, col.y, col.z));
        ImGui::Dummy(ImVec2(s, h));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
}
} // namespace

bool Row(Ctx& c, const char* label, const RowOpts& o, bool* reset) {
    if (reset) *reset = false;
    if (!Matches(c, label, o.Keywords)) return false;
    OpenPending(c);
    EnsureTable(c, o.Wide);
    ++c.Shown;
    if (c.InTable) ImGui::TableNextColumn();
    ImGui::PushID(label);
    // Строка подписи: подпись, метки применения и цены, сброс справа.
    ImGui::BeginDisabled(o.Disabled);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (o.Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", o.Tooltip);
    if (o.Modified) {
        // Точка «изменено» — тем же акцентом, что у несохранённой вкладки.
        ImGui::SameLine(0.0f, 6.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 3.0f, p.y + h * 0.5f), 3.0f,
                                                    EditorTheme::Color32(EditorTheme::Role::Accent));
        ImGui::Dummy(ImVec2(6.0f, h));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Changed from the default"));
    }
    // Метки цены и применения — сколько влезет до кнопки сброса. В узкой
    // колонке длинная метка уезжала в соседнюю и закрывала её подпись, поэтому
    // не влезшая сворачивается в значок с той же подсказкой.
    const float reserve = (o.Modified && reset) ? ImGui::GetFrameHeight() + 4.0f : 0.0f;
    if (o.Price == Cost::Heavy)
        Mark(T("heavy"), EditorTheme::Role::Warn, "warn", T("Noticeably lowers the frame rate"), reserve);
    if (o.When != Apply::Live)
        Mark(o.When == Apply::Launch ? T("on game start") : T("after restart"), EditorTheme::Role::TextDim, "clock",
             o.When == Apply::Launch ? T("The game window is created with this value")
                                     : T("Takes effect after the program restarts"),
             reserve);
    if (o.Modified && reset) {
        const float bw = ImGui::GetFrameHeight();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - bw));
        if (EditorIcons::IconOnlyButton("refresh", T("Reset to default"))) *reset = true;
    }
    ImGui::EndDisabled();
    if (o.Disabled && o.DisabledWhy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", o.DisabledWhy);
    ImGui::BeginDisabled(o.Disabled);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return true;
}

void EndRow(Ctx& c, const RowOpts& o) {
    (void)o;
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0.0f, Sage::UI::Get().SpacingXS));
    ImGui::PopID();
    (void)c;
}

bool Toggle(const char* id, bool* v, const char* caption) {
    const float h = ImGui::GetFrameHeight() * 0.8f;
    const float w = h * 1.8f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##toggle", ImVec2(w, ImGui::GetFrameHeight()));
    if (pressed) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();
    const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = *v ? EditorTheme::Color(EditorTheme::Role::Accent) : EditorTheme::Color(EditorTheme::Role::Input);
    if (hovered && !disabled) bg = ImVec4(bg.x * 1.12f, bg.y * 1.12f, bg.z * 1.12f, bg.w);
    if (disabled) bg.w *= 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(bg), h * 0.5f);
    const float r = h * 0.5f - 2.5f;
    const float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(245, 247, 250, disabled ? 140 : 255), 20);
    if (ImGui::IsItemFocused())
        dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + w + 2, p.y + h + 2),
                    EditorTheme::Color32(EditorTheme::Role::Accent), h * 0.5f + 2, 0, 1.5f);
    ImGui::PopID();
    if (caption) {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        // Перенос по ширине ячейки: длинная подпись иначе уезжает в соседнюю
        // колонку и закрывает её переключатель.
        ImGui::PushTextWrapPos(0.0f);
        Sage::UI::TextSecondary("%s", caption);
        ImGui::PopTextWrapPos();
    }
    return pressed;
}

bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt, float step) {
    // Ползунок, а значение — внутри него: ImGui пишет число по центру, и
    // Ctrl+щелчок (или двойной щелчок) переключает в ввод числа.
    const bool ch = ImGui::SliderFloat(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (ch && step > 0.0f) *v = std::round(*v / step) * step;
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("%s", T("Ctrl+click to type a value"));
    return ch;
}

bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt) {
    const bool ch = ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("%s", T("Ctrl+click to type a value"));
    return ch;
}

void Note(const char* text) {
    const float s = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec4 dim = EditorTheme::Color(EditorTheme::Role::TextDim);
    EditorIcons::Overlay(ImGui::GetWindowDrawList(), p.x, p.y + 2.0f, s, "info", glm::vec3(dim.x, dim.y, dim.z));
    ImGui::Dummy(ImVec2(s, s));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::PushTextWrapPos(0.0f);
    Sage::UI::TextSecondary("%s", text);
    ImGui::PopTextWrapPos();
}

} // namespace sage::editor::settingsui
