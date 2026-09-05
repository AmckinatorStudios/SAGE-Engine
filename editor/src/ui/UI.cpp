#include "ui/UI.h"

#include <cstdarg>
#include <cstdio>
#include <cctype>
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

bool FilterChip(const char* label, bool* on, int count, Role role, const char* icon) {
    const Style& ui = Get();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    char countBuf[16] = {0};
    if (count >= 0) std::snprintf(countBuf, sizeof(countBuf), "%d", count);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    const ImVec2 countSize = countBuf[0] ? ImGui::CalcTextSize(countBuf) : ImVec2(0, 0);
    const float iconW = icon ? ui.IconSize + ui.SpacingXS : 0.0f;
    const float w = ui.PaddingControl * 2.0f + iconW + labelSize.x +
                    (countBuf[0] ? ui.SpacingSM + countSize.x : 0.0f);
    const float h = ui.ControlHeight;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemClicked();
    if (pressed) *on = !*on;

    const ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
    const ImVec4 tint = C(role);
    // Включённая фишка заливается своим цветом слабо, а обводится сильно:
    // сильная заливка под текстом того же цвета убивает контраст, а обводка
    // держит форму и на светлой теме, где слабой заливки почти не видно.
    if (*on) {
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.16f)),
                          ui.CornerRadius);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.55f)),
                    ui.CornerRadius, 0, ui.BorderWidth);
    } else if (hovered) {
        dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Hover), ui.CornerRadius);
    } else {
        dl->AddRect(p0, p1, EditorTheme::Color32(Role::Line), ui.CornerRadius, 0, ui.BorderWidth);
    }

    // Выключенная фишка приглушена, но НЕ до нечитаемости: её всё ещё надо
    // прочитать, чтобы включить обратно.
    const ImU32 fg = *on ? ImGui::GetColorU32(tint)
                         : EditorTheme::Color32Alpha(Role::TextDim, hovered ? 1.0f : 0.75f);
    float x = p0.x + ui.PaddingControl;
    if (icon) {
        EditorIcons::Overlay(x, p0.y + (h - ui.IconSize) * 0.5f, ui.IconSize, icon,
                             glm::vec3(tint.x, tint.y, tint.z) * (*on ? 1.0f : 0.7f));
        x += ui.IconSize + ui.SpacingXS;
    }
    dl->AddText(ImVec2(x, p0.y + (h - labelSize.y) * 0.5f), fg, label);
    if (countBuf[0]) {
        dl->AddText(ImVec2(p1.x - ui.PaddingControl - countSize.x, p0.y + (h - countSize.y) * 0.5f),
                    *on ? fg : EditorTheme::Color32Alpha(Role::TextFaint, 1.0f), countBuf);
    }
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

// Цвета осей — ТЕ ЖЕ, что у осей сетки в трёхмерном виде (sage::render::
// GridSettings). Совпадение не косметическое: человек тянет за красную стрелку
// в сцене и должен без раздумий понимать, какое из трёх полей инспектора
// сейчас меняется. Разойдись эти два красных — и подпись начнёт врать.
static const ImVec4 kAxisColor[3] = {
    ImVec4(0.85f, 0.40f, 0.42f, 1.0f), // X — как XAxisColor сетки, чуть светлее
    ImVec4(0.48f, 0.76f, 0.44f, 1.0f), // Y — вверх
    ImVec4(0.42f, 0.56f, 0.90f, 1.0f), // Z — как ZAxisColor сетки
};

