#include "sage/ui/UIBridge.h"

#include "sage/ui/UIComponents.h"

namespace sage::ui {

bool IsElement(const entt::registry& reg, entt::entity e) {
    return reg.valid(e) && reg.all_of<Transform>(e);
}

UIElementComponent Compose(const entt::registry& reg, entt::entity e) {
    UIElementComponent out;
    const Transform* t = reg.try_get<Transform>(e);
    if (!t) return out;

    out.Anchor = t->Anchor;
    out.Offset = t->Offset;
    out.Size = t->Size;
    out.Layer = t->Layer;
    out.Visible = t->Visible;
    out.LayoutSize = t->LayoutSize;

    const Fill* fill = reg.try_get<Fill>(e);
    const Label* label = reg.try_get<Label>(e);
    const Image* image = reg.try_get<Image>(e);
    const Bar* bar = reg.try_get<Bar>(e);
    const Icon* icon = reg.try_get<Icon>(e);
    const Interactable* act = reg.try_get<Interactable>(e);
    const TextInput* input = reg.try_get<TextInput>(e);
    const Range* range = reg.try_get<Range>(e);

    // Вид элемента ВЫВОДИТСЯ из набора компонентов, а не хранится полем.
    // Именно это поле и было корнем прежней архитектуры: «вид» решал, какие из
    // сорока полей что-то значат.
    if (image) out.Type = UIElementComponent::Kind::Image;
    else if (bar) out.Type = UIElementComponent::Kind::Bar;
    else if (input) out.Type = UIElementComponent::Kind::Input;
    else if (range) {
        out.Type = range->Toggle ? UIElementComponent::Kind::Checkbox
                                 : UIElementComponent::Kind::Slider;
    } else if (icon && !fill && !label) out.Type = UIElementComponent::Kind::Icon;
    else if (label && !fill) out.Type = UIElementComponent::Kind::Label;
    else out.Type = UIElementComponent::Kind::Panel;

    if (fill) {
        out.Color = fill->Color;
        out.Rounding = fill->Rounding;
        out.BorderThickness = fill->BorderThickness;
        out.BorderColor = fill->BorderColor;
        out.GradientColor = fill->Gradient;
        out.ShadowSize = fill->ShadowSize;
    } else if (!image) {
        // Нет подложки — нет и заливки: у надписи фон не рисуется.
        out.Color = glm::vec4(0.0f);
        out.Rounding = 0.0f;
        out.BorderThickness = 0.0f;
        out.GradientColor = glm::vec4(0.0f);
        out.ShadowSize = 0.0f;
    }
    if (label) {
        out.Text = label->Text;
        out.TextScale = label->Scale;
        out.TextColor = label->Color;
        out.TextCentered = label->Horizontal == Label::Align::Center;
        out.WrapText = label->Wrap;
        out.AutoWidth = label->AutoWidth;
        out.PadX = label->PadX;
    } else {
        out.TextColor.a = 0.0f; // отрисовка пропускает прозрачный текст
    }
    if (image) {
        out.TexturePath = image->Path;
        out.Color = image->Tint;
        out.Sprite = image->Sprite;
        out.SliceBorder = image->SliceBorder;
        out.PixelScale = image->PixelScale;
        out.PixelArt = image->PixelArt;
        out.SpriteHover = image->SpriteHover;
        out.SpritePressed = image->SpritePressed;
        out.Tex = image->Tex;
    }
    if (bar) {
        // Показывается сглаженное значение, если сглаживание включено: полоса
        // здоровья, прыгающая рывком, читается хуже едущей за четверть секунды.
        out.Value = (bar->Smoothing > 0.0f && bar->Displayed >= 0.0f) ? bar->Displayed
                                                                      : bar->Value;
        out.BarFillColor = bar->FillColor;
    }
    if (icon) {
        out.Icon = icon->Name;
        out.IconColor = icon->Color;
    }
    if (act) {
        out.Interactive = true;
        out.Enabled = act->Enabled;
        out.Hovered = act->Runtime.Hovered;
        out.Pressed = act->Runtime.Pressed;
        out.Focused = act->Runtime.Focused;
        out.Clicked = act->Runtime.Clicked;
        out.Changed = act->Runtime.Changed;
        out.Caret = act->Runtime.Caret;
        out.CaretBlink = act->Runtime.CaretBlink;
    }
    if (input) {
        out.Placeholder = input->Placeholder;
        out.MaxLength = input->MaxLength;
        out.Password = input->Password;
    }
    if (range) {
        out.MinValue = range->Min;
        out.MaxValue = range->Max;
        out.Value = (range->Max > range->Min) ? (range->Value - range->Min) /
                                                    (range->Max - range->Min)
                                              : range->Value;
    }
    out.ClipChildren = reg.all_of<Mask>(e);
    return out;
}

void Decompose(const UIElementComponent& flat, entt::registry& reg, entt::entity e) {
    using Kind = UIElementComponent::Kind;

    Transform t;
    t.Anchor = flat.Anchor;
    t.Offset = flat.Offset;
    t.Size = flat.Size;
    t.Layer = flat.Layer;
    t.Visible = flat.Visible;
    reg.emplace_or_replace<Transform>(e, t);

    // Подложка есть у всех, кроме чистой надписи и чистой картинки: у них фон
    // не рисовался и в старой системе.
    if (flat.Type != Kind::Label && flat.Type != Kind::Image && flat.Type != Kind::Icon) {
        Fill fill;
        fill.Color = flat.Color;
        fill.Rounding = flat.Rounding;
        fill.BorderThickness = flat.BorderThickness;
        fill.BorderColor = flat.BorderColor;
        fill.Gradient = flat.GradientColor;
        fill.ShadowSize = flat.ShadowSize;
        reg.emplace_or_replace<Fill>(e, fill);
    }
    if (!flat.Text.empty() || flat.Type == Kind::Label || flat.Type == Kind::Input) {
        Label label;
        label.Text = flat.Text;
        label.Scale = flat.TextScale;
        label.Color = flat.TextColor;
        label.Horizontal = flat.TextCentered ? Label::Align::Center : Label::Align::Start;
        label.Wrap = flat.WrapText;
        label.AutoWidth = flat.AutoWidth;
        label.PadX = flat.PadX;
        reg.emplace_or_replace<Label>(e, label);
    }
    if (flat.Type == Kind::Image) {
        Image image;
        image.Path = flat.TexturePath;
        image.Tint = flat.Color;
        image.Sprite = flat.Sprite;
        image.SliceBorder = flat.SliceBorder;
        image.PixelScale = flat.PixelScale;
        image.PixelArt = flat.PixelArt;
        image.SpriteHover = flat.SpriteHover;
        image.SpritePressed = flat.SpritePressed;
        image.Tex = flat.Tex;
        reg.emplace_or_replace<Image>(e, image);
    }
    if (flat.Type == Kind::Bar) {
        Bar bar;
        bar.Value = flat.Value;
        bar.FillColor = flat.BarFillColor;
        reg.emplace_or_replace<Bar>(e, bar);
    }
    if (!flat.Icon.empty()) {
        Icon icon;
        icon.Name = flat.Icon;
        icon.Color = flat.IconColor;
        reg.emplace_or_replace<Icon>(e, icon);
    }
    if (flat.Interactive) {
        Interactable act;
        act.Enabled = flat.Enabled;
        reg.emplace_or_replace<Interactable>(e, act);
    }
    if (flat.Type == Kind::Input) {
        TextInput input;
        input.Placeholder = flat.Placeholder;
        input.MaxLength = flat.MaxLength;
        input.Password = flat.Password;
        reg.emplace_or_replace<TextInput>(e, input);
    }
    if (flat.Type == Kind::Slider || flat.Type == Kind::Checkbox) {
        Range range;
        range.Min = flat.MinValue;
        range.Max = flat.MaxValue;
        // Value старого компонента хранился в 0..1 — разворачиваем в диапазон.
        range.Value = flat.MinValue + flat.Value * (flat.MaxValue - flat.MinValue);
        range.Toggle = flat.Type == Kind::Checkbox;
        if (range.Toggle) range.Step = 1.0f;
        reg.emplace_or_replace<Range>(e, range);
    }
    if (flat.ClipChildren) reg.emplace_or_replace<Mask>(e, Mask{});
}

} // namespace sage::ui
