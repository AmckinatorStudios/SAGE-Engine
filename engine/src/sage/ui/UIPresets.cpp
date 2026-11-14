#include "sage/ui/UIPresets.h"

#include "sage/ui/UI.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UIPart.h"

namespace sage::ui {

// Заготовка -> набор компонентов на сущности. Здесь и видно, чем новая модель
// отличается от старой: «кнопка» — это не значение перечисления, а Element +
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
    // ТИП ЗАПИСЫВАЕТСЯ В САМ ЭЛЕМЕНТ. Заготовка перестала быть «разовым
    // набором значений»: она объявляет ТИП, и элемент обязан помнить, чем он
    // является, — иначе и инспектор, и список, и скрипт снова вынуждены
    // угадывать это по набору частей.
    Element box = p.Box;
    box.Type = p.Name;
    reg.emplace_or_replace<Element>(e, box);
    part(p.HasFill, p.FillStyle);
    part(p.HasLabel, p.LabelStyle);
    part(p.HasImage, p.ImageStyle);
    part(p.HasBar, p.BarStyle);
    part(p.HasInteractable, p.Interact);
    part(p.HasInput, p.Input);
    part(p.HasRange, p.RangeValue);
    part(p.HasArrangement, p.Arrange);
    part(p.HasMask, p.MaskRule);
    part(p.HasScroll, p.ScrollRule);
    part(p.HasIcon, p.IconStyle);
}

void BuildChildren(Scene& scene, entt::entity parent, const Preset& p) {
    for (const Preset& child : p.Children) {
        GameObject obj = scene.CreateEmptyObject(child.Name.empty() ? "Element" : child.Name);
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


bool ApplyPreset(Scene& scene, entt::entity e, const std::string& preset, bool replaceChildren) {
    const Preset* p = FindPreset(preset);
    if (!p) return false;
    if (!replaceChildren) {
        entt::registry& reg = scene.Registry();
        bool hasElementChildren = false;
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e))
            for (entt::entity c : h->Children)
                if (reg.all_of<Element>(c)) hasElementChildren = true;
        ApplyNode(reg, e, *p);
        if (!hasElementChildren) BuildChildren(scene, e, *p);
        return true;
    }

    // Прежние дети-ЭЛЕМЕНТЫ убираются: применить заготовку — значит получить
    // ровно её. Не-элементы (звук, скрипт на объекте) не трогаем: они не часть
    // внешнего вида и убирать их заготовка не просила.
    entt::registry& reg = scene.Registry();
    std::vector<int> stale;
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
        for (entt::entity c : h->Children) {
            const IdComponent* id = reg.try_get<IdComponent>(c);
            if (id && reg.all_of<Element>(c)) stale.push_back(id->Id);
        }
    }
    for (int id : stale) scene.RemoveObject(id);

    ApplyNode(reg, e, *p);
    BuildChildren(scene, e, *p);
    return true;
}

std::string InferType(const entt::registry& reg, entt::entity e) {
    if (!reg.all_of<Element>(e)) return {};
    const bool fill = reg.all_of<Fill>(e);
    const bool label = reg.all_of<Label>(e);
    const bool image = reg.all_of<Image>(e);
    const bool icon = reg.all_of<Icon>(e);
    const bool act = reg.all_of<Interactable>(e);

    // Порядок проверок — от САМОГО ОПРЕДЕЛЁННОГО набора к менее: у поля ввода
    // есть и подложка, и надпись, и реакция, и оно же единственное с
    // TextInput. Начни с подложки — и каждое поле ввода стало бы панелью.
    if (reg.all_of<TextInput>(e)) return "Input Field";
    if (const Range* r = reg.try_get<Range>(e)) return r->Toggle ? "Checkbox" : "Slider";
    if (reg.all_of<Bar>(e)) return "Progress Bar";
    if (fill && act) return "Button";
    if (image && !fill && !act) return "Image";
    if (icon && !fill && !label && !act) return "Icon";
    if (label && !fill && !act) return "Text";
    // Подложка — панель, ничего видимого — контейнер: оба держат детей.
    if (fill && !label && !image && !icon) return "Panel";
    if (!fill && !label && !image && !icon && !act) return "Container";
    return {};
}

int InferMissingTypes(Scene& scene) {
    entt::registry& reg = scene.Registry();
    int named = 0;
    for (entt::entity e : reg.view<Element>()) {
        Element& el = reg.get<Element>(e);
        if (!el.Type.empty()) continue;
        el.Type = InferType(reg, e);
        ++named;
    }
    return named;
}

} // namespace sage::ui
