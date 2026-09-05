#include "ui/UI.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"

#include "EditorIcons.h"

namespace Sage::UI {

using EditorTheme::Role;

namespace {

// Цвет по роли — короткий псевдоним, чтобы код компонентов читался.
ImVec4 C(Role r) { return EditorTheme::Color(r); }

ImVec4 Shade(const ImVec4& c, float k) {
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    return ImVec4(cl(c.x * k), cl(c.y * k), cl(c.z * k), c.w);
}

// Три цвета кнопки + цвет её текста по виду. Одно место, где решается, как
// выглядит каждая кнопка редактора.
struct ButtonPaint {
    ImVec4 Bg, Hovered, Active, Text;
    bool Border = false;
};

ButtonPaint PaintFor(ButtonStyle style) {
    ButtonPaint p;
    switch (style) {
        case ButtonStyle::Primary:
            p.Bg = C(Role::Accent);
            p.Hovered = C(Role::AccentHover);
            p.Active = C(Role::AccentActive);
            p.Text = C(Role::TextOnAccent);
            break;
        case ButtonStyle::Danger:
            p.Bg = C(Role::Danger);
            p.Hovered = Shade(C(Role::Danger), 1.12f);
            p.Active = Shade(C(Role::Danger), 0.86f);
            p.Text = C(Role::TextOnAccent);
            break;
        case ButtonStyle::Success:
            p.Bg = C(Role::Ok);
            p.Hovered = Shade(C(Role::Ok), 1.12f);
            p.Active = Shade(C(Role::Ok), 0.86f);
            p.Text = C(Role::TextOnAccent);
            break;
        case ButtonStyle::Ghost:
            // Без заливки в покое: действие «на полях» не должно спорить с
            // главным. Появляется только под курсором.
            p.Bg = ImVec4(0, 0, 0, 0);
            p.Hovered = C(Role::Hover);
            p.Active = C(Role::Elevated);
            p.Text = C(Role::TextDim);
            break;
        case ButtonStyle::Secondary:
        default:
            p.Bg = C(Role::Input);
            p.Hovered = C(Role::Elevated);
            p.Active = C(Role::LineStrong);
            p.Text = C(Role::Text);
            p.Border = true;
            break;
    }
    return p;
}

// Ширина колонки подписей в сетке свойств. Не константа: узкий инспектор
// обязан отдавать значению больше места, чем подписи (см. BeginProperties).
float& LabelWidthRef() {
    static float w = 0.0f;
    return w;
}

} // namespace

bool Button(const char* label, ButtonStyle style, const ImVec2& size) {
    const Style& ui = Get();
    const ButtonPaint p = PaintFor(style);
    ImGui::PushStyleColor(ImGuiCol_Button, p.Bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.Hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.Active);
    ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
    ImGui::PushStyleColor(ImGuiCol_Border, p.Border ? C(Role::Line) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, p.Border ? ui.BorderWidth : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ui.PaddingControl * 1.5f, ui.PaddingControlY));
    // Высота у всех кнопок одна — иначе ряд кнопок «пляшет» по вертикали.
    ImVec2 sz = size;
    if (sz.y == 0.0f) sz.y = ui.ControlHeight;
    const bool pressed = ImGui::Button(label, sz);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return pressed;
}

bool IconButton(const char* icon, const char* tooltip, bool active, const char* shortcut) {
    const Style& ui = Get();
    ImGui::PushStyleColor(ImGuiCol_Button, active ? C(Role::AccentMuted) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C(Role::Hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, C(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? C(Role::Accent) : C(Role::TextDim));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.PaddingControlY, ui.PaddingControlY));
    // Квадратная: одинаковая ширина и высота держат ряд иконок ровным.
    const bool pressed = EditorIcons::IconOnlyButton(icon, nullptr, active);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (tooltip) Tooltip(tooltip, shortcut);
    return pressed;
}

bool SearchField(const char* id, char* buf, size_t bufSize, const char* hint, float width) {
    const Style& ui = Get();
    ImGui::PushID(id);
    if (width > 0.0f) ImGui::SetNextItemWidth(width);
    else ImGui::SetNextItemWidth(-FLT_MIN);

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float iconRoom = ui.IconSize + ui.SpacingSM;
    // Место под иконку — отступом самого поля, а не Dummy рядом: иначе иконка
    // и рамка живут отдельно и разъезжаются при любой смене размера.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ui.PaddingControl + iconRoom, ui.PaddingControlY));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, C(Role::Input));
    const bool changed = ImGui::InputTextWithHint("##field", hint, buf, bufSize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    const float h = ImGui::GetItemRectSize().y;
    EditorIcons::Overlay(start.x + ui.PaddingControl, start.y + (h - ui.IconSize) * 0.5f,
                         ui.IconSize, "search",
                         glm::vec3(C(Role::TextFaint).x, C(Role::TextFaint).y, C(Role::TextFaint).z));
    ImGui::PopID();
    return changed;
}

bool Section(const char* label, bool defaultOpen) {
    const Style& ui = Get();
    ImGui::PushStyleColor(ImGuiCol_Header, C(Role::SurfaceAlt));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, C(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, C(Role::Elevated));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.PaddingControl, ui.PaddingControlY));
    const bool open = ImGui::CollapsingHeader(
        label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return open;
}

