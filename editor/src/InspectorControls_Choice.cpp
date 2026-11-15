// ---------------------------------------------------------------------------
// EDITOR UI KIT: ВЫБОР И ДЕЙСТВИЯ — выпадающий список, группа кнопок, кнопка.
//
// Список — одно цельное поле: значение и шеврон Tabler внутри одной рамки,
// нажимается всё поле. Раньше справа торчала отдельная квадратная кнопка со
// стрелкой-треугольником: поле читалось как «текст и ещё что-то рядом».
//
// Группа кнопок одной операции (выравнивание, якоря) — общая рамка с равными
// сегментами через тонкие разделители, а не ряд отдельных квадратных плиток.
// ---------------------------------------------------------------------------
#include "InspectorControls.h"
#include "InspectorControlsDetail.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorIcons.h"
#include "EditorTheme.h"

namespace sage::editor::ictl {

using detail::K;
using EditorTheme::State;

namespace {

glm::vec3 Vec3(const ImVec4& c) { return glm::vec3(c.x, c.y, c.z); }

// Видимая часть подписи: до «##».
std::string Visible(const char* text) {
    if (!text) return {};
    const char* end = text + std::strlen(text);
    if (const char* hash = std::strstr(text, "##")) end = hash;
    return std::string(text, end);
}

} // namespace

bool BeginDropdown(const char* id, const char* preview, DropLook look) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float chev = std::floor(h * K().ChevronIcon);
    const float padX = style.FramePadding.x;
    if (!preview) preview = "";
    // Видимая подпись («Clip», «Тип») — слева, в колонке подписей, как у
    // всех полей kit; сам список — без подписи.
    if (!Visible(id).empty()) Label(id);
    if (look == DropLook::Compact)
        ImGui::SetNextItemWidth(std::floor(ImGui::CalcTextSize(preview).x + padX * 2.0f + chev + K().TagGap));
    const float w = std::max(chev + padX * 2.0f, ImGui::CalcItemWidth());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Метка списка ВСЕГДА скрыта: подпись строки рисует Label, а не список.
    std::string label = id;
    if (label.find("##") == std::string::npos) label = "##" + label;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(EditorTheme::ControlFill(State::Normal)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                          ImGui::ColorConvertU32ToFloat4(EditorTheme::ControlFill(State::Hover)));
    // Без стрелки ImGui: шеврон рисуем сами, внутри того же поля.
    const bool open = ImGui::BeginCombo(label.c_str(), "", ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleColor(2);
    const bool hovered = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + h));
    const State st = open ? State::Focused : hovered ? State::Hover : State::Normal;
    // Значение — обрезается перед шевроном, а не наезжает на него.
    dl->PushClipRect(p, ImVec2(p.x + w - chev - padX, p.y + h), true);
    dl->AddText(ImVec2(p.x + padX, p.y + style.FramePadding.y), EditorTheme::Color32(EditorTheme::Role::Text), preview);
    dl->PopClipRect();
    EditorIcons::Overlay(dl, std::floor(p.x + w - padX * 0.5f - chev), std::floor(p.y + (h - chev) * 0.5f), chev,
                         open ? "chevron-up" : "chevron-down", Vec3(EditorTheme::IconTint(st)));
    if (open) dl->AddRect(p, ImVec2(p.x + w, p.y + h), EditorTheme::ControlBorder(State::Focused), K().Rounding);
    return open;
}

void EndDropdown() { ImGui::EndCombo(); }

bool DropButton(const char* id, const char* preview, bool open, const char* tooltip) {
    // Тот же вид, что у списка (поле, значение, шеврон Tabler внутри), но
    // окно открывает вызывающий — своё, а не список пунктов.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float chev = std::floor(h * K().ChevronIcon);
    const float padX = style.FramePadding.x;
    if (!preview) preview = "";
    const float w = std::max(chev + padX * 2.0f, ImGui::CalcItemWidth());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && tooltip && *tooltip && !open) ImGui::SetTooltip("%s", tooltip);
    const State st = open ? State::Focused : hovered ? State::Hover : State::Normal;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    detail::Frame(dl, p, ImVec2(p.x + w, p.y + h), st == State::Focused ? State::Normal : st);
    dl->PushClipRect(p, ImVec2(p.x + w - chev - padX, p.y + h), true);
    dl->AddText(ImVec2(p.x + padX, p.y + style.FramePadding.y), EditorTheme::Color32(EditorTheme::Role::Text), preview);
    dl->PopClipRect();
    EditorIcons::Overlay(dl, std::floor(p.x + w - padX * 0.5f - chev), std::floor(p.y + (h - chev) * 0.5f), chev,
                         open ? "chevron-up" : "chevron-down", Vec3(EditorTheme::IconTint(st)));
    if (open) dl->AddRect(p, ImVec2(p.x + w, p.y + h), EditorTheme::ControlBorder(State::Focused), K().Rounding);
    return clicked;
}

bool Dropdown(const char* label, int* current, const char* const* items, int count, DropLook look) {
    ImGui::PushID(label);
    Label(label);
    const bool changed = Combo("##dd", current, items, count, look);
    ImGui::PopID();
    return changed;
}

bool Combo(const char* id, int* current, const char* const* items, int count, DropLook look) {
    const char* preview = (*current >= 0 && *current < count) ? items[*current] : "";
    bool changed = false;
    if (BeginDropdown(id, preview, look)) {
        for (int i = 0; i < count; ++i) {
            const bool selected = i == *current;
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i], selected) && !selected) {
                *current = i;
                changed = true;
            }
            ImGui::PopID();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        EndDropdown();
    }
    return changed;
}

