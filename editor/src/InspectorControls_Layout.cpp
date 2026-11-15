// ---------------------------------------------------------------------------
// EDITOR UI KIT: КОМПОНОВКА — строка свойства (подпись слева), группа,
// разделители, разделы и вложенные группы.
//
// Шапка раздела — ОДНА на весь редактор: инспектор сцены (через
// EditorTheme::SectionHeader) и инспектор интерфейса (BeginSection) рисуют её
// этим же кодом — значок, заголовок, шеврон Tabler, справа «сброс» и
// «удалить». Тело раздела — без своей карточки: у обоих инспекторов поля
// начинаются с одного края, вложенность даёт только SubSection.
// ---------------------------------------------------------------------------
#include "InspectorControls.h"
#include "InspectorControlsDetail.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor::ictl {

using detail::K;
using EditorTheme::Role;
using EditorTheme::State;

namespace {

glm::vec3 Vec3(const ImVec4& c) { return glm::vec3(c.x, c.y, c.z); }

// Открытые вложенные группы: где начинается линия вложенности.
struct GroupMark {
    ImVec2 Start;
};
std::vector<GroupMark>& Groups() {
    static std::vector<GroupMark> s;
    return s;
}

} // namespace

float LabelWidth() {
    // Колонка подписей — доля ширины панели: одинаковая у всех строк уровня.
    return std::clamp(ImGui::GetContentRegionAvail().x * K().LabelRatio, K().LabelMin, K().LabelMax);
}

void Label(const char* text, float minField) {
    const float x0 = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = LabelWidth();
    // «Имя:» — подпись свойства с двоеточием; часть после «##» — не подпись.
    const char* end = text + std::strlen(text);
    if (const char* hash = std::strstr(text, "##")) end = hash;
    std::string shown(text, end);
    // Без видимой подписи («##v» в ячейке таблицы) — только ширина поля.
    if (shown.empty()) {
        ImGui::SetNextItemWidth(avail);
        return;
    }
    if (shown.back() != ':') shown += ':';
    // Длинная подпись (или узкое поле) — НАД полем, целиком: обрезанная
    // посередине подпись не читается, а подсказку никто не ищет.
    if (ImGui::CalcTextSize(shown.c_str()).x > w - K().LabelGap || avail - w < std::max(60.0f, minField)) {
        ImGui::TextUnformatted(shown.c_str());
        ImGui::SetNextItemWidth(avail);
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(shown.c_str());
    ImGui::SameLine(x0 + w);
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
}

void GroupTitle(const char* title) {
    // Подпись группы над её строками: «Картинка:», «Фон:». Небольшой отступ
    // сверху отделяет группу от предыдущей; до содержимого — обычный шаг.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    std::string shown = title;
    if (!shown.empty() && shown.back() != ':') shown += ':';
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextDim));
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
}

void BeginGroup(const char* title) {
    if (title && *title) GroupTitle(title);
    ImGui::BeginGroup();
}

void EndGroup() {
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, K().GroupGap));
}

void Separator(bool vertical) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = K().SeparatorThickness;
    const ImU32 c = EditorTheme::Color32(Role::Line);
    if (vertical) {
        // В строке: между группами одного ряда, высотой с поле.
        ImGui::SameLine(0.0f, K().TagGap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        dl->AddRectFilled(ImVec2(p.x, p.y + K().SeparatorInset), ImVec2(p.x + t, p.y + h - K().SeparatorInset), c);
        ImGui::Dummy(ImVec2(t, h));
        ImGui::SameLine(0.0f, K().TagGap);
        return;
    }
    const float gap = std::floor(K().GroupGap * 0.5f);
    ImGui::Dummy(ImVec2(0.0f, gap));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + t), c);
    ImGui::Dummy(ImVec2(0.0f, gap + t));
}

void SeparatorText(const char* text) {
    // Подпись части группы и тонкая линия до края — той же толщины и цвета,
    // что все разделители kit.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    std::string shown = text;
    if (const size_t hash = shown.find("##"); hash != std::string::npos) shown.resize(hash);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, EditorTheme::Color32(Role::TextDim), shown.c_str());
    const float lx = p.x + ts.x + (ts.x > 0.0f ? K().LabelGap : 0.0f);
    if (lx < p.x + w)
        dl->AddRectFilled(ImVec2(lx, std::floor(p.y + ts.y * 0.5f)),
                          ImVec2(p.x + w, std::floor(p.y + ts.y * 0.5f) + K().SeparatorThickness),
                          EditorTheme::Color32(Role::Line));
    ImGui::Dummy(ImVec2(w, ts.y));
}

std::string UpperTitle(const char* title) {
    std::string upper;
    for (const char* c = title; *c; ++c) {
        if (c[0] == '#' && c[1] == '#') break;   // «###id» — не подпись
        upper += (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
    }
    // Кириллица — ЗАГЛАВНЫМИ тоже: строчные из UTF-8 по двум байтам.
    for (size_t i = 0; i + 1 < upper.size(); ++i) {
        unsigned char a = (unsigned char)upper[i], b = (unsigned char)upper[i + 1];
        if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) { upper[i + 1] = (char)(b - 0x20); ++i; }
        else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { upper[i] = (char)0xD0; upper[i + 1] = (char)(b + 0x20); ++i; }
        else if (a == 0xD1 && b == 0x91) { upper[i] = (char)0xD0; upper[i + 1] = (char)0x81; ++i; }
        else if (a >= 0xC0) ++i;
    }
    return upper;
}

