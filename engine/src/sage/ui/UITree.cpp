// ---------------------------------------------------------------------------
// ПРАВИЛА ДЕРЕВА ИНТЕРФЕЙСА: главный контейнер, кто держит детей, перенос.
//
// Отдельно от обхода сцены (UISceneSystem.cpp): тот рисует и раскладывает, а
// здесь — что куда можно положить. Правила одни на редактор, скрипт и тесты.
// ---------------------------------------------------------------------------
#include "sage/ui/UISceneSystem.h"

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"

namespace sage::ui {

entt::entity MainContainer(Scene& scene, entt::entity interfaceEntity) {
    entt::registry& reg = scene.Registry();
    if (interfaceEntity == entt::null || !reg.valid(interfaceEntity) ||
        !reg.all_of<InterfaceComponent>(interfaceEntity))
        return entt::null;
    entt::entity found = entt::null;
    for (entt::entity c : scene.ChildrenOf(interfaceEntity)) {
        if (!IsElement(reg, c)) continue;
        if (found != entt::null) return entt::null;   // корней несколько — старый вид
        found = c;
    }
    // Единственный корень — главный, только если он и есть область: контейнер
    // во весь родитель. Панель 400x300 единственным корнем старого интерфейса
    // главным не становится — при выносе в файл она ляжет В него.
    if (found == entt::null) return entt::null;
    const Element& el = reg.get<Element>(found);
    const Preset* type = FindPreset(el.Type);
    return el.Mode == Element::Stretch::Both && type && type->Container ? found : entt::null;
}

bool HoldsChildren(const entt::registry& reg, entt::entity e) {
    const Element* el = reg.try_get<Element>(e);
    const Preset* type = el ? FindPreset(el->Type) : nullptr;
    return type && type->Container;
}

bool IsMainContainer(Scene& scene, entt::entity element) {
    return element != entt::null && MainContainer(scene, scene.ParentOf(element)) == element;
}

bool CanReparent(Scene& scene, entt::entity element, entt::entity newParent) {
    entt::registry& reg = scene.Registry();
    if (element == entt::null || !reg.valid(element) || !IsElement(reg, element)) return false;
    // Главный контейнер — область всего интерфейса: внутрь чужого элемента он
    // не кладётся, иначе у интерфейса не осталось бы своей области.
    if (IsMainContainer(scene, element)) return false;
    if (newParent == entt::null) return true;
    if (!reg.valid(newParent) || newParent == element || !IsElement(reg, newParent)) return false;
    // Детей держат только контейнеры (Container, Panel): у кнопки и надписи
    // своих детей нет, и брошенный на них элемент пропал бы из раскладки.
    if (!HoldsChildren(reg, newParent)) return false;
    // Сам себе предок: новый родитель лежит внутри переносимого.
    for (entt::entity a = newParent; a != entt::null && reg.valid(a); a = scene.ParentOf(a))
        if (a == element) return false;
    return InterfaceOf(scene, newParent) == InterfaceOf(scene, element);
}

bool ReparentElement(Scene& scene, entt::entity element, entt::entity newParent) {
    if (!CanReparent(scene, element, newParent)) return false;
    // «В корень» — в главный контейнер интерфейса (у старого вида — под сам
    // интерфейс; если элемент ни в каком — он и так живёт в корне сцены).
    entt::entity target = newParent;
    if (target == entt::null) {
        const entt::entity iface = InterfaceOf(scene, element);
        const entt::entity main = MainContainer(scene, iface);
        target = main != entt::null ? main : iface;
    }
    if (scene.ParentOf(element) == target) return true;
    scene.SetParent(element, target);
    return scene.ParentOf(element) == target;
}

} // namespace sage::ui
