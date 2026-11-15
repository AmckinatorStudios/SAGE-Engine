#include "sage/ui/UIPartsDetail.h"

#include <cctype>
#include <sstream>

// Выпадающий список и проверка ввода (см. UIPartsDetail.h; ввод и раскрытый
// список — UIControls.cpp).
namespace sage::ui {

bool InputAccepts(TextInput::Rule rule, const std::string& text, const std::string& c) {
    if (rule == TextInput::Rule::None || c.empty()) return true;
    // Не-ASCII (кириллица, значки) — только «без проверки»: буквы и цифры
    // правил ниже — латинские, как у чисел и кодов, которых ими ждут.
    if ((unsigned char)c[0] >= 0x80) return false;
    const char ch = c[0];
    const bool digit = std::isdigit((unsigned char)ch) != 0;
    switch (rule) {
    case TextInput::Rule::Integer: return digit || (ch == '-' && text.empty());
    case TextInput::Rule::Decimal:
        if (digit) return true;
        if (ch == '-') return text.empty();
        if (ch == '.' || ch == ',') return text.find_first_of(".,") == std::string::npos;
        return false;
    case TextInput::Rule::Alphanumeric: return std::isalnum((unsigned char)ch) != 0;
    case TextInput::Rule::None: break;
    }
    return true;
}

std::vector<std::string> DropdownOptions(const Dropdown& d) {
    std::vector<std::string> out;
    std::istringstream in(d.Options);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool DropdownDisabled(const Dropdown& d, int index) {
    const std::vector<std::string> options = DropdownOptions(d);
    if (index < 0 || index >= (int)options.size()) return true;
    std::istringstream in(d.Disabled);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == options[(size_t)index]) return true;
    }
    return false;
}

namespace parts {

const std::vector<PartField>& DropdownFields() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"options", SAGE_UI_TEXT("Options"), PartField::Kind::String, offsetof(Dropdown, Options), 0.0f,
             0.0f, "One option per line.", nullptr, 0, PartField::Widget::Multiline},
            {"selected", SAGE_UI_TEXT("Selected"), PartField::Kind::Int, offsetof(Dropdown, Selected), -1.0f,
             1000.0f, "The number of the chosen option, from 0. -1 — nothing, the placeholder is shown."},
            {"placeholder", SAGE_UI_TEXT("Placeholder"), PartField::Kind::String,
             offsetof(Dropdown, Placeholder)},
            {"disabled", SAGE_UI_TEXT("Disabled options"), PartField::Kind::String,
             offsetof(Dropdown, Disabled), 0.0f, 0.0f,
             "Options shown but not selectable — their text, one per line.", nullptr, 0,
             PartField::Widget::Multiline},
            {"maxVisible", SAGE_UI_TEXT("Visible rows"), PartField::Kind::Int, offsetof(Dropdown, MaxVisible),
             1.0f, 32.0f, "A longer list scrolls with the wheel."},
            {"listColor", SAGE_UI_TEXT("List colour"), PartField::Kind::Color, offsetof(Dropdown, ListColor)},
            {"highlightColor", SAGE_UI_TEXT("Highlight colour"), PartField::Kind::Color,
             offsetof(Dropdown, HighlightColor)},
            {"textColor", SAGE_UI_TEXT("Text colour"), PartField::Kind::Color, offsetof(Dropdown, TextColor)},
        };
        for (int i = 0; i < 4; ++i) v[(size_t)i].Content = true;
        MarkAdvanced(v, {"maxVisible", "listColor", "highlightColor", "textColor"});
        MarkSection(v, kSectionLook, {"listColor", "highlightColor", "textColor"});
        return v;
    }();
    return f;
}

// Закрытый список: выбранный вариант (или бледная подсказка) и стрелка
// справа. Раскрытый рисуется отдельно, поверх всего (DrawDropdownOverlays).
void DrawDropdownPart(const PartDrawContext& c) {
    const Dropdown& d = *static_cast<const Dropdown*>(c.Data);
    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const std::vector<std::string> options = DropdownOptions(d);
    const bool chosen = d.Selected >= 0 && d.Selected < (int)options.size();
    const std::string text = chosen ? options[(size_t)d.Selected] : d.Placeholder;
    const Label* label = c.Sibling<Label>();
    const UITextStyle style = label ? StyleOf(*label, ui) : UITextStyle{};
    // Своей надписи у списка нет — кегль как у надписи по умолчанию, иначе
    // текст выходит «родными» семипиксельными буквами.
    const float scale = (label ? label->Scale : Label{}.Scale) * c.Scale;
    const float pad = (label ? label->PadX : 10.0f) * c.Scale;
    const float arrow = std::min(r.h * 0.28f, 10.0f * c.Scale);
    const glm::vec3 rgb = StateTint(c, glm::vec3(d.TextColor));
    const float y = r.y + (r.h - ui.LineHeight(scale, style)) * 0.5f;
    ui.PushClipRect(r.x, r.y, std::max(0.0f, r.w - pad - arrow * 2.0f), r.h);
    ui.Text(r.x + pad, y, scale, rgb, text, AlphaOf(c, d.TextColor.a * (chosen ? 1.0f : 0.5f)), style);
    ui.PopClipRect();
    // Стрелка: вниз у закрытого, вверх у раскрытого.
    const float cx = r.x + r.w - pad - arrow, cy = r.y + r.h * 0.5f;
    const float k = d.Open ? -1.0f : 1.0f;
    ui.Triangle({cx - arrow, cy - arrow * 0.5f * k}, {cx + arrow, cy - arrow * 0.5f * k},
                {cx, cy + arrow * 0.5f * k}, rgb, AlphaOf(c, d.TextColor.a));
}

} // namespace parts
} // namespace sage::ui
