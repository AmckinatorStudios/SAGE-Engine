#include "sage/ui/visual/UIText.h"

#include "sage/ui/core/UINode.h"
#include "sage/ui/visual/UITextLayout.h"

namespace sage::ui {

const char* const* UITextAlignNames() {
    static const char* n[] = {"Left", "Center", "Right", "Justify"};
    return n;
}
int UITextAlignCount() { return 4; }

const char* const* UITextVAlignNames() {
    static const char* n[] = {"Top", "Center", "Bottom", "Baseline"};
    return n;
}
int UITextVAlignCount() { return 4; }

const char* const* UITextWrapNames() {
    static const char* n[] = {"None", "Word", "Character"};
    return n;
}
int UITextWrapCount() { return 3; }

const char* const* UITextOverflowNames() {
    static const char* n[] = {"Clip", "Ellipsis", "Visible"};
    return n;
}
int UITextOverflowCount() { return 3; }

const UIComponentType& UIText::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "text";
        d.Title = SAGE_UI_TEXT("Text");
        d.Hint = SAGE_UI_TEXT("Typography with its own layout pass");
        d.Icon = "text";
        d.Category = UIComponentCategory::Text;
        d.Order = 60;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIText()); };
        d.Props = {
            {"Text", SAGE_UI_TEXT("Text"), UIProperty::Kind::String, SAGE_UI_OFFSET(UIText, Text),
             0, 0, nullptr, nullptr, 0, UIProperty::Widget::Multiline, nullptr},
            {"Key", SAGE_UI_TEXT("Localization key"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIText, Key), 0, 0,
             SAGE_UI_TEXT("Resolved through the game localization; text stays as a fallback"),
             nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Font", SAGE_UI_TEXT("Font"), UIProperty::Kind::String, SAGE_UI_OFFSET(UIText, Font),
             0, 0, nullptr, nullptr, 0, UIProperty::Widget::Font, SAGE_UI_TEXT("Font")},
            {"Weight", SAGE_UI_TEXT("Weight"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UIText, Weight), 100.0f, 900.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Font")},
            {"Italic", SAGE_UI_TEXT("Italic"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIText, Italic), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Font")},
            {"Size", SAGE_UI_TEXT("Size"), UIProperty::Kind::Float, SAGE_UI_OFFSET(UIText, Size),
             4.0f, 200.0f, nullptr, nullptr, 0, UIProperty::Widget::Auto, SAGE_UI_TEXT("Font")},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIText, Color), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Gradient", SAGE_UI_TEXT("Gradient"), UIProperty::Kind::Gradient,
             SAGE_UI_OFFSET(UIText, Gradient), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Align", SAGE_UI_TEXT("Horizontal align"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIText, Align), 0, 0, nullptr, UITextAlignNames(), 4,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Layout")},
            {"VAlign", SAGE_UI_TEXT("Vertical align"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIText, VAlign), 0, 0, nullptr, UITextVAlignNames(), 4,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Layout")},
            {"Wrap", SAGE_UI_TEXT("Wrap"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIText, Wrap), 0,
             0, nullptr, UITextWrapNames(), 3, UIProperty::Widget::Auto, SAGE_UI_TEXT("Layout")},
            {"Overflow", SAGE_UI_TEXT("Overflow"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIText, Overflow), 0, 0, nullptr, UITextOverflowNames(), 3,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Layout")},
            {"MaxLines", SAGE_UI_TEXT("Max lines"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UIText, MaxLines), 0.0f, 64.0f,
             SAGE_UI_TEXT("Zero means no limit"), nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Layout")},
            {"LetterSpacing", SAGE_UI_TEXT("Letter spacing"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, LetterSpacing), -8.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Layout")},
            {"LineSpacing", SAGE_UI_TEXT("Line spacing"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, LineSpacing), 0.5f, 3.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Layout")},
            {"ParagraphSpacing", SAGE_UI_TEXT("Paragraph spacing"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, ParagraphSpacing), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Layout")},
            {"AutoSize", SAGE_UI_TEXT("Auto size"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIText, AutoSize), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Auto size")},
            {"MinSize", SAGE_UI_TEXT("Min size"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, MinSize), 4.0f, 100.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Auto size")},
            {"MaxSize", SAGE_UI_TEXT("Max size"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, MaxSize), 4.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Auto size")},
            {"Padding", SAGE_UI_TEXT("Padding"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIText, Padding), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Layout")},
            {"OutlineWidth", SAGE_UI_TEXT("Outline width"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, OutlineWidth), 0.0f, 8.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Outline")},
            {"OutlineColor", SAGE_UI_TEXT("Outline color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIText, OutlineColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Outline")},
            {"ShadowOffset", SAGE_UI_TEXT("Shadow offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIText, ShadowOffset), -32.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Shadow")},
            {"ShadowColor", SAGE_UI_TEXT("Shadow color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIText, ShadowColor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Shadow")},
            {"ShadowSoftness", SAGE_UI_TEXT("Shadow softness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIText, ShadowSoftness), 0.0f, 16.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Shadow")},
        };
        return d;
    }();
    return t;
}

std::string UIText::Resolve(const UIContext& ctx) const {
    // Ключ важнее строки, но строка остаётся запасным вариантом: без словаря
    // (в редакторе, в тесте, на раннем этапе) интерфейс обязан оставаться
    // читаемым, а не показывать «menu.play» (§108, §134).
    if (!Key.empty()) {
        std::string translated = ctx.Text(Key);
        if (!translated.empty() && translated != Key) return translated;
        if (!Text.empty()) return Text;
        return Key;
    }
    return Text;
}

glm::vec2 UIText::Measure(const UIContext& ctx, const UINode& node, glm::vec2 available) const {
    (void)node;
    // Ширина ограничивается ТОЛЬКО если текст переносится: иначе надпись в
    // контейнере по содержимому обрезала бы сама себя.
    const float w = Wrap == UITextWrap::None ? 0.0f : available.x;
    const UITextLayoutResult r = UILayoutText(ctx, *this, w, 0.0f);
    return r.Size;
}

} // namespace sage::ui
