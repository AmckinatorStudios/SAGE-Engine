#include "sage/ui/UIPresets.h"

#include "sage/ui/UI.h"
#include "sage/ui/UIComponents.h"

namespace sage::ui {

// Заготовка -> набор компонентов на сущности. Здесь и видно, чем новая модель
// отличается от старой: «кнопка» — это не значение перечисления, а Transform +
// Fill + Label + Interactable, и собирается она перечислением того, из чего
// состоит, без единой ветки «если вид такой-то».
bool ApplyPreset(entt::registry& reg, entt::entity e, const std::string& preset) {
    const Preset* p = FindPreset(preset);
    if (!p) return false;

    // Часть либо есть в заготовке — тогда ставим её значения, либо её нет —
    // тогда снимаем. Иначе «сделать из кнопки надпись» оставляло бы надпись
    // нажимаемой, и понять почему было бы неоткуда.
    auto part = [&reg, e](bool present, auto value) {
        using T = decltype(value);
        if (present) reg.emplace_or_replace<T>(e, value);
        else reg.remove<T>(e);
    };

    reg.emplace_or_replace<Transform>(e, p->Xf);
    part(p->HasFill, p->FillStyle);
    part(p->HasLabel, p->LabelStyle);
    part(p->HasImage, p->ImageStyle);
    part(p->HasBar, p->BarStyle);
    part(p->HasInteractable, p->Interact);
    part(p->HasInput, p->Input);
    part(p->HasRange, p->RangeValue);
    part(p->HasLayout, p->LayoutRule);
    part(p->HasMask, p->MaskRule);
    return true;
}

// ---------------------------------------------------------------------------
// Переходник со СТАРОГО компонента на новую систему.
//
// Заготовки теперь живут в одном месте — sage/ui/UI.cpp — и описаны НАБОРОМ
// компонентов (Transform + Fill + Label + Interactable для кнопки). Пока в
// движке ещё жив прежний UIElementComponent на сорок полей, ему нужен тот же
// «Button», что и новой системе: две таблицы заготовок разошлись бы на первой
// же правке, и кнопка из редактора перестала бы совпадать с кнопкой из
// скрипта. Поэтому таблица одна, а здесь она укладывается в старый компонент.
//
// Файл исчезнет вместе со старым компонентом.
// ---------------------------------------------------------------------------
bool ApplyPreset(UIElementComponent& element, const std::string& preset) {
    const Preset* p = FindPreset(preset);
    if (!p) return false;

    element.Anchor = p->Xf.Anchor;
    element.Offset = p->Xf.Offset;
    element.Size = p->Xf.Size;
    element.Layer = p->Xf.Layer;
    element.Visible = p->Xf.Visible;

    // Вид элемента у старого компонента один на всё, поэтому он выводится из
    // набора: есть картинка — Image, есть шкала — Bar, есть поле ввода — Input.
    if (p->HasImage) element.Type = UIElementComponent::Kind::Image;
    else if (p->HasBar) element.Type = UIElementComponent::Kind::Bar;
    else if (p->HasInput) element.Type = UIElementComponent::Kind::Input;
    else if (p->HasRange) {
        element.Type = p->RangeValue.Toggle ? UIElementComponent::Kind::Checkbox
                                            : UIElementComponent::Kind::Slider;
    } else if (p->HasLabel && !p->HasFill) element.Type = UIElementComponent::Kind::Label;
    else element.Type = UIElementComponent::Kind::Panel;

    if (p->HasFill) {
        element.Color = p->FillStyle.Color;
        element.Rounding = p->FillStyle.Rounding;
        element.BorderThickness = p->FillStyle.BorderThickness;
        element.BorderColor = p->FillStyle.BorderColor;
        element.GradientColor = p->FillStyle.Gradient;
        element.ShadowSize = p->FillStyle.ShadowSize;
    }
    if (p->HasLabel) {
        element.Text = p->LabelStyle.Text;
        element.TextScale = p->LabelStyle.Scale;
        element.TextColor = p->LabelStyle.Color;
        element.TextCentered = p->LabelStyle.Horizontal == Label::Align::Center;
        element.WrapText = p->LabelStyle.Wrap;
        element.AutoWidth = p->LabelStyle.AutoWidth;
        element.PadX = p->LabelStyle.PadX;
    }
    if (p->HasImage) {
        element.TexturePath = p->ImageStyle.Path;
        element.Color = p->ImageStyle.Tint;
        element.Sprite = p->ImageStyle.Sprite;
        element.SliceBorder = p->ImageStyle.SliceBorder;
        element.PixelScale = p->ImageStyle.PixelScale;
        element.PixelArt = p->ImageStyle.PixelArt;
    }
    if (p->HasBar) {
        element.Value = p->BarStyle.Value;
        element.BarFillColor = p->BarStyle.FillColor;
    }
    element.Interactive = p->HasInteractable;
    if (p->HasInteractable) element.Enabled = p->Interact.Enabled;
    if (p->HasInput) {
        element.Placeholder = p->Input.Placeholder;
        element.MaxLength = p->Input.MaxLength;
        element.Password = p->Input.Password;
    }
    if (p->HasRange) {
        element.MinValue = p->RangeValue.Min;
        element.MaxValue = p->RangeValue.Max;
        element.Value = p->RangeValue.Value;
    }
    // Маска и раскладка старому компоненту недоступны целиком: у него есть
    // только флаг «обрезать детей», а расставлять их он не умеет вовсе.
    element.ClipChildren = p->HasMask;
    return true;
}

} // namespace sage::ui
