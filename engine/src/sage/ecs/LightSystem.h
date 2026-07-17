#pragma once
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Light.h"
#include "sage/scene/Transform.h"

// LightSystem — сбор итогового освещения кадра. «Система» в терминах ECS:
// берёт окружение сцены (ambient + солнце + scene-level точечные света из
// Scene::Lighting) и ДОБАВЛЯЕТ к нему света-сущности (LightComponent +
// Transform), пока не упрётся в лимит шейдера (MaxPointLights). Результат
// передаётся в UploadLighting как обычный LightingEnvironment — рендеру всё
// равно, откуда пришёл свет: из настроек сцены или из сущности.
namespace sage::ecs {

inline LightingEnvironment CollectLighting(Scene& scene) {
    LightingEnvironment env = scene.Lighting;
    auto view = scene.Registry().view<LightComponent, Transform>();
    for (auto e : view) {
        if ((int)env.PointLights.size() >= LightingEnvironment::MaxPointLights) break;
        const LightComponent& lc = view.get<LightComponent>(e);
        PointLight p;
        p.Position = view.get<Transform>(e).Position;
        p.Color = lc.Color;
        p.Intensity = lc.Intensity;
        p.Range = lc.Range;
        env.PointLights.push_back(p);
    }
    return env;
}

} // namespace sage::ecs