int ButtonGroup(const char* id, const GroupItem* items, int count, ButtonLook look, int columns, float width) {
    if (count <= 0) return -1;
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const int cols = columns > 0 ? std::min(columns, count) : count;
    const int rows = (count + cols - 1) / cols;
    // Значок-кнопка — того же размера, что одиночная IconOnlyButton (доля
    // высоты поля); с подписью — кегля текста.
    const float icon = look == ButtonLook::IconOnly ? std::floor(h * 0.64f) : std::floor(K().IconSize);
    const float pad = look == ButtonLook::Compact ? K().ButtonPadXCompact : K().ButtonPadX;
    // Сегменты РАВНЫЕ: по самому широкому содержимому (или квадраты).
    float seg = h;
    if (look != ButtonLook::IconOnly) {
        for (int i = 0; i < count; ++i) {
            const std::string text = Visible(items[i].Text);
            float c = text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str()).x;
            if (items[i].Icon) c += icon + (text.empty() ? 0.0f : K().TagGap);
            seg = std::max(seg, std::floor(c + pad * 2.0f));
        }
    }
    // Не шире строки: подписи длинные — сегменты делят ширину поровну, а
    // подпись обрезается в своём сегменте, а не уезжает за край панели.
    const float avail = ImGui::GetContentRegionAvail().x;
    if (width <= 0.0f && look != ButtonLook::IconOnly && seg * (float)cols > avail) width = avail;
    if (width > 0.0f) seg = std::floor(width / (float)cols);
    else width = seg * (float)cols;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    detail::Frame(dl, p, ImVec2(p.x + width, p.y + h * (float)rows), State::Normal);
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        const GroupItem& it = items[i];
        const int r = i / cols, c = i % cols;
        const ImVec2 a(p.x + seg * (float)c, p.y + h * (float)r);
        const float sw = c + 1 == cols ? width - seg * (float)c : seg;
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::BeginDisabled(!it.Enabled);
        if (ImGui::InvisibleButton("##b", ImVec2(sw, h))) clicked = i;
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        // Подсказка — и у погашенной кнопки: она объясняет, почему кнопка
        // сейчас недоступна.
        if (it.Tooltip && *it.Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", it.Tooltip);
        ImGui::EndDisabled();
        ImGui::PopID();
        const State st = !it.Enabled ? State::Disabled
                         : held      ? State::Active
                         : it.Active ? State::Active
                         : hovered   ? State::Hover
                                     : State::Normal;
        // Подсветка — только этого сегмента; крайние повторяют углы рамки.
        ImDrawFlags corners = 0;
        if (r == 0 && c == 0) corners |= ImDrawFlags_RoundCornersTopLeft;
        if (r == 0 && c + 1 == cols) corners |= ImDrawFlags_RoundCornersTopRight;
        if (r + 1 == rows && c == 0) corners |= ImDrawFlags_RoundCornersBottomLeft;
        if (r + 1 == rows && (c + 1 == cols || i + 1 == count)) corners |= ImDrawFlags_RoundCornersBottomRight;
        if (const ImU32 fill = EditorTheme::SegmentFill(st == State::Disabled ? State::Normal : st))
            dl->AddRectFilled(a, ImVec2(a.x + sw, a.y + h), fill, K().Rounding,
                              corners ? corners : ImDrawFlags_RoundCornersNone);
        const State tint = it.Active ? State::Active : hovered ? State::Hover : st;
        const std::string text = look == ButtonLook::IconOnly ? std::string() : Visible(it.Text);
        const float tw = text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str()).x;
        const float cw = (it.Icon ? icon : 0.0f) + tw + (it.Icon && tw > 0.0f ? K().TagGap : 0.0f);
        float x = std::floor(a.x + std::max(K().ButtonPadXCompact, (sw - cw) * 0.5f));
        dl->PushClipRect(a, ImVec2(a.x + sw - 2.0f, a.y + h), true);
        if (it.Icon) {
            EditorIcons::Overlay(dl, x, std::floor(a.y + (h - icon) * 0.5f), icon, it.Icon,
                                 Vec3(EditorTheme::IconTint(tint)));
            x += icon + K().TagGap;
        }
        if (tw > 0.0f)
            dl->AddText(ImVec2(x, a.y + ImGui::GetStyle().FramePadding.y),
                        EditorTheme::Color32(!it.Enabled ? EditorTheme::Role::TextFaint
                                             : it.Active ? EditorTheme::Role::Accent
                                                         : EditorTheme::Role::Text),
                        text.c_str());
        dl->PopClipRect();
        if (c > 0) detail::InnerSeparator(dl, a.x, a.y, h);
        if (r > 0 && c == 0) detail::InnerSeparatorH(dl, p.x, a.y, width);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h * (float)rows));
    ImGui::PopID();
    return clicked;
}

bool Button(const char* icon, const char* text, const char* tooltip, ButtonLook look, float width, bool active) {
    GroupItem it;
    it.Icon = icon;
    it.Text = text;
    it.Tooltip = tooltip;
    it.Active = active;
    // ID — по подписи целиком (с «##»), иначе по значку.
    return ButtonGroup(text && *text ? text : (icon ? icon : "##btn"), &it, 1, look, 0, width) == 0;
}

} // namespace sage::editor::ictl
