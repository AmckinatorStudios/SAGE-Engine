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

template <typename Fn>
inline void ForEachRenderable(Scene& scene, Fn&& fn) {
    auto view = scene.Registry().view<Transform, MeshRendererComponent>();
    for (auto entity : view) {
        MeshRendererComponent& mr = view.template get<MeshRendererComponent>(entity);
        if (!mr.MeshPtr) continue; // сущность без назначенного меша не рисуется
        fn(view.template get<Transform>(entity), mr);
    }
}

} // namespace sage::ecs
