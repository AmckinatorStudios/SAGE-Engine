#include "sage/ui/UIPresets.h"

#include "sage/ui/UI.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UIPart.h"

namespace sage::ui {

// Заготовка -> набор компонентов на сущности. Здесь и видно, чем новая модель
// отличается от старой: «кнопка» — это не значение перечисления, а Transform +
// Fill + Label + Interactable, и собирается она перечислением того, из чего
// состоит, без единой ветки «если вид такой-то».
namespace {

// Ставит на сущность части ОДНОГО узла заготовки (без детей).
void ApplyNode(entt::registry& reg, entt::entity e, const Preset& p) {
    auto part = [&reg, e](bool present, auto value) {
        using T = decltype(value);
        if (present) reg.emplace_or_replace<T>(e, value);
        else reg.remove<T>(e);
    };
    reg.emplace_or_replace<Transform>(e, p.Xf);
    part(p.HasFill, p.FillStyle);
    part(p.HasLabel, p.LabelStyle);
    part(p.HasImage, p.ImageStyle);
    part(p.HasBar, p.BarStyle);
    part(p.HasInteractable, p.Interact);
    part(p.HasInput, p.Input);
    part(p.HasRange, p.RangeValue);
    part(p.HasLayout, p.LayoutRule);
    part(p.HasMask, p.MaskRule);
}

void BuildChildren(Scene& scene, entt::entity parent, const Preset& p) {
    for (const Preset& child : p.Children) {
        GameObject obj = scene.CreateObject(child.Name.empty() ? "Element" : child.Name);
        scene.SetParent(obj.Entity(), parent);
        ApplyNode(scene.Registry(), obj.Entity(), child);
        BuildChildren(scene, obj.Entity(), child); // заготовка может быть глубже
    }
}

} // namespace

// Без сцены — только части самой сущности. Дети (надпись на кнопке) требуют
// создания ОБЪЕКТОВ, а это умеет только сцена: см. перегрузку ниже.
bool ApplyPreset(entt::registry& reg, entt::entity e, const std::string& preset) {
    const Preset* p = FindPreset(preset);
    if (!p) return false;
    ApplyNode(reg, e, *p);
    return true;
}


bool ApplyPreset(Scene& scene, entt::entity e, const std::string& preset) {
    const Preset* p = FindPreset(preset);
    if (!p) return false;

    // Прежние дети-ЭЛЕМЕНТЫ убираются: применить заготовку — значит получить
    // ровно её. Не-элементы (звук, скрипт на объекте) не трогаем: они не часть
    // внешнего вида и убирать их заготовка не просила.
    entt::registry& reg = scene.Registry();
    std::vector<int> stale;
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
        for (entt::entity c : h->Children) {
            const IdComponent* id = reg.try_get<IdComponent>(c);
            if (id && reg.all_of<Transform>(c)) stale.push_back(id->Id);
        }
    }
    for (int id : stale) scene.RemoveObject(id);

    ApplyNode(reg, e, *p);
    BuildChildren(scene, e, *p);
    return true;
}

} // namespace sage::ui
