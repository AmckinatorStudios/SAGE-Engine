#pragma once
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Light.h"
#include "sage/scene/Transform.h"

// LightSystem — сбор итогового освещения кадра. «Система» в терминах ECS:
// берёт окружение сцены (ambient + солнце + scene-level света из
// Scene::Lighting) и ДОБАВЛЯЕТ к нему света-сущности (LightComponent +
// Transform), раскладывая их по типу: Point -> PointLights, Spot ->
// SpotLights, пока не упрётся в лимит шейдера. Результат передаётся в
// UploadLighting как обычный LightingEnvironment — рендеру всё равно, откуда
// пришёл свет: из настроек сцены или из сущности.
namespace sage::ecs {

// «Вперёд» сущности из углов Эйлера Transform (градусы, порядок XYZ — как в
// Transform::GetMatrix и в камере): локальный -Z, повёрнутый ориентацией.
// Общая точка для направления прожекторов и камеры.
inline glm::vec3 ForwardFromEuler(const glm::vec3& eulerDeg) {
    glm::mat4 rot = glm::eulerAngleXYZ(glm::radians(eulerDeg.x),
                                       glm::radians(eulerDeg.y),
                                       glm::radians(eulerDeg.z));
    return glm::normalize(glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
}

inline LightingEnvironment CollectLighting(Scene& scene) {
    LightingEnvironment env = scene.Lighting;
    auto view = scene.Registry().view<LightComponent, Transform>();
    for (auto e : view) {
        const LightComponent& lc = view.get<LightComponent>(e);
        // Мировые позиция/направление (учёт иерархии родителей).
        glm::mat4 world = scene.WorldMatrix(e);
        glm::vec3 wpos = glm::vec3(world[3]);
        if (lc.Kind == LightComponent::Type::Spot) {
            if ((int)env.SpotLights.size() >= LightingEnvironment::MaxSpotLights) continue;
            SpotLight s;
            s.Position = wpos;
            s.Direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            s.Color = lc.Color;
            s.Intensity = lc.Intensity;
            s.Range = lc.Range;
            s.InnerAngleDeg = lc.InnerConeDeg;
            s.OuterAngleDeg = lc.OuterConeDeg;
            env.SpotLights.push_back(s);
        } else {
            if ((int)env.PointLights.size() >= LightingEnvironment::MaxPointLights) continue;
            PointLight p;
            p.Position = wpos;
            p.Color = lc.Color;
            p.Intensity = lc.Intensity;
            p.Range = lc.Range;
            env.PointLights.push_back(p);
        }
    }
    return env;
}

} // namespace sage::ecs
