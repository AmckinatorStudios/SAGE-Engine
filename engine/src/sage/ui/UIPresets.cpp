#include "sage/ui/UIPresets.h"

#include "sage/ui/UI.h"

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

} // namespace sage::ui
