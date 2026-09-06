#pragma once
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Transform.h"

// RenderSystem — обход рендерящихся сущностей сцены. Это «система» в терминах
// ECS: она итерирует все сущности, у которых есть И Transform, И MeshRenderer
// (с назначенным GPU-мешем), и отдаёт их вызывающему. Сама она не знает о
// конкретных шейдерах/униформах — как именно рисовать, решает вызывающий код
// (основной проход выставляет цвет объекта, depth-проход теней — только модель).
// Так одна система обслуживает оба прохода без дублирования обхода сцены.
namespace sage::ecs {

// Скрыта ли сущность — сама или любым из предков.
//
// Наследование по цепочке, а не проверка одной метки: спрятать группу и
// увидеть её детей на экране — это сломанное действие, а не особенность.
// Цепочка короткая (глубина иерархии, не число сущностей), и проверка стоит
// ровно столько же, сколько уже стоит подъём за мировой матрицей.
inline bool IsHidden(const entt::registry& reg, entt::entity e) {
    while (e != entt::null && reg.valid(e)) {
        if (reg.all_of<HiddenComponent>(e)) return true;
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (!h) return false;
        e = h->Parent;
    }
    return false;
}

template <typename Fn>
inline void ForEachRenderable(Scene& scene, Fn&& fn) {
    auto view = scene.Registry().view<Transform, MeshRendererComponent>();
    for (auto entity : view) {
        MeshRendererComponent& mr = view.template get<MeshRendererComponent>(entity);
        if (!mr.MeshPtr) continue; // сущность без назначенного меша не рисуется
        if (IsHidden(scene.Registry(), entity)) continue;
        fn(view.template get<Transform>(entity), mr);
    }
}

// Вариант с сущностью — для кода, которому нужна МИРОВАЯ матрица (иерархия):
// fn(entt::entity, Transform&, MeshRendererComponent&). Мировую матрицу
// вызывающий берёт через scene.WorldMatrix(entity).
template <typename Fn>
inline void ForEachRenderableEntity(Scene& scene, Fn&& fn) {
    auto view = scene.Registry().view<Transform, MeshRendererComponent>();
    for (auto entity : view) {
        MeshRendererComponent& mr = view.template get<MeshRendererComponent>(entity);
        if (!mr.MeshPtr) continue;
        if (IsHidden(scene.Registry(), entity)) continue;
        fn(entity, view.template get<Transform>(entity), mr);
    }
}

} // namespace sage::ecs