void PanelHeader(const char* title) {
    const Style& ui = Get();
    ImGui::PushStyleColor(ImGuiCol_Text, C(Role::Text));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ui.SpacingSM, ui.HeaderHeight - ImGui::GetTextLineHeight()));
}

void Separator() {
    const Style& ui = Get();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingXS));
    ImGui::PushStyleColor(ImGuiCol_Separator, C(Role::Line));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingXS));
}

void BeginProperties(const char* id) {
    const Style& ui = Get();
    // Колонка подписей адаптивная: на широкой панели она фиксирована (значения
    // начинаются на одной координате), на узкой ужимается до 40 % ширины —
    // иначе полю остаётся полтора сантиметра и в нём не видно числа.
    const float avail = ImGui::GetContentRegionAvail().x;
    LabelWidthRef() = ImMin(ui.LabelColumn, avail * 0.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, ui.SpacingXS));
    ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, LabelWidthRef());
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
}

void EndProperties() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void PropertyLabel(const char* label, const char* tooltip) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextDim));
    // Длинная подпись УКОРАЧИВАЕТСЯ, а не расталкивает колонку: сетка держится
    // на том, что значения начинаются в одной координате всегда.
    const std::string shown = Truncate(label, LabelWidthRef() - Get().SpacingSM);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    if (shown != label || tooltip) Tooltip(tooltip ? tooltip : label);
    ImGui::TableSetColumnIndex(1);
}

bool PropertyVec3(const char* id, float v[3], float speed, const char* format) {
    const Style& ui = Get();
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    // Три равных поля с одинаковыми промежутками. Ширина считается один раз и
    // округляется вниз, чтобы третье поле не вылезало за край панели.
    const float gap = ui.SpacingXS;
    const float w = ImFloor((avail - gap * 2.0f) / 3.0f);
    bool changed = false;
    static const char* kAxis[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(i == 2 ? ImMax(w, avail - (w + gap) * 2.0f) : w);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ui.PaddingControlY, ui.PaddingControlY));
        changed |= ImGui::DragFloat("##v", &v[i], speed, 0.0f, 0.0f, format);
        ImGui::PopStyleVar();
        // Буква оси — ВНУТРИ поля слева, приглушённо: отдельной подписью она
        // съедала бы треть ширины строки, а без неё три одинаковых поля не
        // отличить друг от друга.
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(p0.x + ui.SpacingXS, p0.y + (p1.y - p0.y - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(C(Role::TextFaint)), kAxis[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

void Badge(const char* text, Role role) {
    const Style& ui = Get();
    const ImVec4 col = C(role);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x, col.y, col.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x, col.y, col.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x, col.y, col.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.SpacingSM, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ui.CornerRadiusSmall);
    ImGui::SmallButton(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void TextSecondary(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextDim));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void EmptyState(const char* title, const char* hint) {
    const Style& ui = Get();
    // Пустая панель без объяснения читается как поломка. Текст ставится по
    // центру доступного места, а не в угол: угол выглядит как забытая надпись.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float titleW = ImGui::CalcTextSize(title).x;
    ImGui::Dummy(ImVec2(0.0f, ImMax(avail.y * 0.3f, ui.SpacingXL)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax((avail.x - titleW) * 0.5f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextDim));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (hint && *hint) {
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingXS));
        const float hintW = ImMin(ImGui::CalcTextSize(hint).x, avail.x - ui.SpacingLG * 2.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax((avail.x - hintW) * 0.5f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextFaint));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + hintW);
        ImGui::TextUnformatted(hint);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

void Tooltip(const char* text, const char* shortcut) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    const Style& ui = Get();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui.SpacingSM, ui.SpacingXS));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    if (shortcut && *shortcut) {
        ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextFaint));
        ImGui::TextUnformatted(shortcut);
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

std::string Truncate(const char* text, float maxWidth) {
    if (!text || !*text) return {};
    if (maxWidth <= 0.0f) return text;
    if (ImGui::CalcTextSize(text).x <= maxWidth) return text;

    const char* kEllipsis = "...";
    const float dots = ImGui::CalcTextSize(kEllipsis).x;
    std::string out;
    // По ОДНОМУ СИМВОЛУ UTF-8, а не по байту: обрезав кириллицу посередине
    // буквы, получим не «короче», а мусор в конце строки.
    const char* p = text;
    while (*p) {
        const char* next = p + 1;
        while ((*next & 0xC0) == 0x80) ++next;   // хвостовые байты символа
        std::string probe = out + std::string(p, next);
        if (ImGui::CalcTextSize(probe.c_str()).x + dots > maxWidth) break;
        out = std::move(probe);
        p = next;
    }
    if (out.empty()) return kEllipsis;
    return out + kEllipsis;
}

} // namespace Sage::UI
