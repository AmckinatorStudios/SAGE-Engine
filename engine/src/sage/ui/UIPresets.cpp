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
    part(p.HasStack, p.StackRule);
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
    const bool stack = reg.all_of<Stack>(e);
    const bool scroll = reg.all_of<Scroll>(e);
    const bool mask = reg.all_of<Mask>(e);
    const bool visual = fill || label || image || icon || reg.all_of<Bar>(e) ||
                        reg.all_of<Range>(e) || reg.all_of<TextInput>(e);

    // Порядок проверок — от САМОГО ОПРЕДЕЛЁННОГО набора к менее: у поля ввода
    // есть и подложка, и надпись, и реакция, и оно же единственное с
    // TextInput. Начни с подложки — и каждое поле ввода стало бы панелью.
    if (reg.all_of<TextInput>(e)) return "Input Field";
    if (const Range* r = reg.try_get<Range>(e)) return r->Toggle ? "Checkbox" : "Slider";
    if (reg.all_of<Bar>(e)) return "Progress Bar";

    // Контейнер — только если в нём НЕТ ничего видимого. Старая «панель со
    // списком» (подложка + раскладка) — это уже два объекта, и разделить её
    // на панель и контейнер внутри — дело переноса сцены (SplitVisualContainers).
    if (!visual) {
        if (scroll) return "Scroll View";
        if (stack) {
            const Stack& st = reg.get<Stack>(e);
            if (st.Direction == Stack::Flow::Grid) return "Grid";
            if (st.Direction == Stack::Flow::Horizontal) return st.Wrap ? "Wrap Row" : "Row";
            return "Column";
        }
        if (mask) return "Clip Area";
        if (act) return {};
        return "Group";
    }
    if (image && !fill && !act) return "Image";
    if (icon && !fill && !label && !act) return "Icon";
    if (label && !fill && !act) return "Text";
    if (fill && act) return "Button";
    if (fill && !label && !image && !icon) return "Panel";
    return {};
}

int NormalizeElements(Scene& scene) {
    entt::registry& reg = scene.Registry();
    int split = 0;

    // Сначала — кого делить. Список собирается заранее: деление создаёт новые
    // объекты, а менять набор сущностей посреди обхода нельзя.
    std::vector<entt::entity> mixed;
    for (entt::entity e : reg.view<Element>()) {
        const bool containerPart = reg.any_of<Stack, Scroll, Mask>(e);
        const bool visual = reg.any_of<Fill, Label, Image, Icon, Bar, Range, TextInput>(e);
        if (containerPart && visual) mixed.push_back(e);
    }

    for (entt::entity e : mixed) {
        // Контейнер ВНУТРИ элемента, во весь его прямоугольник: дети стоят
        // там же, где стояли, а подложка остаётся у элемента.
        GameObject box = scene.CreateEmptyObject("Layout");
        Element cb;
        cb.Anchor = UIAnchor::TopLeft;
        cb.Mode = Element::Stretch::Both;
        cb.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        reg.emplace<Element>(box.Entity(), cb);
        if (const Stack* st = reg.try_get<Stack>(e)) {
            reg.emplace<Stack>(box.Entity(), *st);
            reg.remove<Stack>(e);
        }
        if (const Scroll* sc = reg.try_get<Scroll>(e)) {
            reg.emplace<Scroll>(box.Entity(), *sc);
            reg.remove<Scroll>(e);
        }
        if (const Mask* m = reg.try_get<Mask>(e)) {
            reg.emplace<Mask>(box.Entity(), *m);
            reg.remove<Mask>(e);
        }
        // Дети переезжают в контейнер В ТОМ ЖЕ ПОРЯДКЕ: от порядка зависит,
        // кто в списке первым.
        std::vector<entt::entity> kids;
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) kids = h->Children;
        for (entt::entity k : kids) scene.SetParent(k, box.Entity());
        scene.SetParent(box.Entity(), e);
        reg.get<Element>(box.Entity()).Type = InferType(reg, box.Entity());
        if (NameComponent* nc = reg.try_get<NameComponent>(box.Entity()))
            nc->Name = reg.get<Element>(box.Entity()).Type;
        reg.get<Element>(e).Type.clear();   // имя типа ниже выведется заново
        ++split;
    }

    // Имена типов — нынешние: «Toolbar» из старой сцены — это «Row», а
    // «Screen» — панель. Без типа (собран кодом) — выводится по частям.
    for (entt::entity e : reg.view<Element>()) {
        Element& el = reg.get<Element>(e);
        if (el.Type.empty()) {
            el.Type = InferType(reg, e);
            continue;
        }
        const Preset* p = FindPreset(el.Type);
        if (!p || p->Name == el.Type) continue;
        // Переименованный тип обязан совпасть с набором частей: «Toolbar»,
        // из которого раскладка уехала в контейнер, — уже панель.
        const std::string inferred = InferType(reg, e);
        el.Type = inferred.empty() ? p->Name : inferred;
    }
    return split;
}

} // namespace sage::ui