bool SectionHeader(const char* id, const char* icon, const char* title, bool* open, const HeaderActions& act) {
    if (act.Reset) *act.Reset = false;
    if (act.Remove) *act.Remove = false;
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    const float side = ImGui::GetFrameHeight();
    const float h = side + K().HeaderExtra;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const int slots = (act.Remove ? 1 : 0) + (act.Reset ? 1 : 0);
    const float buttons = slots > 0 ? slots * side + (slots - 1) * 2.0f + 6.0f : 0.0f;

    // Шапка нажимается ВСЯ, кроме кнопок справа: они — свои элементы, и
    // щелчок по ним не сворачивает раздел.
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##head", ImVec2(std::max(1.0f, w - buttons), h));
    if (clicked) *open = !*open;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && act.Hint && *act.Hint) ImGui::SetTooltip("%s", act.Hint);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = K().Rounding;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), EditorTheme::Color32(hovered ? Role::Hover : Role::SurfaceAlt), r);
    dl->AddRectFilled(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x + K().HeaderAccent, p.y + h - 4.0f),
                      EditorTheme::Color32(Role::Accent), K().HeaderAccent * 0.5f);
    float x = p.x + K().HeaderPadX;
    const float is = K().IconSize;
    if (icon && *icon) {
        EditorIcons::Overlay(dl, x, p.y + std::floor((h - is) * 0.5f), is, icon);
        x += is + K().LabelGap;
    }
    const std::string upper = UpperTitle(title);
    dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), EditorTheme::Color32(Role::Text),
                upper.c_str());
    // Шеврон — у правого края, перед кнопками: вниз — раскрыт, вправо — свёрнут.
    const float chev = std::floor(side * K().ChevronIcon);
    EditorIcons::Overlay(dl, std::floor(p.x + w - buttons - chev - 6.0f), std::floor(p.y + (h - chev) * 0.5f), chev,
                         *open ? "chevron-down" : "chevron-right",
                         Vec3(EditorTheme::IconTint(hovered ? State::Hover : State::Normal)));

    // Кнопки справа — по центру высоты шапки; вид — тот же, что у всех
    // значковых кнопок kit.
    const float by = p.y + (h - side) * 0.5f;
    float bx = p.x + w - buttons + 4.0f;
    // СБРОС — слева от корзины: действие частое и безопасное (Ctrl+Z).
    if (act.Reset) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        *act.Reset = EditorIcons::IconOnlyButton("refresh", T("Reset to default values (a single field: right-click it)"));
        ImGui::PopStyleColor();
        bx += side + 2.0f;
    }
    if (act.Remove) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        const ImVec4 danger = EditorTheme::Color(Role::Danger);
        // Иконка красная всегда, подложка — только под курсором: десяток
        // красных квадратов превратил бы инспектор в предупреждение.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(danger.x * 0.85f, danger.y * 0.85f, danger.z * 0.85f, danger.w));
        const glm::vec3 hoverTint = Vec3(EditorTheme::Color(Role::TextOnAccent));
        *act.Remove = EditorIcons::IconOnlyButton("trash", T("Remove this component"), false, Vec3(danger), &hoverTint);
        ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    // Курсор — под шапку, с небольшим зазором до свойств раздела.
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0.0f, *open ? 2.0f : 0.0f));
    return clicked;
}

bool BeginSection(const char* id, const char* title, bool defaultOpen, const char* icon) {
    const std::string key = std::string("inspector.section.") + id;
    bool open = prefs::GetBool(key, defaultOpen);
    if (SectionHeader(id, icon, title, &open)) prefs::SetBool(key, open);
    return open;
}

void EndSection() { ImGui::Dummy(ImVec2(0.0f, K().SectionGap)); }

bool SubSection(const char* id, const char* title, bool defaultOpen) {
    const std::string key = std::string("inspector.sub.") + id;
    bool open = prefs::GetBool(key, defaultOpen);
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##grp", ImVec2(w, h))) {
        open = !open;
        prefs::SetBool(key, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float indent = K().Indent;
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), EditorTheme::Color32(Role::Hover), K().Rounding);
    // Шеврон в квадрате шириной Indent: подпись группы начинается там же,
    // где начнётся её содержимое.
    const float chev = std::floor(h * 0.5f);
    EditorIcons::Overlay(dl, std::floor(p.x + (indent - chev) * 0.5f), std::floor(p.y + (h - chev) * 0.5f), chev,
                         open ? "chevron-down" : "chevron-right",
                         Vec3(EditorTheme::IconTint(hovered ? State::Hover : State::Normal)));
    // Подпись — до «###»: у раздела может быть переменный заголовок и
    // постоянный ID.
    std::string shown = title;
    if (const size_t hash = shown.find("##"); hash != std::string::npos) shown.resize(hash);
    dl->AddText(ImVec2(p.x + indent + 2.0f, p.y + ImGui::GetStyle().FramePadding.y),
                EditorTheme::Color32(hovered ? Role::Text : Role::TextDim), shown.c_str());
    if (!open) return false;
    Groups().push_back({ImVec2(p.x + std::floor(indent * 0.5f), p.y + h)});
    ImGui::Indent(indent);
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    return true;
}

void EndSubSection() {
    ImGui::Unindent(K().Indent);
    if (Groups().empty()) return;
    const GroupMark m = Groups().back();
    Groups().pop_back();
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(m.Start.x, m.Start.y + 2.0f),
                                              ImVec2(m.Start.x + K().SeparatorThickness, bottom),
                                              EditorTheme::Color32(Role::Line));
    ImGui::Dummy(ImVec2(0.0f, K().GroupGap * 0.5f));
}

} // namespace sage::editor::ictl
