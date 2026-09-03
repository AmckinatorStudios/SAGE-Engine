#include "sage/ui/UILegacy.h"

namespace sage::ui {

void Decompose(const LegacyElement& flat, entt::registry& reg, entt::entity e) {
    using Kind = LegacyElement::Kind;

    Transform t;
    t.Anchor = flat.Anchor;
    t.Offset = flat.Offset;
    t.Size = flat.Size;
    t.Layer = flat.Layer;
    t.Visible = flat.Visible;
    reg.emplace_or_replace<Transform>(e, t);

    // Подложка есть у всех, кроме чистой надписи, чистой картинки и элементов
    // ДИАПАЗОНА: у первых фон не рисовался и в старой системе, а у ползунка с
    // галкой цвет подложки уходил в дорожку и квадратик — теперь эти цвета
    // хранит сам диапазон (см. ниже), и вторая заливка во весь элемент
    // закрасила бы то, поверх чего он рисуется.
    if (flat.Type != Kind::Label && flat.Type != Kind::Image && flat.Type != Kind::Icon &&
        flat.Type != Kind::Slider && flat.Type != Kind::Checkbox) {
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
        // Цвета старого элемента ложатся ПРЯМО В ДИАПАЗОН: подложка была
        // дорожкой и квадратиком, а цвет заполнения полосы — ручкой и
        // галочкой. Раньше ради второго рядом заводилась пустая шкала, и
        // «перекрасить ползунок» означало «повесить на него полосу».
        range.TrackColor = flat.Color;
        range.AccentColor = flat.BarFillColor;
        range.BorderColor = flat.BorderColor;
        range.BorderThickness = flat.BorderThickness;
        range.Rounding = flat.Rounding;
        reg.emplace_or_replace<Range>(e, range);
    }
    if (flat.ClipChildren) reg.emplace_or_replace<Mask>(e, Mask{});
}

} // namespace sage::ui
