#pragma once
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <cmath>

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

// Обратная задача: поворот сущности (градусы, XYZ), при котором её «вперёд»
// смотрит вдоль dir. Нужна там, где направление задано вектором, а хранится
// поворотом, — прежде всего при переносе старого солнца из настроек сцены в
// сущность (миграция v4->v5) и в панели, где солнце крутят направлением.
//
// Крена (Z) у решения нет и быть не может: одному направлению отвечает целое
// семейство поворотов, отличающихся вращением вокруг самого луча, и выбрать из
// них нужно тот, что не заваливает горизонт, — то есть с нулевым креном.
inline glm::vec3 EulerFromForward(const glm::vec3& dir) {
    const float len = glm::length(dir);
    if (len < 1e-6f) return glm::vec3(0.0f);
    const glm::vec3 f = dir / len;
    // ForwardFromEuler даёт f = (-sin y, sin x * cos y, -cos x * cos y).
    // Отсюда: x = atan2(f.y, -f.z), y = atan2(-f.x, sqrt(f.y^2 + f.z^2)).
    const float pitch = std::atan2(f.y, -f.z);
    const float yaw = std::atan2(-f.x, std::sqrt(f.y * f.y + f.z * f.z));
    return glm::degrees(glm::vec3(pitch, yaw, 0.0f));
}

// Какая сущность СЕЙЧАС является солнцем сцены, или entt::null.
//
// Правило одно на весь движок и на редактор: солнце — направленный свет с
// НАИМЕНЬШИМ номером объекта. Пока это правило жило только внутри
// CollectLighting, редактор не мог ни показать «вот это солнце», ни
// предупредить «а этот направленный свет в кадре не участвует» — и человек
// крутил второй источник, глядя на неподвижную картинку.
//
// Порядок обхода ECS-view для выбора не годится: entt его не обещает и меняет
// при удалении сущностей (см. комментарий в CollectLighting).
inline entt::entity FindSunEntity(Scene& scene) {
    entt::entity sun = entt::null;
    int sunId = 0;
    auto view = scene.Registry().view<LightComponent, Transform>();
    for (auto e : view) {
        if (view.get<LightComponent>(e).Kind != LightComponent::Type::Directional) continue;
        const IdComponent* id = scene.Registry().try_get<IdComponent>(e);
        const int candidate = id ? id->Id : 0;
        if (sun != entt::null && candidate >= sunId) continue;
        sun = e;
        sunId = candidate;
    }
    return sun;
}

inline LightingEnvironment CollectLighting(Scene& scene) {
    LightingEnvironment env = scene.Lighting;
    // Направленный свет-сущность ПЕРЕКРЫВАЕТ солнце из настроек сцены, а не
    // складывается с ним. Складывать нельзя: сцена, где солнце перенесли в
    // объект, светила бы вдвое — и объяснить это по картинке невозможно.
    // Основание при этом остаётся: код, собирающий сцену без редактора (игры,
    // превью ассетов, тесты), продолжает ставить свет одной строкой.
    // Какое из направленных светов считать солнцем — вопрос не праздный: их в
    // сцене может быть несколько, а шейдер знает одно (и одну каскадную карту
    // под него). Берётся источник с НАИМЕНЬШИМ id объекта, то есть первый в
    // иерархии редактора.
    //
    // Порядок обхода ECS-view для этого не годится, и это не придирка: entt его
    // не обещает и меняет при удалении сущностей. Сцена выбирала бы солнцем то
    // один свет, то другой — после удаления соседнего объекта, после
    // пересохранения, после загрузки на другой машине. Ошибка, которая
    // «иногда».
    int sunId = 0;
    bool sunTaken = false;
    auto view = scene.Registry().view<LightComponent, Transform>();
    for (auto e : view) {
        const LightComponent& lc = view.get<LightComponent>(e);
        // Мировые позиция/направление (учёт иерархии родителей).
        glm::mat4 world = scene.WorldMatrix(e);
        glm::vec3 wpos = glm::vec3(world[3]);
        glm::vec3 wdir = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        if (lc.Kind == LightComponent::Type::Directional) {
            const IdComponent* id = scene.Registry().try_get<IdComponent>(e);
            const int candidate = id ? id->Id : 0;
            if (sunTaken && candidate >= sunId) continue;
            sunTaken = true;
            sunId = candidate;
            env.Sun.Direction = wdir;
            env.Sun.Color = lc.Color;
            env.Sun.Intensity = lc.Intensity;
            env.Sun.CastShadows = lc.CastShadows;
        } else if (lc.Kind == LightComponent::Type::Spot) {
            if ((int)env.SpotLights.size() >= LightingEnvironment::MaxSpotLights) continue;
            SpotLight s;
            s.Position = wpos;
            s.Direction = wdir;
            s.Color = lc.Color;
            s.Intensity = lc.Intensity;
            s.Range = lc.Range;
            s.InnerAngleDeg = lc.InnerConeDeg;
            s.OuterAngleDeg = lc.OuterConeDeg;
            s.CastShadows = lc.CastShadows;
            env.SpotLights.push_back(s);
        } else {
            if ((int)env.PointLights.size() >= LightingEnvironment::MaxPointLights) continue;
            PointLight p;
            p.Position = wpos;
            p.Color = lc.Color;
            p.Intensity = lc.Intensity;
            p.Range = lc.Range;
            p.CastShadows = lc.CastShadows;
            env.PointLights.push_back(p);
        }
    }
    return env;
}

} // namespace sage::ecs