bool PropertyVec3(const char* id, float v[3], float speed, const char* format) {
    const Style& ui = Get();
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;

    // Буква оси стоит СНАРУЖИ поля, а не внутри него.
    //
    // Внутри она была ради ширины: три поля с подписями занимают больше места,
    // чем три поля. Но плата оказалась выше выигрыша. Число в DragFloat
    // выравнено по центру, и при длинном значении («-1234.567») оно наезжало
    // на букву — читалось «X1234.567», а у отрицательных минус сливался с
    // буквой вовсе. Ещё хуже, что буква не подсвечивалась при наведении и
    // выглядела частью значения: люди пытались её выделить и стереть.
    //
    // Снаружи буква — это подпись поля, чем она и является: своим цветом, вне
    // рамки ввода, и её ширина считается по самому широкому символу, чтобы три
    // поля начинались на одной координате.
    const float letter = ImMax(ImMax(ImGui::CalcTextSize("X").x, ImGui::CalcTextSize("Y").x),
                               ImGui::CalcTextSize("Z").x);
    const float cell = letter + ui.SpacingXS; // буква + зазор до рамки
    const float gap = ui.SpacingSM;           // между группами «буква + поле»
    // Ширина поля считается один раз и округляется вниз, чтобы третья группа
    // не вылезала за край панели; остаток отдаётся последнему полю.
    const float w = ImFloor((avail - gap * 2.0f - cell * 3.0f) / 3.0f);

    bool changed = false;
    static const char* kAxis[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        // Буква по центру своей колонки: у «X», «Y» и «Z» разная ширина, и без
        // этого поля разъезжались бы на пиксель-другой.
        const float bias = (letter - ImGui::CalcTextSize(kAxis[i]).x) * 0.5f;
        if (bias > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bias);
        ImGui::TextColored(kAxisColor[i], "%s", kAxis[i]);
        ImGui::SameLine(0.0f, ui.SpacingXS - bias);
        const float fw = (i == 2) ? ImMax(w, avail - (w + cell + gap) * 2.0f - cell) : w;
        ImGui::SetNextItemWidth(fw);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ui.PaddingControlY, ui.PaddingControlY));
        changed |= ImGui::DragFloat("##v", &v[i], speed, 0.0f, 0.0f, format);
        ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// --- остальные поля строки свойств ------------------------------------------
//
// Все они устроены одинаково: своё имя прячется за "##" (подпись уже стоит в
// левой колонке), ширина растягивается на всю колонку значения. Отступ внутри
// поля берётся из токенов, а не из текущего стиля ImGui, чтобы высота строки
// совпадала с высотой полей PropertyVec3 в соседнем разделе.
namespace {
// Общая подготовка: id без подписи и ширина во всю колонку.
struct FieldScope {
    char name[64];
    explicit FieldScope(const char* id) {
        std::snprintf(name, sizeof(name), "##%s", id);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Get().PaddingControlY, Get().PaddingControlY));
    }
    ~FieldScope() { ImGui::PopStyleVar(); }
};
} // namespace

bool PropertyCombo(const char* id, int* current, const char* const items[], int count) {
    FieldScope f(id);
    return ImGui::Combo(f.name, current, items, count);
}

bool PropertyFloat(const char* id, float* v, float speed, float min, float max,
                   const char* format) {
    FieldScope f(id);
    return ImGui::DragFloat(f.name, v, speed, min, max, format);
}

bool PropertySlider(const char* id, float* v, float min, float max, const char* format) {
    FieldScope f(id);
    return ImGui::SliderFloat(f.name, v, min, max, format);
}

bool PropertyInt(const char* id, int* v, int step) {
    FieldScope f(id);
    return ImGui::InputInt(f.name, v, step);
}

bool PropertyCheckbox(const char* id, bool* v) {
    FieldScope f(id);
    // Галка не растягивается — она квадратная. Но выравнивание по левому краю
    // колонки значения обязано совпасть с левым краем соседних полей.
    return ImGui::Checkbox(f.name, v);
}

bool PropertyColor(const char* id, float col[3]) {
    FieldScope f(id);
    return ImGui::ColorEdit3(f.name, col, ImGuiColorEditFlags_NoInputs |
                                              ImGuiColorEditFlags_NoLabel);
}

bool PropertyText(const char* id, char* buf, size_t size, const char* hint) {
    FieldScope f(id);
    if (hint) return ImGui::InputTextWithHint(f.name, hint, buf, size);
    return ImGui::InputText(f.name, buf, size);
}

void PropertyValue(const char* fmt, ...) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, C(Role::TextDim));
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
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


// --- поиск (см. UI.h: одна свёртка на редактор) -----------------------------

std::string Fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out.push_back((char)std::tolower(c));
            ++i;
        } else if (c == 0xD0 && i + 1 < s.size()) {
            // А-П это D0 90..9F, Р-Я это D0 A0..AF, Ё это D0 81.
            const unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F) {
                out.push_back((char)0xD0);
                out.push_back((char)(d + 0x20));
            } else if (d >= 0xA0 && d <= 0xAF) {
                out.push_back((char)0xD1);
                out.push_back((char)(d - 0x20));
            } else if (d == 0x81) {
                out.push_back((char)0xD1);
                out.push_back((char)0x91);
            } else {
                out.push_back((char)c);
                out.push_back((char)d);
            }
            i += 2;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            out.push_back((char)c);
            out.push_back(s[i + 1]);
            i += 2;
        } else {
            out.push_back((char)c);
            ++i;
        }
    }
    return out;
}

bool Matches(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return Fold(haystack).find(Fold(needle)) != std::string::npos;
}

} // namespace Sage::UI
